// SPDX-License-Identifier: GPL-2.0-only
/*
 * BU27034ANUC ROHM Ambient Light Sensor
 *
 * Copyright (c) 2023, ROHM Semiconductor.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/iio-gts-helper.h>
#include <linux/iio/kfifo_buf.h>

#define BU27034_REG_SYSTEM_CONTROL	0x40
#define BU27034_MASK_SW_RESET		BIT(7)
#define BU27034_MASK_PART_ID		GENMASK(5, 0)
#define BU27034_ID			0x19
#define BU27034_REG_MODE_CONTROL1	0x41
#define BU27034_MASK_MEAS_MODE		GENMASK(2, 0)

#define BU27034_REG_MODE_CONTROL2	0x42
#define BU27034_MASK_D01_GAIN		GENMASK(7, 3)

#define BU27034_REG_MODE_CONTROL3	0x43
#define BU27034_REG_MODE_CONTROL4	0x44
#define BU27034_MASK_MEAS_EN		BIT(0)
#define BU27034_MASK_VALID		BIT(7)
#define BU27034_NUM_HW_DATA_CHANS	2
#define BU27034_REG_DATA0_LO		0x50
#define BU27034_REG_DATA1_LO		0x52
#define BU27034_REG_DATA1_HI		0x53
#define BU27034_REG_MANUFACTURER_ID	0x92
#define BU27034_REG_MAX BU27034_REG_MANUFACTURER_ID

/*
 * The BU27034 does not have interrupt to trigger the data read when a
 * measurement has finished. Hence we poll the VALID bit in a thread. We will
 * try to wake the thread BU27034_MEAS_WAIT_PREMATURE_MS milliseconds before
 * the expected sampling time to prevent the drifting.
 *
 * If we constantly wake up a bit too late we would eventually skip a sample.
 * And because the sleep can't wake up _exactly_ at given time this would be
 * inevitable even if the sensor clock would be perfectly phase-locked to CPU
 * clock - which we can't say is the case.
 *
 * This is still fragile. No matter how big advance do we have, we will still
 * risk of losing a sample because things can in a rainy-day scenario be
 * delayed a lot. Yet, more we reserve the time for polling, more we also lose
 * the performance by spending cycles polling the register. So, selecting this
 * value is a balancing dance between severity of wasting CPU time and severity
 * of losing samples.
 *
 * In most cases losing the samples is not _that_ crucial because light levels
 * tend to change slowly.
 *
 * Other option that was pointed to me would be always sleeping 1/2 of the
 * measurement time, checking the VALID bit and just sleeping again if the bit
 * was not set. That should be pretty tolerant against missing samples due to
 * the scheduling delays while also not wasting much of cycles for polling.
 * Downside is that the time-stamps would be very inaccurate as the wake-up
 * would not really be tied to the sensor toggling the valid bit. This would also
 * result 'jumps' in the time-stamps when the delay drifted so that wake-up was
 * performed during the consecutive wake-ups (Or, when sensor and CPU clocks
 * were very different and scheduling the wake-ups was very close to given
 * timeout - and when the time-outs were very close to the actual sensor
 * sampling, Eg. once in a blue moon, two consecutive time-outs would occur
 * without having a sample ready).
 */
#define BU27034_MEAS_WAIT_PREMATURE_MS	5
#define BU27034_DATA_WAIT_TIME_US	1000
#define BU27034_TOTAL_DATA_WAIT_TIME_US (BU27034_MEAS_WAIT_PREMATURE_MS * 1000)

#define BU27034_RETRY_LIMIT 18

enum {
	BU27034_CHAN_ALS,
	BU27034_CHAN_DATA0,
	BU27034_CHAN_DATA1,
	BU27034_NUM_CHANS
};

static const unsigned long bu27034_scan_masks[] = {
	GENMASK(BU27034_CHAN_DATA1, BU27034_CHAN_DATA0),
	GENMASK(BU27034_CHAN_DATA1, BU27034_CHAN_ALS), 0
};

/*
 * Available scales with gain 1x - 1024x, timings 55, 100, 200, 400 mS
 * Time impacts to gain: 1x, 2x, 4x, 8x.
 *
 * => Max total gain is HWGAIN * gain by integration time (8 * 1024) = 8192
 * if 1x gain is scale 1, scale for 2x gain is 0.5, 4x => 0.25,
 * ... 8192x => 0.0001220703125 => 122070.3125 nanos
 *
 * Using NANO precision for scale, we must use scale 16x corresponding gain 1x
 * to avoid precision loss. (8x would result scale 976 562.5(nanos).
 */
#define BU27034_SCALE_1X	16

/* See the data sheet for the "Gain Setting" table */
#define BU27034_GSEL_1X		0x00 /* 00000 */
#define BU27034_GSEL_4X		0x08 /* 01000 */
#define BU27034_GSEL_32X	0x0b /* 01011 */
#define BU27034_GSEL_256X	0x18 /* 11000 */
#define BU27034_GSEL_512X	0x19 /* 11001 */
#define BU27034_GSEL_1024X	0x1a /* 11010 */

