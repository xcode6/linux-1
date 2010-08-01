/**********************************************************************/
/*   This file contains code for static probes. Under Linux, we take  */
/*   a  different  strategy  to Solaris/Apple - since we dont molest  */
/*   the kernel code directly. (This is still supported - sort of).   */
/*   								      */
/*   Author: Paul D. Fox					      */
/*   $Header: Last edited: 28-Jul-2010 1.1 $ 			      */
/**********************************************************************/

/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

//#pragma ident	"@(#)sdt.c	1.6	06/03/24 SMI"

#include <dtrace_linux.h>
#include <linux/miscdevice.h>
#include <sys/modctl.h>
#include <sys/dtrace.h>
#include <sys/stack.h>
#include <sys/sdt.h>
#include <sys/sdt_impl.h>
#include <sys/dtrace_impl.h>
#include "dtrace_proto.h"

# define regs pt_regs
#include <sys/stack.h>
#include <sys/frame.h>
#include <sys/privregs.h>

/**********************************************************************/
/*   Make the Linux patch an INT3 instruction.			      */
/**********************************************************************/
#if defined(linux)
#  define	SDT_PATCHVAL	0xcc
#else
#  define	SDT_PATCHVAL	0xf0
#endif

#define	SDT_ADDR2NDX(addr)	((((uintptr_t)(addr)) >> 4) & sdt_probetab_mask)
#define	SDT_PROBETAB_SIZE	0x1000		/* 4k entries -- 16K total */

# if defined(sun)
static int			sdt_verbose = 0;
# endif
static sdt_probe_t		**sdt_probetab;
static int			sdt_probetab_size;
static int			sdt_probetab_mask;

/**********************************************************************/
/*   Easy handles to the specific providers we want to process.	      */
/**********************************************************************/
static sdt_provider_t *io_prov;

/**********************************************************************/
/*   Go hunting for the static io:: provider slots.		      */
/**********************************************************************/
static int
io_prov_entry(pf_info_t *infp, uint8_t *instr, int size, int modrm)
{
	sdt_probe_t *sdp;
	sdt_provider_t *prov;
	uint8_t *offset;
	char	*name;

printk("io_prov_entry called %s:%s\n", infp->modname, infp->name);

	for (prov = sdt_providers; prov->sdtp_prefix != NULL; prov++) {
		if (strcmp(prov->sdtp_name, infp->modname) == 0)
			break;
	}
	name = kstrdup(infp->name, KM_SLEEP);
	sdp = kmem_zalloc(sizeof (sdt_probe_t), KM_SLEEP);
	sdp->sdp_id = dtrace_probe_create(prov->sdtp_id,
			    infp->func, NULL, name, 3, sdp);
	sdp->sdp_name = name;
	sdp->sdp_namelen = strlen(name);
	sdp->sdp_inslen = size;
	sdp->sdp_modrm = modrm;
	sdp->sdp_provider = prov;

	/***********************************************/
	/*   Add the entry to the hash table.	       */
	/***********************************************/
	offset = instr;
	sdp->sdp_hashnext =
	    sdt_probetab[SDT_ADDR2NDX(offset)];
	sdt_probetab[SDT_ADDR2NDX(offset)] = sdp;

	sdp->sdp_patchval = SDT_PATCHVAL;
	sdp->sdp_patchpoint = (uint8_t *)offset;
	sdp->sdp_savedval = *sdp->sdp_patchpoint;

	infp->retptr = NULL;
	return 1;
}
static int
io_prov_return(pf_info_t *infp, uint8_t *instr, int size)
{
	sdt_probe_t *sdp;
	sdt_provider_t *prov;
	uint8_t *offset;
	char	*name;
	sdt_probe_t *retsdt = infp->retptr;

printk("io_prov_return called %s:%s %p  sz=%x\n", infp->modname, infp->name, instr, size);

	for (prov = sdt_providers; prov->sdtp_prefix != NULL; prov++) {
		if (strcmp(prov->sdtp_name, infp->modname) == 0)
			break;
	}
	name = kstrdup(infp->name2, KM_SLEEP);
	sdp = kmem_zalloc(sizeof (sdt_probe_t), KM_SLEEP);
	/***********************************************/
	/*   Daisy chain the return exit points so we  */
	/*   dont  end  up firing all of them when we  */
	/*   return from the probe.		       */
	/***********************************************/
	if (retsdt == NULL) {
		sdp->sdp_id = dtrace_probe_create(prov->sdtp_id,
			    infp->func, NULL, name, 3, sdp);
		infp->retptr = sdp;
	} else {
		retsdt->sdp_next = sdp;
		sdp->sdp_id = retsdt->sdp_id;
	}
	sdp->sdp_name = name;
	sdp->sdp_namelen = strlen(name);
	sdp->sdp_inslen = size;
	sdp->sdp_provider = prov;

	/***********************************************/
	/*   Add the entry to the hash table.	       */
	/***********************************************/
	offset = instr;
	sdp->sdp_hashnext =
	    sdt_probetab[SDT_ADDR2NDX(offset)];
	sdt_probetab[SDT_ADDR2NDX(offset)] = sdp;

	sdp->sdp_patchval = SDT_PATCHVAL;
	sdp->sdp_patchpoint = (uint8_t *)offset;
	sdp->sdp_savedval = *sdp->sdp_patchpoint;
	return 1;
}
static void
io_prov_create(char *func, char *name)
{
	pf_info_t	inf;
	uint8_t		*start;
	int		size;

	if (dtrace_function_size(func, &start, &size) == 0) {
		printk("sdt_linux: cannot locate %s\n", func);
		return;
	}

	memset(&inf, 0, sizeof inf);
	inf.modname = "io";
	inf.func = func;
	inf.name = name;
	inf.name2 = "done";

	inf.func_entry = io_prov_entry;
	inf.func_return = io_prov_return;

	dtrace_parse_function(&inf, start, start + size);

}
/**********************************************************************/
/*   Function called from dtrace_linux.c:dtrace_linux_init after the  */
/*   various  symtab hooks are setup so we can find the functions we  */
/*   are after.							      */
/**********************************************************************/
void
io_prov_init(void)
{
	io_prov_create("do_sync_read", "start");
	io_prov_create("do_sync_write", "start");
}

