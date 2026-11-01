/*
 * update - new versions of Chromium NX from its releases on GitHub. See
 * update.c.
 */
#ifndef GX_UPDATE_H
#define GX_UPDATE_H

#include <stdint.h>

#define UPDATE_REPO "shodowlo/chromium-nx"
#define UPDATE_IMAGE "chromium-full.ext4"
#define UPDATE_PROGRAM "chromium-nx.nro"

/* The image next to the program, against what a release has. */
enum {
	IMAGE_UNKNOWN,   /* not known yet, or no release says */
	IMAGE_OK,
	IMAGE_MISSING,
	IMAGE_OTHER,     /* not the one of this version's release */
};

enum {
	UPDATE_IDLE,
	UPDATE_CHECKING,
	UPDATE_CHECKED,      /* what is below is known */
	UPDATE_DOWNLOADING,
	UPDATE_INSTALLED,    /* the program was replaced: to be started again */
	UPDATE_IMAGE_DONE,   /* only the image was: nothing to start again */
	UPDATE_FAILED,       /* error says why */
};

struct update_status {
	int state;
	int newer;               /* a release newer than this program */
	char latest[16];         /* its version */
	uint64_t update_bytes;   /* what updating to it downloads */
	int image;               /* IMAGE_* for this program's version */
	uint64_t image_bytes;    /* the size of this version's image, if known */
	/* While downloading: */
	char file[64];
	uint64_t done, total;    /* bytes of everything to download */
	char error[160];
};

/* Looks for a newer release and at the image, on a thread of its own. */
void update_check(void);
/* A copy of where things are. */
void update_status(struct update_status *out);
/* Downloads and installs: the newest release (everything that changed), or
 * only this version's image. On a thread of its own. */
void update_install_latest(void);
void update_install_image(void);
/* Stops a download; what was downloaded is kept for the next time. */
void update_cancel(void);
/* Before the launcher ends: whatever runs stops, and the network is let go. */
void update_exit(void);

#endif
