#ifndef VDISK_H
#define VDISK_H

#include <stdint.h>

/* A disk image read on demand from a read-only base (a file in the pdx,
 * say), with the Mac's writes kept in RAM as a sparse overlay of 512-byte
 * blocks.  The overlay can be saved and loaded separately, so the base
 * never needs rewriting.
 */

typedef int  (*vdisk_read_fn)(void *ctx, uint8_t *buf, uint32_t offset, uint32_t len);
typedef int  (*vdisk_write_fn)(void *ctx, const void *buf, uint32_t len);
typedef void *(*vdisk_realloc_fn)(void *p, size_t size);

typedef struct {
        uint32_t size;          /* bytes */
        uint32_t nblocks;
        vdisk_read_fn read_base;
        void *ctx;
        vdisk_realloc_fn realloc;

        uint16_t *slot;         /* per block: 0 = base, else overlay index + 1 */
        uint8_t *pool;          /* overlay blocks */
        uint32_t *pool_block;   /* block number of each overlay entry */
        uint32_t used, cap;
        uint32_t fingerprint;   /* of the base image, so overlays can't cross disks */
        int dirty;
} vdisk_t;

int     vdisk_init(vdisk_t *d, uint32_t size, vdisk_read_fn rd, void *ctx, vdisk_realloc_fn ra);

/* uMac disc_op_read / disc_op_write compatible (ctx = vdisk_t *) */
int     vdisk_read(void *d, uint8_t *data, unsigned int offset, unsigned int len);
int     vdisk_write(void *d, uint8_t *data, unsigned int offset, unsigned int len);

/* Overlay persistence */
int     vdisk_save_overlay(vdisk_t *d, vdisk_write_fn wr, void *ctx);
int     vdisk_load_overlay(vdisk_t *d, const uint8_t *buf, uint32_t len);

#endif
