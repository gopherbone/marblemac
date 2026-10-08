/* Host test for the cursor plumbing: boot a disk, steer the mouse via
 * low memory, open a menu, and dump the screen with our rotated cursor
 * composited on top (as the Playdate would draw it).
 * usage: host_cursor rom.bin disk.img outprefix [script...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "umac.h"
#include "machw.h"
#include "rom.h"
#include "cursor.h"
#include "macglue.h"
#include "vdisk.h"

static uint64_t t, vs = 16626, hz = 1000000;
static mac_cursor_t shadow;

static void run_ms(int ms)
{
        uint64_t end = t + ms * 1000ull;
        while (t < end) {
                umac_loop();
                t += 5000;
                while (t >= vs) { umac_vsync_event(); vs += 16626; }
                while (t >= hz) { umac_1hz_event(); hz += 1000000; }
        }
}

static FILE *disk_fp;
static vdisk_t vd;

static int base_read(void *ctx, uint8_t *buf, uint32_t off, uint32_t len)
{
        fseek(disk_fp, off, SEEK_SET);
        return fread(buf, 1, len, disk_fp) == len ? 0 : -1;
}

static uint8_t ovl_buf[16 << 20];
static int ovl_append(void *ctx, const void *buf, uint32_t len)
{
        size_t *n = ctx;
        memcpy(ovl_buf + *n, buf, len);
        *n += len;
        return 0;
}

static void *host_realloc(void *p, size_t n) { return n ? realloc(p, n) : (free(p), NULL); }

static uint8_t *slurp(const char *p, size_t *n)
{
        FILE *f = fopen(p, "rb");
        if (!f) { perror(p); exit(1); }
        fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc(*n);
        if (fread(b, 1, *n, f) != *n) exit(1);
        fclose(f);
        return b;
}

/* Frame step like the Playdate update(): position, emulate, steal */
static void frame(int x, int y, int button)
{
        mac_set_mouse(x, y);
        umac_mouse(0, 0, button);
        run_ms(25);
}

static void dump(const char *prefix, const char *name, int x, int y, float dir_deg)
{
        /* Mac fb is 1=black; convert to Playdate polarity, draw cursor */
        static uint8_t f[64 * 342];
        const uint8_t *fb = ram_get_base() + umac_get_fb_offset();
        mac_crsr_undo_t undo;
        mac_erase_cursor(&undo);
        for (int i = 0; i < 64 * 342; i++)
                f[i] = ~fb[i];
        mac_unerase_cursor(&undo);
        mac_get_cursor(&shadow);
        char path[256];
        snprintf(path, sizeof path, "%s_%s_raw.bin", prefix, name);
        FILE *o = fopen(path, "wb"); fwrite(f, 1, sizeof f, o); fclose(o);
        if (mac_cursor_visible()) {
                float rot = cursor_is_pointer(&shadow) ? dir_deg * (float)M_PI / 180 - CURSOR_ARROW_ANGLE : 0;
                cursor_draw(f, 64, 512, 342, &shadow, x, y, rot);
        }
        snprintf(path, sizeof path, "%s_%s.bin", prefix, name);
        o = fopen(path, "wb"); fwrite(f, 1, sizeof f, o); fclose(o);
        printf("%s: Mouse=(%d,%d) CrsrState=%d visible=%d hot=(%d,%d) pointer=%d\n", name,
               (int16_t)RAM_RD16(LM_MOUSE + 2), (int16_t)RAM_RD16(LM_MOUSE),
               (int16_t)RAM_RD16(LM_CRSRSTATE), mac_cursor_visible(),
               shadow.hot_h, shadow.hot_v, cursor_is_pointer(&shadow));
}

int main(int argc, char **argv)
{
        size_t rn, dn;
        setvbuf(stdout, NULL, _IONBF, 0);
        uint8_t *rom = slurp(argv[1], &rn);
        if (rom_patch(rom)) { puts("rom_patch failed"); return 1; }
        uint8_t *ram = calloc(1, RAM_SIZE);
        ram[0x2af] = 0x40;
        disc_descr_t d[DISC_NUM_DRIVES] = {0};
        /* Same on-demand + overlay path as the Playdate */
        disk_fp = fopen(argv[2], "rb");
        fseek(disk_fp, 0, SEEK_END);
        vdisk_init(&vd, ftell(disk_fp), base_read, NULL, host_realloc);
        d[0].size = vd.size;
        d[0].op_ctx = &vd;
        d[0].op_read = vdisk_read;
        d[0].op_write = vdisk_write;
        umac_init(ram, rom, d);

        int waited = 0;
        while (!mac_booted()) { run_ms(100); waited += 100; }
        printf("mac_booted after %d ms\n", waited);
        run_ms(20000);  /* let the Finder come up */

        /* Script from argv[4..]: "m X Y" glide to X,Y; "c" click;
         * "dc" double-click; "w MS" wait; "s NAME" snapshot
         */
        int x = 15, y = 15;
        for (int i = 4; i < argc; i++) {
                if (!strcmp(argv[i], "m")) {
                        int tx = atoi(argv[++i]), ty = atoi(argv[++i]);
                        for (int k = 1; k <= 20; k++)
                                frame(x + (tx - x) * k / 20, y + (ty - y) * k / 20, 0);
                        x = tx; y = ty;
                } else if (!strcmp(argv[i], "c") || !strcmp(argv[i], "dc")) {
                        int n = argv[i][0] == 'd' ? 2 : 1;
                        for (int k = 0; k < n; k++) {
                                for (int j = 0; j < 3; j++) frame(x, y, 1);
                                for (int j = 0; j < 3; j++) frame(x, y, 0);
                        }
                } else if (!strcmp(argv[i], "w")) {
                        int ms = atoi(argv[++i]);
                        for (int j = 0; j < ms / 25; j++) frame(x, y, 0);
                } else if (!strcmp(argv[i], "s")) {
                        dump(argv[3], argv[++i], x, y, 45);
                }
        }
        printf("overlay: %u blocks written\n", vd.used);

        /* Round-trip the overlay and check every block reads back the same */
        size_t olen = 0;
        vdisk_save_overlay(&vd, ovl_append, &olen);
        vdisk_t v2;
        vdisk_init(&v2, vd.size, base_read, NULL, host_realloc);
        int bad = vdisk_load_overlay(&v2, ovl_buf, olen) < 0;
        static uint8_t a[512], b[512];
        for (uint32_t off = 0; !bad && off < vd.size; off += 512) {
                vdisk_read(&vd, a, off, 512);
                vdisk_read(&v2, b, off, 512);
                bad = memcmp(a, b, 512) != 0;
        }
        printf("overlay round trip: %s (%zu bytes)\n", bad ? "MISMATCH" : "ok", olen);

        return 0;
}