/**********************************************************************/
/*   This is called when we hit an SDT breakpoint.		      */
/**********************************************************************/
typedef struct fileinfo_t {
	char	*fi_name;
	char	*fi_dirname;
	char	*fi_pathname;
	long long fi_offset;
	char *fi_fs;
	char *fi_mount;
} fileinfo_t;

static fileinfo_t finfo;
/*ARGSUSED*/
static int
sdt_invop(uintptr_t addr, uintptr_t *stack, uintptr_t eax, trap_instr_t *tinfo)
{
	uintptr_t stack0, stack1, stack2, stack3, stack4;
	sdt_probe_t *sdt = sdt_probetab[SDT_ADDR2NDX(addr)];
	struct pt_regs *regs;

	for (; sdt != NULL; sdt = sdt->sdp_hashnext) {
//printk("sdt_invop %p %p\n", sdt->sdp_patchpoint, addr);
		if (sdt->sdp_enabled && (uintptr_t)sdt->sdp_patchpoint == addr) {
			tinfo->t_opcode = sdt->sdp_savedval;
			tinfo->t_inslen = sdt->sdp_inslen;
			tinfo->t_modrm = sdt->sdp_modrm;
			/***********************************************/
			/*   Dont fire probe if this is unsafe.	       */
			/***********************************************/
			if (!tinfo->t_doprobe)
				return (DTRACE_INVOP_NOP);
			/*
			 * When accessing the arguments on the stack, we must
			 * protect against accessing beyond the stack.  We can
			 * safely set NOFAULT here -- we know that interrupts
			 * are already disabled.
			 */
			regs = (struct pt_regs *) stack;
			DTRACE_CPUFLAG_SET(CPU_DTRACE_NOFAULT);
			stack0 = regs->c_arg0;
			stack1 = regs->c_arg1;
			stack2 = regs->c_arg2;
{
struct file *file = stack0;
memset(&finfo, 0, sizeof finfo);
stack2 = &finfo;
//printk("stack2=%p\n", stack2);
finfo.fi_offset = 0x12345;
finfo.fi_name = "this/is/fi_pathname";
finfo.fi_pathname = "hello/world";
//printk("fi_name=%p offset=%llx\n", finfo.fi_name, finfo.fi_offset);
}
			stack3 = regs->c_arg3;
			stack4 = regs->c_arg4;
			DTRACE_CPUFLAG_CLEAR(CPU_DTRACE_NOFAULT |
			    CPU_DTRACE_BADADDR);

			dtrace_probe(sdt->sdp_id, stack0, stack1,
			    stack2, stack3, stack4);

			return (DTRACE_INVOP_NOP);
		}
	}
//printk("none in invop for dsdt\n");

	return (0);
}