/* Available gain settings */
static const struct iio_gain_sel_pair bu27034_gains[] = {
	GAIN_SCALE_GAIN(1, BU27034_GSEL_1X),
	GAIN_SCALE_GAIN(4, BU27034_GSEL_4X),
	GAIN_SCALE_GAIN(32, BU27034_GSEL_32X),
	GAIN_SCALE_GAIN(256, BU27034_GSEL_256X),
	GAIN_SCALE_GAIN(512, BU27034_GSEL_512X),
	GAIN_SCALE_GAIN(1024, BU27034_GSEL_1024X),
};

/*
 * Measurement modes are 55, 100, 200 and 400 mS modes - which do have direct
 * multiplying impact to the data register values (similar to gain).
 *
 * This means that if meas-mode is changed for example from 400 => 200,
 * the scale is doubled. Eg, time impact to total gain is x1, x2, x4, x8.
 */
#define BU27034_MEAS_MODE_100MS		0
#define BU27034_MEAS_MODE_55MS		1
#define BU27034_MEAS_MODE_200MS		2
#define BU27034_MEAS_MODE_400MS		4

static const struct iio_itime_sel_mul bu27034_itimes[] = {
	GAIN_SCALE_ITIME_US(400000, BU27034_MEAS_MODE_400MS, 8),
	GAIN_SCALE_ITIME_US(200000, BU27034_MEAS_MODE_200MS, 4),
	GAIN_SCALE_ITIME_US(100000, BU27034_MEAS_MODE_100MS, 2),
	GAIN_SCALE_ITIME_US(55000, BU27034_MEAS_MODE_55MS, 1),
};

#define BU27034_CHAN_DATA(_name)					\
{									\
	.type = IIO_INTENSITY,						\
	.channel = BU27034_CHAN_##_name,				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |			\
			      BIT(IIO_CHAN_INFO_SCALE) |		\
			      BIT(IIO_CHAN_INFO_HARDWAREGAIN),		\
	.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),	\
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_INT_TIME),		\
	.info_mask_shared_by_all_available =				\
					BIT(IIO_CHAN_INFO_INT_TIME),	\
	.address = BU27034_REG_##_name##_LO,				\
	.scan_index = BU27034_CHAN_##_name,				\
	.scan_type = {							\
		.sign = 'u',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_LE,					\
	},								\
	.indexed = 1,							\
}

static const struct iio_chan_spec bu27034_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.channel = BU27034_CHAN_ALS,
		.scan_index = BU27034_CHAN_ALS,
		.scan_type = {
			.sign = 'u',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	/*
	 * The BU27034 DATA0 and DATA1 channels are both on the visible light
	 * area (mostly). The data0 sensitivity peaks at 500nm, DATA1 at 600nm.
	 * These wave lengths are cyan(ish) and orange(ish), making these
	 * sub-optiomal candidates for R/G/B standardization. Hence the
	 * colour modifier is omitted.
	 */
	BU27034_CHAN_DATA(DATA0),
	BU27034_CHAN_DATA(DATA1),
	IIO_CHAN_SOFT_TIMESTAMP(4),
};

struct bu27034_data {
	struct regmap *regmap;
	struct device *dev;
	/*
	 * Protect gain and time during scale adjustment and data reading.
	 * Protect measurement enabling/disabling.
	 */
	struct mutex mutex;
	struct iio_gts gts;
	struct task_struct *task;
	__le16 raw[BU27034_NUM_HW_DATA_CHANS];
	struct {
		u32 mlux;
		__le16 channels[BU27034_NUM_HW_DATA_CHANS];
		aligned_s64 ts;
	} scan;
};

static const struct regmap_range bu27034_volatile_ranges[] = {
	{
		.range_min = BU27034_REG_SYSTEM_CONTROL,
		.range_max = BU27034_REG_SYSTEM_CONTROL,
	}, {
		.range_min = BU27034_REG_MODE_CONTROL4,
		.range_max = BU27034_REG_MODE_CONTROL4,
	}, {
		.range_min = BU27034_REG_DATA0_LO,
		.range_max = BU27034_REG_DATA1_HI,
	},
};

static const struct regmap_access_table bu27034_volatile_regs = {
	.yes_ranges = &bu27034_volatile_ranges[0],
	.n_yes_ranges = ARRAY_SIZE(bu27034_volatile_ranges),
};

static const struct regmap_range bu27034_read_only_ranges[] = {
	{
		.range_min = BU27034_REG_DATA0_LO,
		.range_max = BU27034_REG_DATA1_HI,
	}, {
		.range_min = BU27034_REG_MANUFACTURER_ID,
		.range_max = BU27034_REG_MANUFACTURER_ID,
	}
};

static const struct regmap_access_table bu27034_ro_regs = {
	.no_ranges = &bu27034_read_only_ranges[0],
	.n_no_ranges = ARRAY_SIZE(bu27034_read_only_ranges),
};

static const struct regmap_config bu27034_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = BU27034_REG_MAX,
	.cache_type = REGCACHE_RBTREE,
	.volatile_table = &bu27034_volatile_regs,
	.wr_table = &bu27034_ro_regs,
};

