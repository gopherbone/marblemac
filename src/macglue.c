/* Mac low-memory plumbing for driving the cursor from outside:
 * absolute mouse positioning, reading the current cursor image, and
 * temporarily erasing the cursor the Mac drew so we can draw our own.
 */

#include <string.h>
#include "machw.h"
#include "umac.h"
#include "macglue.h"

#define ROWBYTES        (DISP_WIDTH / 8)

int mac_booted(void)
{
        /* ROM overlay gone, ticks running, cursor initialised */
        return !overlay && RAM_RD32(LM_TICKS) > 90 && RAM_RD8(LM_CRSRCOUPLE) != 0;
}

/* Absolute positioning, as Mini vMac does: write the new position into
 * MTemp/RawMouse and ask the cursor VBL task to redraw.
 */
void mac_set_mouse(int x, int y)
{
        int v = RAM_RD16(LM_MOUSE);
        int h = RAM_RD16(LM_MOUSE + 2);
        if (v == y && h == x)
                return;
        RAM_WR16(LM_MTEMP, y);
        RAM_WR16(LM_MTEMP + 2, x);
        RAM_WR16(LM_RAWMOUSE, y);
        RAM_WR16(LM_RAWMOUSE + 2, x);
        RAM_WR8(LM_CRSRNEW, RAM_RD8(LM_CRSRCOUPLE));
}

void mac_get_cursor(mac_cursor_t *out)
{
        for (int i = 0; i < 16; i++) {
                out->data[i] = RAM_RD16(LM_THECRSR + i * 2);
                out->mask[i] = RAM_RD16(LM_THECRSR + 32 + i * 2);
        }
        out->hot_v = (int16_t)RAM_RD16(LM_THECRSR + 64);
        out->hot_h = (int16_t)RAM_RD16(LM_THECRSR + 66);
}

int mac_cursor_visible(void)
{
        return (int16_t)RAM_RD16(LM_CRSRSTATE) >= 0 && RAM_RD8(LM_CRSROBSCURE) == 0;
}

/* If the Mac's cursor is on screen, put back the pixels it saved from
 * underneath it (one long per row at CrsrAddr), remembering what was
 * there so mac_unerase_cursor() can restore the Mac's view exactly.
 */
void mac_erase_cursor(mac_crsr_undo_t *u)
{
        u->active = 0;
        if (!RAM_RD8(LM_CRSRVIS))
                return;

        uint32_t fb = umac_get_fb_offset();
        uint32_t addr = RAM_RD32(LM_CRSRADDR) & 0xffffff;
        int rows = (int16_t)RAM_RD16(LM_CRSRRECT + 4) - (int16_t)RAM_RD16(LM_CRSRRECT);
        if (addr < fb || addr >= fb + ROWBYTES * DISP_HEIGHT || rows <= 0 || rows > 16)
                return;

        uint8_t *ram = ram_get_base();
        int col = (addr - fb) % ROWBYTES;
        for (int r = 0; r < rows; r++) {
                uint8_t *p = ram + addr + r * ROWBYTES;
                if (p + 4 > ram + fb + ROWBYTES * DISP_HEIGHT)
                        break;
                for (int b = 0; b < 4 && col + b < ROWBYTES; b++) {
                        u->pix[r][b] = p[b];
                        p[b] = ram[LM_CRSRSAVE + r * 4 + b];
                }
        }
        u->addr = addr;
        u->rows = rows;
        u->active = 1;
}

void mac_unerase_cursor(mac_crsr_undo_t *u)
{
        if (!u->active)
                return;
        uint8_t *ram = ram_get_base();
        uint32_t fb = umac_get_fb_offset();
        int col = (u->addr - fb) % ROWBYTES;
        for (int r = 0; r < u->rows; r++) {
                uint8_t *p = ram + u->addr + r * ROWBYTES;
                if (p + 4 > ram + fb + ROWBYTES * DISP_HEIGHT)
                        break;
                for (int b = 0; b < 4 && col + b < ROWBYTES; b++)
                        p[b] = u->pix[r][b];
        }
        u->active = 0;
}
