// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Module strict rwx
 *
 * Copyright (C) 2015 Rusty Russell
 */

#include <linux/module.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>
#include <linux/execmem.h>
#include "internal.h"

static int module_set_memory(const struct module *mod, enum mod_mem_type type,
			     int (*set_memory)(unsigned long start, int num_pages))
{
	const struct module_memory *mod_mem = &mod->mem[type];

	if (!mod_mem->base)
		return 0;

	set_vm_flush_reset_perms(mod_mem->base);
	return set_memory((unsigned long)mod_mem->base, mod_mem->size >> PAGE_SHIFT);
}

/*
 * Since some arches are moving towards PAGE_KERNEL module allocations instead
 * of PAGE_KERNEL_EXEC, keep module_enable_x() independent of
 * CONFIG_STRICT_MODULE_RWX because they are needed regardless of whether we
 * are strict.
 */
int module_enable_text_rox(const struct module *mod)
{
	for_class_mod_mem_type(type, text) {
		const struct module_memory *mem = &mod->mem[type];
		int ret;

		if (mem->is_rox)
			ret = execmem_restore_rox(mem->base, mem->size);
		else if (IS_ENABLED(CONFIG_STRICT_MODULE_RWX))
			ret = module_set_memory(mod, type, set_memory_rox);
		else
			ret = module_set_memory(mod, type, set_memory_x);
		if (ret)
			return ret;
	}
	return 0;
}

int module_enable_rodata_ro(const struct module *mod)
{
	int ret;

	if (!IS_ENABLED(CONFIG_STRICT_MODULE_RWX) || !rodata_enabled)
		return 0;

	ret = module_set_memory(mod, MOD_RODATA, set_memory_ro);
	if (ret)
		return ret;
	ret = module_set_memory(mod, MOD_INIT_RODATA, set_memory_ro);
	if (ret)
		return ret;

	return 0;
}

int module_enable_rodata_ro_after_init(const struct module *mod)
{
	if (!IS_ENABLED(CONFIG_STRICT_MODULE_RWX) || !rodata_enabled)
		return 0;

	return module_set_memory(mod, MOD_RO_AFTER_INIT, set_memory_ro);
}

int module_enable_data_nx(const struct module *mod)
{
	if (!IS_ENABLED(CONFIG_STRICT_MODULE_RWX))
		return 0;

	for_class_mod_mem_type(type, data) {
		int ret = module_set_memory(mod, type, set_memory_nx);

		if (ret)
			return ret;
	}
	return 0;
}

int module_enforce_rwx_sections(const Elf_Ehdr *hdr, const Elf_Shdr *sechdrs,
				const char *secstrings,
				const struct module *mod)
{
	const unsigned long shf_wx = SHF_WRITE | SHF_EXECINSTR;
	int i;

	if (!IS_ENABLED(CONFIG_STRICT_MODULE_RWX))
		return 0;

	for (i = 0; i < hdr->e_shnum; i++) {
		if ((sechdrs[i].sh_flags & shf_wx) == shf_wx) {
			pr_err("%s: section %s (index %d) has invalid WRITE|EXEC flags\n",
			       mod->name, secstrings + sechdrs[i].sh_name, i);
			return -ENOEXEC;
		}
	}

	return 0;
}

static const char *const ro_after_init[] = {
	/*
	 * Section .data..ro_after_init holds data explicitly annotated by
	 * __ro_after_init.
	 */
	".data..ro_after_init",

	/*
	 * Section __jump_table holds data structures that are never modified,
	 * with the exception of entries that refer to code in the __init
	 * section, which are marked as such at module load time.
	 */
	"__jump_table",

#ifdef CONFIG_HAVE_STATIC_CALL_INLINE
	/*
	 * Section .static_call_sites holds data structures that need to be
	 * sorted and processed at module load time but are never modified
	 * afterwards.
	 */
	".static_call_sites",
#endif
};

void module_mark_ro_after_init(const Elf_Ehdr *hdr, Elf_Shdr *sechdrs,
			       const char *secstrings)
{
	int i, j;

	for (i = 1; i < hdr->e_shnum; i++) {
		Elf_Shdr *shdr = &sechdrs[i];

		for (j = 0; j < ARRAY_SIZE(ro_after_init); j++) {
			if (strcmp(secstrings + shdr->sh_name,
				   ro_after_init[j]) == 0) {
				shdr->sh_flags |= SHF_RO_AFTER_INIT;
				break;
			}
		}
	}
}