struct bu27034_gain_check {
	int old_gain;
	int new_gain;
	int chan;
};

static int bu27034_get_gain_sel(struct bu27034_data *data, int chan)
{
	int reg[] = {
		[BU27034_CHAN_DATA0] = BU27034_REG_MODE_CONTROL2,
		[BU27034_CHAN_DATA1] = BU27034_REG_MODE_CONTROL3,
	};
	int ret, val;

	ret = regmap_read(data->regmap, reg[chan], &val);
	if (ret)
		return ret;

	return FIELD_GET(BU27034_MASK_D01_GAIN, val);
}

static int bu27034_get_gain(struct bu27034_data *data, int chan, int *gain)
{
	int ret, sel;

	ret = bu27034_get_gain_sel(data, chan);
	if (ret < 0)
		return ret;

	sel = ret;

	ret = iio_gts_find_gain_by_sel(&data->gts, sel);
	if (ret < 0) {
		dev_err(data->dev, "chan %u: unknown gain value 0x%x\n", chan,
			sel);

		return ret;
	}

	*gain = ret;

	return 0;
}

static int bu27034_get_int_time(struct bu27034_data *data)
{
	int ret, sel;

	ret = regmap_read(data->regmap, BU27034_REG_MODE_CONTROL1, &sel);
	if (ret)
		return ret;

	return iio_gts_find_int_time_by_sel(&data->gts,
					    sel & BU27034_MASK_MEAS_MODE);
}

static int _bu27034_get_scale(struct bu27034_data *data, int channel, int *val,
			      int *val2)
{
	int gain, ret;

	ret = bu27034_get_gain(data, channel, &gain);
	if (ret)
		return ret;

	ret = bu27034_get_int_time(data);
	if (ret < 0)
		return ret;

	return iio_gts_get_scale(&data->gts, gain, ret, val, val2);
}

static int bu27034_get_scale(struct bu27034_data *data, int channel, int *val,
			      int *val2)
{
	int ret;

	if (channel == BU27034_CHAN_ALS) {
		*val = 0;
		*val2 = 1000;
		return IIO_VAL_INT_PLUS_MICRO;
	}

	mutex_lock(&data->mutex);
	ret = _bu27034_get_scale(data, channel, val, val2);
	mutex_unlock(&data->mutex);
	if (ret)
		return ret;

	return IIO_VAL_INT_PLUS_NANO;
}

/* Caller should hold the lock to protect lux reading */
static int bu27034_write_gain_sel(struct bu27034_data *data, int chan, int sel)
{
	static const int reg[] = {
		[BU27034_CHAN_DATA0] = BU27034_REG_MODE_CONTROL2,
		[BU27034_CHAN_DATA1] = BU27034_REG_MODE_CONTROL3,
	};
	int mask, val;

	val = FIELD_PREP(BU27034_MASK_D01_GAIN, sel);
	mask = BU27034_MASK_D01_GAIN;

	return regmap_update_bits(data->regmap, reg[chan], mask, val);
}

static int bu27034_set_gain(struct bu27034_data *data, int chan, int gain)
{
	int ret;

	ret = iio_gts_find_sel_by_gain(&data->gts, gain);
	if (ret < 0)
		return ret;

	return bu27034_write_gain_sel(data, chan, ret);
}

/* Caller should hold the lock to protect data->int_time */
static int bu27034_set_int_time(struct bu27034_data *data, int time)
{
	int ret;

	ret = iio_gts_find_sel_by_int_time(&data->gts, time);
	if (ret < 0)
		return ret;

	return regmap_update_bits(data->regmap, BU27034_REG_MODE_CONTROL1,
				 BU27034_MASK_MEAS_MODE, ret);
}

/*
 * We try to change the time in such way that the scale is maintained for
 * given channels by adjusting gain so that it compensates the time change.
 */
static int bu27034_try_set_int_time(struct bu27034_data *data, int time_us)
{
	struct bu27034_gain_check gains[] = {
		{ .chan = BU27034_CHAN_DATA0 },
		{ .chan = BU27034_CHAN_DATA1 },
	};
	int numg = ARRAY_SIZE(gains);
	int ret, int_time_old, i;

	guard(mutex)(&data->mutex);
	ret = bu27034_get_int_time(data);
	if (ret < 0)
		return ret;

	int_time_old = ret;

	if (!iio_gts_valid_time(&data->gts, time_us)) {
		dev_err(data->dev, "Unsupported integration time %u\n",
			time_us);
		return -EINVAL;
	}

	if (time_us == int_time_old)
		return 0;

	for (i = 0; i < numg; i++) {
		ret = bu27034_get_gain(data, gains[i].chan, &gains[i].old_gain);
		if (ret)
			return 0;

		ret = iio_gts_find_new_gain_by_old_gain_time(&data->gts,
							     gains[i].old_gain,
							     int_time_old, time_us,
							     &gains[i].new_gain);
		if (ret) {
			int scale1, scale2;
			bool ok;

			_bu27034_get_scale(data, gains[i].chan, &scale1, &scale2);
			dev_dbg(data->dev,
				"chan %u, can't support time %u with scale %u %u\n",
				gains[i].chan, time_us, scale1, scale2);

			if (gains[i].new_gain < 0)
				return ret;

			/*
			 * If caller requests for integration time change and we
			 * can't support the scale - then the caller should be
			 * prepared to 'pick up the pieces and deal with the
			 * fact that the scale changed'.
			 */
			ret = iio_find_closest_gain_low(&data->gts,
							gains[i].new_gain, &ok);

			if (!ok)
				dev_dbg(data->dev,
					"optimal gain out of range for chan %u\n",
					gains[i].chan);

			if (ret < 0) {
				dev_dbg(data->dev,
					 "Total gain increase. Risk of saturation");
				ret = iio_gts_get_min_gain(&data->gts);
				if (ret < 0)
					return ret;
			}
			dev_dbg(data->dev, "chan %u scale changed\n",
				 gains[i].chan);
			gains[i].new_gain = ret;
			dev_dbg(data->dev, "chan %u new gain %u\n",
				gains[i].chan, gains[i].new_gain);
		}
	}

	for (i = 0; i < numg; i++) {
		ret = bu27034_set_gain(data, gains[i].chan, gains[i].new_gain);
		if (ret)
			return ret;
	}

	return bu27034_set_int_time(data, time_us);
}

