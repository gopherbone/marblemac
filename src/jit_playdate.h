#ifndef JIT_PLAYDATE_H
#define JIT_PLAYDATE_H

#include <stdint.h>
#include "pd_api.h"

/* Returns 0 if the JIT is ready; otherwise Musashi runs everything */
int     jit_playdate_init(PlaydateAPI *api, uint8_t *ram, uint32_t ram_size,
                          const uint8_t *rom, uint32_t rom_size);

#endif
