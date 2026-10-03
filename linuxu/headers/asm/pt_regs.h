/* linuxu: SHIM (vendor/linux/arch/x86/include/asm/pt_regs.h)
 *
 * The KMD's printk/vprintk path passes a va_list through a
 * (struct pt_regs *) parameter.  The shadow set intentionally has no
 * architecture-specific pt_regs layout (it would collide with the
 * macOS SDK when the dext compiles C++); the kernel-facing ABI only
 * needs *one word* of storage — the va_list itself.
 */
#ifndef __ASM_PT_REGS_H
#define __ASM_PT_REGS_H

struct pt_regs {
	/* ABI: vprintk/vscnprintf receive the va_list here */
	union {
		void *ap; /* = (void *)va_list */
		void *r[1];
	};
};

#endif /* __ASM_PT_REGS_H */