static int bu27034_set_scale(struct bu27034_data *data, int chan,
			    int val, int val2)
{
	int ret, time_sel, gain_sel, i;
	bool found = false;

	if (chan == BU27034_CHAN_ALS) {
		if (val == 0 && val2 == 1000000)
			return 0;

		return -EINVAL;
	}

	guard(mutex)(&data->mutex);
	ret = regmap_read(data->regmap, BU27034_REG_MODE_CONTROL1, &time_sel);
	if (ret)
		return ret;

	ret = iio_gts_find_gain_sel_for_scale_using_time(&data->gts, time_sel,
						val, val2, &gain_sel);
	if (ret) {
		/*
		 * Could not support scale with given time. Need to change time.
		 * We still want to maintain the scale for all channels
		 */
		struct bu27034_gain_check gain;
		int new_time_sel;

		/*
		 * Populate information for the other channel which should also
		 * maintain the scale.
		 */
		if (chan == BU27034_CHAN_DATA0)
			gain.chan = BU27034_CHAN_DATA1;
		else if (chan == BU27034_CHAN_DATA1)
			gain.chan = BU27034_CHAN_DATA0;

		ret = bu27034_get_gain(data, gain.chan, &gain.old_gain);
		if (ret)
			return ret;

		/*
		 * Iterate through all the times to see if we find one which
		 * can support requested scale for requested channel, while
		 * maintaining the scale for the other channel
		 */
		for (i = 0; i < data->gts.num_itime; i++) {
			new_time_sel = data->gts.itime_table[i].sel;

			if (new_time_sel == time_sel)
				continue;

			/* Can we provide requested scale with this time? */
			ret = iio_gts_find_gain_sel_for_scale_using_time(
				&data->gts, new_time_sel, val, val2,
				&gain_sel);
			if (ret)
				continue;

			/* Can the other channel maintain scale? */
			ret = iio_gts_find_new_gain_sel_by_old_gain_time(
				&data->gts, gain.old_gain, time_sel,
				new_time_sel, &gain.new_gain);
			if (!ret) {
				/* Yes - we found suitable time */
				found = true;
				break;
			}
		}
		if (!found) {
			dev_dbg(data->dev,
				"Can't set scale maintaining other channel\n");
			return -EINVAL;
		}

		ret = bu27034_set_gain(data, gain.chan, gain.new_gain);
		if (ret)
			return ret;

		ret = regmap_update_bits(data->regmap, BU27034_REG_MODE_CONTROL1,
				  BU27034_MASK_MEAS_MODE, new_time_sel);
		if (ret)
			return ret;
	}

	return bu27034_write_gain_sel(data, chan, gain_sel);
}

/*
 * for (D1/D0 < 1.5):
 *    lx = (0.001193 * D0 + (-0.0000747) * D1) * ((D1/D0 – 1.5) * (0.25) + 1)
 *
 *    => -0.000745625 * D0 + 0.0002515625 * D1 + -0.000018675 * D1 * D1 / D0
 *
 *    => (6.44 * ch1 / gain1 + 19.088 * ch0 / gain0 -
 *       0.47808 * ch1 * ch1 * gain0 / gain1 / gain1 / ch0) /
 *       mt
 *
 * Else
 *    lx = 0.001193 * D0 - 0.0000747 * D1
 *
 *    => (1.91232 * ch1 / gain1 + 30.5408 * ch0 / gain0 +
 *        [0 * ch1 * ch1 * gain0 / gain1 / gain1 / ch0] ) /
 *        mt
 *
 * This can be unified to format:
 * lx = [
 *	 A * ch1 * ch1 * gain0 / (ch0 * gain1 * gain1) +
 *	 B * ch1 / gain1 +
 *	 C * ch0 / gain0
 *	] / mt
 *
 * For case 1:
 * A = -0.47808,
 * B = 6.44,
 * C = 19.088
 *
 * For case 2:
 * A = 0
 * B = 1.91232
 * C = 30.5408
 */

