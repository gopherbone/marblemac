#ifndef MACGLUE_H
#define MACGLUE_H

#include <stdint.h>
#include "cursor.h"

/* Low-memory globals */
#define LM_TICKS        0x16a
#define LM_MTEMP        0x828   /* Point v,h: new mouse position */
#define LM_RAWMOUSE     0x82c
#define LM_MOUSE        0x830
#define LM_CRSRRECT     0x83c   /* Rect t,l,b,r of the drawn cursor */
#define LM_THECRSR      0x844   /* Cursor record: data[16], mask[16], hotspot */
#define LM_CRSRADDR     0x888   /* Screen address of the saved block */
#define LM_CRSRSAVE     0x88c   /* 16 longs of screen from under the cursor */
#define LM_CRSRVIS      0x8cc   /* byte, cursor currently drawn */
#define LM_CRSRNEW      0x8ce
#define LM_CRSRCOUPLE   0x8cf
#define LM_CRSRSTATE    0x8d0   /* word, <0 = hidden */
#define LM_CRSROBSCURE  0x8d2

/* Holds whatever the Mac's cursor overwrote, while it's erased */
typedef struct {
        int     active;
        int     rows;
        uint32_t addr;
        uint8_t pix[16][4];
} mac_crsr_undo_t;

int     mac_booted(void);
void    mac_set_mouse(int x, int y);
void    mac_get_cursor(mac_cursor_t *out);
int     mac_cursor_visible(void);
void    mac_erase_cursor(mac_crsr_undo_t *u);
void    mac_unerase_cursor(mac_crsr_undo_t *u);

#endif