/*ARGSUSED*/
static void
sdt_provide_module(void *arg, struct modctl *ctl)
{
# if defined(sun)
	struct module *mp = ctl->mod_mp;
	char *modname = ctl->mod_modname;
	sdt_probedesc_t *sdpd;
	sdt_probe_t *sdp, *old;
	sdt_provider_t *prov;

	/*
	 * One for all, and all for one:  if we haven't yet registered all of
	 * our providers, we'll refuse to provide anything.
	 */
	for (prov = sdt_providers; prov->sdtp_name != NULL; prov++) {
		if (prov->sdtp_id == DTRACE_PROVNONE)
			return;
	}

	if (mp->sdt_nprobes != 0 || (sdpd = mp->sdt_probes) == NULL)
		return;

	for (sdpd = mp->sdt_probes; sdpd != NULL; sdpd = sdpd->sdpd_next) {
		char *name = sdpd->sdpd_name, *func, *nname;
		int i, j, len;
		sdt_provider_t *prov;
		ulong_t offs;
		dtrace_id_t id;

		for (prov = sdt_providers; prov->sdtp_prefix != NULL; prov++) {
			char *prefix = prov->sdtp_prefix;

			if (strncmp(name, prefix, strlen(prefix)) == 0) {
				name += strlen(prefix);
				break;
			}
		}

		nname = kmem_alloc(len = strlen(name) + 1, KM_SLEEP);

		for (i = 0, j = 0; name[j] != '\0'; i++) {
			if (name[j] == '_' && name[j + 1] == '_') {
				nname[i] = '-';
				j += 2;
			} else {
				nname[i] = name[j++];
			}
		}

		nname[i] = '\0';

		sdp = kmem_zalloc(sizeof (sdt_probe_t), KM_SLEEP);
		sdp->sdp_loadcnt = ctl->mod_loadcnt;
		sdp->sdp_ctl = ctl;
		sdp->sdp_name = nname;
		sdp->sdp_namelen = len;
		sdp->sdp_provider = prov;

		func = kobj_searchsym(mp, sdpd->sdpd_offset, &offs);

		if (func == NULL)
			func = "<unknown>";

		/*
		 * We have our provider.  Now create the probe.
		 */
		if ((id = dtrace_probe_lookup(prov->sdtp_id, modname,
		    func, nname)) != DTRACE_IDNONE) {
			old = dtrace_probe_arg(prov->sdtp_id, id);
			ASSERT(old != NULL);

			sdp->sdp_next = old->sdp_next;
			sdp->sdp_id = id;
			old->sdp_next = sdp;
		} else {
			sdp->sdp_id = dtrace_probe_create(prov->sdtp_id,
			    modname, func, nname, 3, sdp);

			mp->sdt_nprobes++;
		}

		sdp->sdp_hashnext =
		    sdt_probetab[SDT_ADDR2NDX(sdpd->sdpd_offset)];
		sdt_probetab[SDT_ADDR2NDX(sdpd->sdpd_offset)] = sdp;

		sdp->sdp_patchval = SDT_PATCHVAL;
		sdp->sdp_patchpoint = (uint8_t *)sdpd->sdpd_offset;
		sdp->sdp_savedval = *sdp->sdp_patchpoint;
	}
# endif
}

/*ARGSUSED*/
static void
sdt_destroy(void *arg, dtrace_id_t id, void *parg)
{
	sdt_probe_t *sdp = parg, *old, *last, *hash;
	int ndx;

# if defined(sun)
	struct modctl *ctl = sdp->sdp_ctl;
	if (ctl != NULL && ctl->mod_loadcnt == sdp->sdp_loadcnt) {
		if ((ctl->mod_loadcnt == sdp->sdp_loadcnt &&
		    ctl->mod_loaded)) {
			((struct module *)(ctl->mod_mp))->sdt_nprobes--;
		}
	}
# endif

	while (sdp != NULL) {
		old = sdp;

		/*
		 * Now we need to remove this probe from the sdt_probetab.
		 */
		ndx = SDT_ADDR2NDX(sdp->sdp_patchpoint);
		last = NULL;
		hash = sdt_probetab[ndx];

		while (hash != sdp) {
			ASSERT(hash != NULL);
			last = hash;
			hash = hash->sdp_hashnext;
		}

		if (last != NULL) {
			last->sdp_hashnext = sdp->sdp_hashnext;
		} else {
			sdt_probetab[ndx] = sdp->sdp_hashnext;
		}

		kmem_free(sdp->sdp_name, sdp->sdp_namelen);
		sdp = sdp->sdp_next;
		kmem_free(old, sizeof (sdt_probe_t));
	}
}