struct bu27034_lx_coeff {
	unsigned int A;
	unsigned int B;
	unsigned int C;
	/* Indicate which of the coefficients above are negative */
	bool is_neg[3];
};

static inline u64 gain_mul_div_helper(u64 val, unsigned int gain,
				      unsigned int div)
{
	/*
	 * Max gain for a channel is 4096. The max u64 (0xffffffffffffffffULL)
	 * divided by 4096 is 0xFFFFFFFFFFFFF (GENMASK_ULL(51, 0)) (floored).
	 * Thus, the 0xFFFFFFFFFFFFF is the largest value we can safely multiply
	 * with the gain, no matter what gain is set.
	 *
	 * So, multiplication with max gain may overflow if val is greater than
	 * 0xFFFFFFFFFFFFF (52 bits set)..
	 *
	 * If this is the case we divide first.
	 */
	if (val < GENMASK_ULL(51, 0)) {
		val *= gain;
		do_div(val, div);
	} else {
		do_div(val, div);
		val *= gain;
	}

	return val;
}

static u64 bu27034_fixp_calc_t1_64bit(unsigned int coeff, unsigned int ch0,
				      unsigned int ch1, unsigned int gain0,
				      unsigned int gain1)
{
	unsigned int helper;
	u64 helper64;

	helper64 = (u64)coeff * (u64)ch1 * (u64)ch1;

	helper = gain1 * gain1;
	if (helper > ch0) {
		do_div(helper64, helper);

		return gain_mul_div_helper(helper64, gain0, ch0);
	}

	do_div(helper64, ch0);

	return gain_mul_div_helper(helper64, gain0, helper);

}

static u64 bu27034_fixp_calc_t1(unsigned int coeff, unsigned int ch0,
				unsigned int ch1, unsigned int gain0,
				unsigned int gain1)
{
	unsigned int helper, tmp;

	/*
	 * Here we could overflow even the 64bit value. Hence we
	 * multiply with gain0 only after the divisions - even though
	 * it may result loss of accuracy
	 */
	helper = coeff * ch1 * ch1;
	tmp = helper * gain0;

	helper = ch1 * ch1;

	if (check_mul_overflow(helper, coeff, &helper))
		return bu27034_fixp_calc_t1_64bit(coeff, ch0, ch1, gain0, gain1);

	if (check_mul_overflow(helper, gain0, &tmp))
		return bu27034_fixp_calc_t1_64bit(coeff, ch0, ch1, gain0, gain1);

	return tmp / (gain1 * gain1) / ch0;

}

static u64 bu27034_fixp_calc_t23(unsigned int coeff, unsigned int ch,
				 unsigned int gain)
{
	unsigned int helper;
	u64 helper64;

	if (!check_mul_overflow(coeff, ch, &helper))
		return helper / gain;

	helper64 = (u64)coeff * (u64)ch;
	do_div(helper64, gain);

	return helper64;
}

static int bu27034_fixp_calc_lx(unsigned int ch0, unsigned int ch1,
				unsigned int gain0, unsigned int gain1,
				unsigned int meastime, int coeff_idx)
{
	static const struct bu27034_lx_coeff coeff[] = {
		{
			.A = 4780800,		/* -0.47808 */
			.B = 64400000,		/* 6.44 */
			.C = 190880000,		/* 19.088 */
			.is_neg = { true, false, false },
		}, {
			.A = 0,			/* 0 */
			.B = 19123200,		/* 1.91232 */
			.C = 305408000,		/* 30.5408 */
			/* All terms positive */
		},
	};
	const struct bu27034_lx_coeff *c = &coeff[coeff_idx];
	u64 res = 0, terms[3];
	int i;

	if (coeff_idx >= ARRAY_SIZE(coeff))
		return -EINVAL;

	terms[0] = bu27034_fixp_calc_t1(c->A, ch0, ch1, gain0, gain1);
	terms[1] = bu27034_fixp_calc_t23(c->B, ch1, gain1);
	terms[2] = bu27034_fixp_calc_t23(c->C, ch0, gain0);

	/* First, add positive terms */
	for (i = 0; i < 3; i++)
		if (!c->is_neg[i])
			res += terms[i];

	/* No positive term => zero lux */
	if (!res)
		return 0;

	/* Then, subtract negative terms (if any) */
	for (i = 0; i < 3; i++)
		if (c->is_neg[i]) {
			/*
			 * If the negative term is greater than positive - then
			 * the darkness has taken over and we are all doomed! Eh,
			 * I mean, then we can just return 0 lx and go out
			 */
			if (terms[i] >= res)
				return 0;

			res -= terms[i];
		}

	meastime *= 10;
	do_div(res, meastime);

	return (int) res;
}

static bool bu27034_has_valid_sample(struct bu27034_data *data)
{
	int ret, val;

	ret = regmap_read(data->regmap, BU27034_REG_MODE_CONTROL4, &val);
	if (ret) {
		dev_err(data->dev, "Read failed %d\n", ret);

		return false;
	}

	return val & BU27034_MASK_VALID;
}

