/* gx: UML's init.h, for its __uml_setup(): the hostfs= option of UML's
 * command line, which LKL has no use for (the host's root is given to mount). */
#ifndef GX_HOSTFS_INIT_H
#define GX_HOSTFS_INIT_H

#define __uml_setup(str, fn, help...) \
	static int (*const __gx_unused_##fn)(char *, int *) __maybe_unused = fn

#endif
