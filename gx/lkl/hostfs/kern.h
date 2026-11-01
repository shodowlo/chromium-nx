/* gx: UML's kern.h, of which hostfs only needs printf (its option parser). */
#ifndef GX_HOSTFS_KERN_H
#define GX_HOSTFS_KERN_H

#include <linux/printk.h>
#define printf printk

#endif