/*
 * Reading the register where VALID bit is clears this bit. (So does changing
 * any gain / integration time configuration registers) The bit gets
 * set when we have acquired new data. We use this bit to indicate data
 * validity.
 */
static void bu27034_invalidate_read_data(struct bu27034_data *data)
{
	bu27034_has_valid_sample(data);
}

static int bu27034_read_result(struct bu27034_data *data, int chan, int *res)
{
	int reg[] = {
		[BU27034_CHAN_DATA0] = BU27034_REG_DATA0_LO,
		[BU27034_CHAN_DATA1] = BU27034_REG_DATA1_LO,
	};
	int valid, ret;
	__le16 val;

	ret = regmap_read_poll_timeout(data->regmap, BU27034_REG_MODE_CONTROL4,
				       valid, (valid & BU27034_MASK_VALID),
				       BU27034_DATA_WAIT_TIME_US, 0);
	if (ret)
		return ret;

	ret = regmap_bulk_read(data->regmap, reg[chan], &val, sizeof(val));
	if (ret)
		return ret;

	*res = le16_to_cpu(val);

	return 0;
}

static int bu27034_get_result_unlocked(struct bu27034_data *data, __le16 *res,
				       int size)
{
	int ret = 0, retry_cnt = 0;

retry:
	/* Get new value from sensor if data is ready */
	if (bu27034_has_valid_sample(data)) {
		ret = regmap_bulk_read(data->regmap, BU27034_REG_DATA0_LO,
				       res, size);
		if (ret)
			return ret;

		bu27034_invalidate_read_data(data);
	} else {
		/* No new data in sensor. Wait and retry */
		retry_cnt++;

		if (retry_cnt > BU27034_RETRY_LIMIT) {
			dev_err(data->dev, "No data from sensor\n");

			return -ETIMEDOUT;
		}

		msleep(25);

		goto retry;
	}

	return ret;
}

static int bu27034_meas_set(struct bu27034_data *data, bool en)
{
	if (en)
		return regmap_set_bits(data->regmap, BU27034_REG_MODE_CONTROL4,
				       BU27034_MASK_MEAS_EN);

	return regmap_clear_bits(data->regmap, BU27034_REG_MODE_CONTROL4,
				 BU27034_MASK_MEAS_EN);
}

static int bu27034_get_single_result(struct bu27034_data *data, int chan,
				     int *val)
{
	int ret;

	if (chan < BU27034_CHAN_DATA0 || chan > BU27034_CHAN_DATA1)
		return -EINVAL;

	ret = bu27034_meas_set(data, true);
	if (ret)
		return ret;

	ret = bu27034_get_int_time(data);
	if (ret < 0)
		return ret;

	msleep(ret / 1000);

	return bu27034_read_result(data, chan, val);
}

/*
 * The formula given by vendor for computing luxes out of data0 and data1
 * (in open air) is as follows:
 *
 * Let's mark:
 * D0 = data0/ch0_gain/meas_time_ms * 25600
 * D1 = data1/ch1_gain/meas_time_ms * 25600
 *
 * Then:
 * If (D1/D0 < 1.5)
 *    lx = (0.001193 * D0 + (-0.0000747) * D1) * ((D1 / D0 – 1.5) * 0.25 + 1)
 * Else
 *    lx = (0.001193 * D0 + (-0.0000747) * D1)
 *
 * We use it here. Users who have for example some colored lens
 * need to modify the calculation but I hope this gives a starting point for
 * those working with such devices.
 */

static int bu27034_calc_mlux(struct bu27034_data *data, __le16 *res, int *val)
{
	unsigned int gain0, gain1, meastime;
	unsigned int d1_d0_ratio_scaled;
	u16 ch0, ch1;
	u64 helper64;
	int ret;

	/*
	 * We return 0 lux if calculation fails. This should be reasonably
	 * easy to spot from the buffers especially if raw-data channels show
	 * valid values
	 */
	*val = 0;

	ch0 = max_t(u16, 1, le16_to_cpu(res[0]));
	ch1 = max_t(u16, 1, le16_to_cpu(res[1]));

	ret = bu27034_get_gain(data, BU27034_CHAN_DATA0, &gain0);
	if (ret)
		return ret;

	ret = bu27034_get_gain(data, BU27034_CHAN_DATA1, &gain1);
	if (ret)
		return ret;

	ret = bu27034_get_int_time(data);
	if (ret < 0)
		return ret;

	meastime = ret;

	d1_d0_ratio_scaled = (unsigned int)ch1 * (unsigned int)gain0 * 100;
	helper64 = (u64)ch1 * (u64)gain0 * 100LLU;

	if (helper64 != d1_d0_ratio_scaled) {
		unsigned int div = (unsigned int)ch0 * gain1;

		do_div(helper64, div);
		d1_d0_ratio_scaled = helper64;
	} else {
		d1_d0_ratio_scaled /= ch0 * gain1;
	}

	if (d1_d0_ratio_scaled < 150)
		ret = bu27034_fixp_calc_lx(ch0, ch1, gain0, gain1, meastime, 0);
	else
		ret = bu27034_fixp_calc_lx(ch0, ch1, gain0, gain1, meastime, 1);

	if (ret < 0)
		return ret;

	*val = ret;

	return 0;

}