/*ARGSUSED*/
static int
sdt_enable(void *arg, dtrace_id_t id, void *parg)
{
	sdt_probe_t *sdp = parg;

# if defined(sun)
	ctl->mod_nenabled++;
	{struct modctl *ctl = sdp->sdp_ctl;
	/*
	 * If this module has disappeared since we discovered its probes,
	 * refuse to enable it.
	 */
	if (!ctl->mod_loaded) {
		if (sdt_verbose) {
			cmn_err(CE_NOTE, "sdt is failing for probe %s "
			    "(module %s unloaded)",
			    sdp->sdp_name, ctl->mod_modname);
		}
		return 0;
	}

	/*
	 * Now check that our modctl has the expected load count.  If it
	 * doesn't, this module must have been unloaded and reloaded -- and
	 * we're not going to touch it.
	 */
	if (ctl->mod_loadcnt != sdp->sdp_loadcnt) {
		if (sdt_verbose) {
			cmn_err(CE_NOTE, "sdt is failing for probe %s "
			    "(module %s reloaded)",
			    sdp->sdp_name, ctl->mod_modname);
		}
		return 0;
	}
	}
# endif

	while (sdp != NULL) {
		/***********************************************/
		/*   Kernel  code  wil be write protected, so  */
		/*   try and unprotect it.		       */
		/***********************************************/
		sdp->sdp_enabled = TRUE;
		if (memory_set_rw(sdp->sdp_patchpoint, 1, TRUE))
			*sdp->sdp_patchpoint = sdp->sdp_patchval;
		sdp = sdp->sdp_next;
	}
	return 0;
}

/*ARGSUSED*/
static void
sdt_disable(void *arg, dtrace_id_t id, void *parg)
{
	sdt_probe_t *sdp = parg;

# if 0
	ctl->mod_nenabled--;
	{
	struct modctl *ctl = sdp->sdp_ctl;
	if (!ctl->mod_loaded || ctl->mod_loadcnt != sdp->sdp_loadcnt)
		return;
	}
# endif

	while (sdp != NULL) {
		sdp->sdp_enabled = FALSE;
		/***********************************************/
		/*   Memory  should  be  writable,  but if we  */
		/*   failed  in  the  sdt_enable  code,  e.g.  */
		/*   because   we  failed  to  unprotect  the  */
		/*   memory  page,  then dont try and unpatch  */
		/*   something we didnt patch.		       */
		/***********************************************/
		if (*sdp->sdp_patchpoint == sdp->sdp_patchval) {
			*sdp->sdp_patchpoint = sdp->sdp_savedval;
		}
		sdp = sdp->sdp_next;
	}
}

/*ARGSUSED*/
uint64_t
sdt_getarg(void *arg, dtrace_id_t id, void *parg, int argno, int aframes)
{
	uintptr_t val;
	struct frame *fp = (struct frame *)dtrace_getfp();
	uintptr_t *stack = NULL;
	int i;
#if defined(__amd64)
	/*
	 * A total of 6 arguments are passed via registers; any argument with
	 * index of 5 or lower is therefore in a register.
	 */
	int inreg = 5;
#endif

	for (i = 1; i <= aframes; i++) {
//printk("i=%d fp=%p aframes=%d pc:%p\n", i, fp, aframes, fp->fr_savpc);
		fp = (struct frame *)(fp->fr_savfp);
#if defined(linux)
		/***********************************************/
		/*   Temporary hack - not sure which stack we  */
		/*   have here and it is faultiing us.	       */
		/***********************************************/
		if (fp == NULL)
			return 0;
#endif

		if (fp->fr_savpc == (pc_t)dtrace_invop_callsite) {
#if !defined(__amd64)
			/*
			 * If we pass through the invalid op handler, we will
			 * use the pointer that it passed to the stack as the
			 * second argument to dtrace_invop() as the pointer to
			 * the stack.
			 */
			stack = ((uintptr_t **)&fp[1])[1];
#else
			/*
			 * In the case of amd64, we will use the pointer to the
			 * regs structure that was pushed when we took the
			 * trap.  To get this structure, we must increment
			 * beyond the frame structure.  If the argument that
			 * we're seeking is passed on the stack, we'll pull
			 * the true stack pointer out of the saved registers
			 * and decrement our argument by the number of
			 * arguments passed in registers; if the argument
			 * we're seeking is passed in regsiters, we can just
			 * load it directly.
			 */
			struct regs *rp = (struct regs *)((uintptr_t)&fp[1] +
			    sizeof (uintptr_t));

			if (argno <= inreg) {
				stack = (uintptr_t *)&rp->r_rdi;
			} else {
				stack = (uintptr_t *)(rp->r_rsp);
				argno -= (inreg + 1);
			}
#endif
			goto load;
		}
	}

	/*
	 * We know that we did not come through a trap to get into
	 * dtrace_probe() -- the provider simply called dtrace_probe()
	 * directly.  As this is the case, we need to shift the argument
	 * that we're looking for:  the probe ID is the first argument to
	 * dtrace_probe(), so the argument n will actually be found where
	 * one would expect to find argument (n + 1).
	 */
	argno++;

#if defined(__amd64)
	if (argno <= inreg) {
		/*
		 * This shouldn't happen.  If the argument is passed in a
		 * register then it should have been, well, passed in a
		 * register...
		 */
		DTRACE_CPUFLAG_SET(CPU_DTRACE_ILLOP);
		return (0);
	}

	argno -= (inreg + 1);
#endif
	stack = (uintptr_t *)&fp[1];

load:
	DTRACE_CPUFLAG_SET(CPU_DTRACE_NOFAULT);
	val = stack[argno];
	DTRACE_CPUFLAG_CLEAR(CPU_DTRACE_NOFAULT);
printk("sdt_getarg#%d: %lx\n", argno, val);

	return (val);
}

