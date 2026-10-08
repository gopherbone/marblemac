#include <stddef.h>
#include <string.h>
#include "vdisk.h"

#define BLK             512
#define GROW            64
#define OVL_MAGIC       0x4d4f5632      /* 'MOV2' */
#define MAX_SLOTS       65535

int vdisk_init(vdisk_t *d, uint32_t size, vdisk_read_fn rd, void *ctx, vdisk_realloc_fn ra)
{
        memset(d, 0, sizeof *d);
        d->size = size;
        d->nblocks = (size + BLK - 1) / BLK;
        d->read_base = rd;
        d->ctx = ctx;
        d->realloc = ra;
        d->slot = ra(NULL, d->nblocks * sizeof *d->slot);
        if (!d->slot)
                return -1;
        memset(d->slot, 0, d->nblocks * sizeof *d->slot);

        /* Fingerprint: FNV-1a over the size and the base's first 3 blocks
         * (boot blocks + HFS master directory block, which holds the
         * volume's creation date).
         */
        uint8_t head[3 * BLK];
        uint32_t n = size < sizeof head ? size - size % BLK : sizeof head;
        uint32_t h = 2166136261u ^ size;
        if (n && rd(ctx, head, 0, n) == 0)
                for (uint32_t i = 0; i < n; i++)
                        h = (h ^ head[i]) * 16777619u;
        d->fingerprint = h;
        return 0;
}

static uint8_t *overlay_block(vdisk_t *d, uint32_t blk)
{
        if (d->slot[blk])
                return d->pool + (size_t)(d->slot[blk] - 1) * BLK;

        if (d->used == d->cap) {
                if (d->cap >= MAX_SLOTS)
                        return NULL;
                uint32_t ncap = d->cap + GROW;
                if (ncap > MAX_SLOTS)
                        ncap = MAX_SLOTS;
                uint8_t *np = d->realloc(d->pool, (size_t)ncap * BLK);
                if (!np)
                        return NULL;
                d->pool = np;
                uint32_t *nb = d->realloc(d->pool_block, ncap * sizeof *nb);
                if (!nb)
                        return NULL;
                d->pool_block = nb;
                d->cap = ncap;
        }
        d->pool_block[d->used] = blk;
        d->slot[blk] = (uint16_t)(++d->used);
        return d->pool + (size_t)(d->used - 1) * BLK;
}

int vdisk_read(void *vd, uint8_t *data, unsigned int offset, unsigned int len)
{
        vdisk_t *d = vd;
        if (offset + len > d->size || (offset % BLK) || (len % BLK))
                return -1;

        /* One base read for the whole range, then patch in overlay blocks */
        if (d->read_base(d->ctx, data, offset, len) < 0)
                return -1;
        for (uint32_t b = offset / BLK, i = 0; i < len / BLK; b++, i++) {
                if (d->slot[b])
                        memcpy(data + i * BLK, d->pool + (size_t)(d->slot[b] - 1) * BLK, BLK);
        }
        return 0;
}

int vdisk_write(void *vd, uint8_t *data, unsigned int offset, unsigned int len)
{
        vdisk_t *d = vd;
        if (offset + len > d->size || (offset % BLK) || (len % BLK))
                return -1;
        for (uint32_t b = offset / BLK, i = 0; i < len / BLK; b++, i++) {
                uint8_t *p = overlay_block(d, b);
                if (!p)
                        return -1;
                memcpy(p, data + i * BLK, BLK);
        }
        d->dirty = 1;
        return 0;
}

/* Format: magic, base fingerprint, count, then count x (block number, 512 bytes),
 * all big-endian.
 */
static void put32(uint8_t *p, uint32_t v)
{
        p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t get32(const uint8_t *p)
{
        return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

int vdisk_save_overlay(vdisk_t *d, vdisk_write_fn wr, void *ctx)
{
        uint8_t hdr[12];
        put32(hdr, OVL_MAGIC);
        put32(hdr + 4, d->fingerprint);
        put32(hdr + 8, d->used);
        if (wr(ctx, hdr, sizeof hdr) < 0)
                return -1;
        for (uint32_t i = 0; i < d->used; i++) {
                uint8_t bn[4];
                put32(bn, d->pool_block[i]);
                if (wr(ctx, bn, 4) < 0 || wr(ctx, d->pool + (size_t)i * BLK, BLK) < 0)
                        return -1;
        }
        d->dirty = 0;
        return 0;
}

int vdisk_load_overlay(vdisk_t *d, const uint8_t *buf, uint32_t len)
{
        if (len < 12 || get32(buf) != OVL_MAGIC || get32(buf + 4) != d->fingerprint)
                return -1;
        uint32_t n = get32(buf + 8);
        if (len < 12 + (uint64_t)n * (4 + BLK))
                return -1;
        const uint8_t *p = buf + 12;
        for (uint32_t i = 0; i < n; i++, p += 4 + BLK) {
                uint32_t b = get32(p);
                if (b >= d->nblocks)
                        return -1;
                uint8_t *dst = overlay_block(d, b);
                if (!dst)
                        return -1;
                memcpy(dst, p + 4, BLK);
        }
        d->dirty = 0;
        return 0;
}