static int bu27034_get_mlux(struct bu27034_data *data, int chan, int *val)
{
	__le16 res[BU27034_NUM_HW_DATA_CHANS];
	int ret;

	ret = bu27034_meas_set(data, true);
	if (ret)
		return ret;

	ret = bu27034_get_result_unlocked(data, &res[0], sizeof(res));
	if (ret)
		return ret;

	ret = bu27034_calc_mlux(data, res, val);
	if (ret)
		return ret;

	ret = bu27034_meas_set(data, false);
	if (ret)
		dev_err(data->dev, "failed to disable measurement\n");

	return 0;
}

static int bu27034_read_raw(struct iio_dev *idev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct bu27034_data *data = iio_priv(idev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		*val = 0;
		*val2 = bu27034_get_int_time(data);
		if (*val2 < 0)
			return *val2;

		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_HARDWAREGAIN:
		ret = bu27034_get_gain(data, chan->channel, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		return bu27034_get_scale(data, chan->channel, val, val2);

	case IIO_CHAN_INFO_RAW:
	{
		int (*result_get)(struct bu27034_data *data, int chan, int *val);

		if (chan->type == IIO_INTENSITY)
			result_get = bu27034_get_single_result;
		else if (chan->type == IIO_LIGHT)
			result_get = bu27034_get_mlux;
		else
			return -EINVAL;

		/* Don't mess with measurement enabling while buffering */
		if (!iio_device_claim_direct(idev))
			return -EBUSY;

		mutex_lock(&data->mutex);
		/*
		 * Reading one channel at a time is inefficient but we
		 * don't care here. Buffered version should be used if
		 * performance is an issue.
		 */
		ret = result_get(data, chan->channel, val);

		mutex_unlock(&data->mutex);
		iio_device_release_direct(idev);

		if (ret)
			return ret;

		return IIO_VAL_INT;
	}
	default:
		return -EINVAL;
	}
}

static int bu27034_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     long mask)
{
	struct bu27034_data *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_INT_TIME:
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_HARDWAREGAIN:
		dev_dbg(data->dev,
			"HARDWAREGAIN is read-only, use scale to set\n");
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static int bu27034_write_raw(struct iio_dev *idev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct bu27034_data *data = iio_priv(idev);
	int ret;

	if (!iio_device_claim_direct(idev))
		return -EBUSY;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		ret = bu27034_set_scale(data, chan->channel, val, val2);
		break;
	case IIO_CHAN_INFO_INT_TIME:
		if (!val)
			ret = bu27034_try_set_int_time(data, val2);
		else
			ret = -EINVAL;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	iio_device_release_direct(idev);

	return ret;
}

static int bu27034_read_avail(struct iio_dev *idev,
			      struct iio_chan_spec const *chan, const int **vals,
			      int *type, int *length, long mask)
{
	struct bu27034_data *data = iio_priv(idev);

	switch (mask) {
	case IIO_CHAN_INFO_INT_TIME:
		return iio_gts_avail_times(&data->gts, vals, type, length);
	case IIO_CHAN_INFO_SCALE:
		return iio_gts_all_avail_scales(&data->gts, vals, type, length);
	default:
		return -EINVAL;
	}
}

static const struct iio_info bu27034_info = {
	.read_raw = &bu27034_read_raw,
	.write_raw = &bu27034_write_raw,
	.write_raw_get_fmt = &bu27034_write_raw_get_fmt,
	.read_avail = &bu27034_read_avail,
};

static int bu27034_chip_init(struct bu27034_data *data)
{
	int ret, sel;

	/* Reset */
	ret = regmap_write_bits(data->regmap, BU27034_REG_SYSTEM_CONTROL,
			   BU27034_MASK_SW_RESET, BU27034_MASK_SW_RESET);
	if (ret)
		return dev_err_probe(data->dev, ret, "Sensor reset failed\n");

	msleep(1);

	ret = regmap_reinit_cache(data->regmap, &bu27034_regmap);
	if (ret) {
		dev_err(data->dev, "Failed to reinit reg cache\n");
		return ret;
	}

	/*
	 * Read integration time here to ensure it is in regmap cache. We do
	 * this to speed-up the int-time acquisition in the start of the buffer
	 * handling thread where longer delays could make it more likely we end
	 * up skipping a sample, and where the longer delays make timestamps
	 * less accurate.
	 */
	ret = regmap_read(data->regmap, BU27034_REG_MODE_CONTROL1, &sel);
	if (ret)
		dev_err(data->dev, "reading integration time failed\n");

	return 0;
}

static int bu27034_wait_for_data(struct bu27034_data *data)
{
	int ret, val;

	ret = regmap_read_poll_timeout(data->regmap, BU27034_REG_MODE_CONTROL4,
				       val, val & BU27034_MASK_VALID,
				       BU27034_DATA_WAIT_TIME_US,
				       BU27034_TOTAL_DATA_WAIT_TIME_US);
	if (ret) {
		dev_err(data->dev, "data polling %s\n",
			!(val & BU27034_MASK_VALID) ? "timeout" : "fail");

		return ret;
	}

	ret = regmap_bulk_read(data->regmap, BU27034_REG_DATA0_LO,
			       &data->scan.channels[0],
			       sizeof(data->scan.channels));
	if (ret)
		return ret;

	bu27034_invalidate_read_data(data);

	return 0;
}

static int bu27034_buffer_thread(void *arg)
{
	struct iio_dev *idev = arg;
	struct bu27034_data *data;
	int wait_ms;

	data = iio_priv(idev);

	wait_ms = bu27034_get_int_time(data);
	wait_ms /= 1000;

	wait_ms -= BU27034_MEAS_WAIT_PREMATURE_MS;

	while (!kthread_should_stop()) {
		int ret;
		int64_t tstamp;

		msleep(wait_ms);
		ret = bu27034_wait_for_data(data);
		if (ret)
			continue;

		tstamp = iio_get_time_ns(idev);

		if (test_bit(BU27034_CHAN_ALS, idev->active_scan_mask)) {
			int mlux;

			ret = bu27034_calc_mlux(data, &data->scan.channels[0],
					       &mlux);
			if (ret)
				dev_err(data->dev, "failed to calculate lux\n");

			/*
			 * The maximum Milli lux value we get with gain 1x time
			 * 55mS data ch0 = 0xffff ch1 = 0xffff fits in 26 bits
			 * so there should be no problem returning int from
			 * computations and casting it to u32
			 */
			data->scan.mlux = (u32)mlux;
		}
		iio_push_to_buffers_with_timestamp(idev, &data->scan, tstamp);
	}

	return 0;
}

static int bu27034_buffer_enable(struct iio_dev *idev)
{
	struct bu27034_data *data = iio_priv(idev);
	struct task_struct *task;
	int ret;

	guard(mutex)(&data->mutex);
	ret = bu27034_meas_set(data, true);
	if (ret)
		return ret;

	task = kthread_run(bu27034_buffer_thread, idev,
				 "bu27034-buffering-%u",
				 iio_device_id(idev));
	if (IS_ERR(task))
		return PTR_ERR(task);

	data->task = task;

	return 0;
}

static int bu27034_buffer_disable(struct iio_dev *idev)
{
	struct bu27034_data *data = iio_priv(idev);

	guard(mutex)(&data->mutex);
	if (data->task) {
		kthread_stop(data->task);
		data->task = NULL;
	}

	return bu27034_meas_set(data, false);
}

static const struct iio_buffer_setup_ops bu27034_buffer_ops = {
	.postenable = &bu27034_buffer_enable,
	.predisable = &bu27034_buffer_disable,
};

static int bu27034_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct bu27034_data *data;
	struct regmap *regmap;
	struct iio_dev *idev;
	unsigned int part_id, reg;
	int ret;

	regmap = devm_regmap_init_i2c(i2c, &bu27034_regmap);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to initialize Regmap\n");

	idev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!idev)
		return -ENOMEM;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get regulator\n");

	data = iio_priv(idev);

	ret = regmap_read(regmap, BU27034_REG_SYSTEM_CONTROL, &reg);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to access sensor\n");

	part_id = FIELD_GET(BU27034_MASK_PART_ID, reg);

	if (part_id != BU27034_ID)
		dev_warn(dev, "unknown device 0x%x\n", part_id);

	ret = devm_iio_init_iio_gts(dev, BU27034_SCALE_1X, 0, bu27034_gains,
				    ARRAY_SIZE(bu27034_gains), bu27034_itimes,
				    ARRAY_SIZE(bu27034_itimes), &data->gts);
	if (ret)
		return ret;

	mutex_init(&data->mutex);
	data->regmap = regmap;
	data->dev = dev;

	idev->channels = bu27034_channels;
	idev->num_channels = ARRAY_SIZE(bu27034_channels);
	idev->name = "bu27034";
	idev->info = &bu27034_info;

	idev->modes = INDIO_DIRECT_MODE | INDIO_BUFFER_SOFTWARE;
	idev->available_scan_masks = bu27034_scan_masks;

	ret = bu27034_chip_init(data);
	if (ret)
		return ret;

	ret = devm_iio_kfifo_buffer_setup(dev, idev, &bu27034_buffer_ops);
	if (ret)
		return dev_err_probe(dev, ret, "buffer setup failed\n");

	ret = devm_iio_device_register(dev, idev);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Unable to register iio device\n");

	return ret;
}

static const struct of_device_id bu27034_of_match[] = {
	{ .compatible = "rohm,bu27034anuc" },
	{ }
};
MODULE_DEVICE_TABLE(of, bu27034_of_match);

static struct i2c_driver bu27034_i2c_driver = {
	.driver = {
		.name = "bu27034-als",
		.of_match_table = bu27034_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = bu27034_probe,
};
module_i2c_driver(bu27034_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matti Vaittinen <matti.vaittinen@fi.rohmeurope.com>");
MODULE_DESCRIPTION("ROHM BU27034 ambient light sensor driver");
MODULE_IMPORT_NS("IIO_GTS_HELPER");