static dtrace_pops_t sdt_pops = {
	NULL,
	sdt_provide_module,
	sdt_enable,
	sdt_disable,
	NULL,
	NULL,
	sdt_getargdesc,
	sdt_getarg,
	NULL,
	sdt_destroy
};

/*ARGSUSED*/
static int
sdt_attach(void)
{
	sdt_provider_t *prov;
# if defined(sun)

	if (ddi_create_minor_node(devi, "sdt", S_IFCHR,
	    0, DDI_PSEUDO, NULL) == DDI_FAILURE) {
		cmn_err(CE_NOTE, "/dev/sdt couldn't create minor node");
		ddi_remove_minor_node(devi, NULL);
		return (DDI_FAILURE);
	}

	ddi_report_dev(devi);
	sdt_devi = devi;
# endif

	if (sdt_probetab_size == 0)
		sdt_probetab_size = SDT_PROBETAB_SIZE;

	sdt_probetab_mask = sdt_probetab_size - 1;
	sdt_probetab =
	    kmem_zalloc(sdt_probetab_size * sizeof (sdt_probe_t *), KM_SLEEP);
	dtrace_invop_add(sdt_invop);

	for (prov = sdt_providers; prov->sdtp_name != NULL; prov++) {
		if (dtrace_register(prov->sdtp_name, prov->sdtp_attr,
		    DTRACE_PRIV_KERNEL, NULL,
		    &sdt_pops, prov, &prov->sdtp_id) != 0) {
			cmn_err(CE_WARN, "failed to register sdt provider %s",
			    prov->sdtp_name);
		}

		if (strcmp(prov->sdtp_name, "io") == 0) {
			io_prov = prov;
		}
	}

	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
sdt_detach(void)
{
	sdt_provider_t *prov;

	for (prov = sdt_providers; prov->sdtp_name != NULL; prov++) {
		if (prov->sdtp_id != DTRACE_PROVNONE) {
			if (dtrace_unregister(prov->sdtp_id) != 0)
				return (DDI_FAILURE);

			prov->sdtp_id = DTRACE_PROVNONE;
		}
	}

	dtrace_invop_remove(sdt_invop);
	kmem_free(sdt_probetab, sdt_probetab_size * sizeof (sdt_probe_t *));

	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
sdt_open(struct inode *inode, struct file *file)
{
	return (0);
}

static const struct file_operations sdt_fops = {
        .open = sdt_open,
};

static struct miscdevice sdt_dev = {
        MISC_DYNAMIC_MINOR,
        "sdt",
        &sdt_fops
};

static int initted;

int sdt_init(void)
{	int	ret;

	ret = misc_register(&sdt_dev);
	if (ret) {
		printk(KERN_WARNING "sdt: Unable to register misc device\n");
		return ret;
		}

	sdt_attach();

	dtrace_printf("sdt loaded: /dev/sdt now available\n");

	initted = 1;

	return 0;
}
void sdt_exit(void)
{
	if (initted) {
		sdt_detach();
		misc_deregister(&sdt_dev);
	}

/*	printk(KERN_WARNING "sdt driver unloaded.\n");*/
}

