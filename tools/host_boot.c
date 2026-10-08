/* Host smoke test: boot uMac headless for N emulated seconds, dump the
 * framebuffer (raw 512x342 1bpp) to a file.
 * usage: host_boot rom.bin [disk.img|-] seconds out.raw
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "umac.h"
#include "machw.h"
#include "rom.h"
#include "m68k.h"

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

int main(int argc, char **argv)
{
        size_t rn, dn = 0;
        uint8_t *rom = slurp(argv[1], &rn);
        if (rom_patch(rom)) { puts("rom_patch failed"); return 1; }
        uint8_t *ram = calloc(1, RAM_SIZE);
        disc_descr_t d[DISC_NUM_DRIVES] = {0};
        if (strcmp(argv[2], "-")) {
                d[0].base = slurp(argv[2], &dn);
                d[0].size = dn;
        }
        if (getenv("WARM")) { ram[0x2af] = 0x40; }
        umac_init(ram, rom, d);
        if (getenv("DASM")) {
                unsigned a = strtoul(getenv("DASM"), 0, 16), end = a + strtoul(getenv("DLEN"), 0, 16);
                char buf[100];
                while (a < end) { int l = m68k_disassemble(buf, a, M68K_CPU_TYPE_68000); printf("%06x: %s\n", a, buf); a += l; }
                return 0;
        }
        double secs = atof(argv[3]);
        uint64_t t = 0, vs = 16626, hz = 1000000;
        while (t < secs * 1e6) {
                if (umac_loop()) { puts("emulator stopped"); break; }
                t += 5000;
                if (getenv("TRACE") && (t % 200000) == 0) printf("t=%llu PC=%06x SR=%04x\n", (unsigned long long)t, m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_SR));
                while (t >= vs) { umac_vsync_event(); vs += 16626; }
                while (t >= hz) { umac_1hz_event(); hz += 1000000; }
        }
        FILE *o = fopen(argv[4], "wb");
        fwrite(ram + umac_get_fb_offset(), 1, DISP_WIDTH * DISP_HEIGHT / 8, o);
        fclose(o);
        printf("Mouse v=%d h=%d CrsrState=%d CrsrCouple=%02x Ticks=%u\n",
               (int16_t)RAM_RD16(0x830), (int16_t)RAM_RD16(0x832),
               (int16_t)RAM_RD16(0x8d0), RAM_RD8(0x8cf), RAM_RD32(0x16a));
        return 0;
}
