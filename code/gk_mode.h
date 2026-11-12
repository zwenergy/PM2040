// GK mini mode for the PM2040 multi-ROM firmware: runs a GameKing game from
// a menu slot through the GK mini emulator (see gk.c and the GK mini repo).
#ifndef GK_MODE_H
#define GK_MODE_H

#include <stdint.h>
#include "hardware/pio.h"

#define GK_SLOT_COUNT 20

// Slot table written by the multi-ROM patcher (section .gkStorage).
// An unpatched table (all zero apart from the magic) means "no GameKing
// games", so every slot behaves exactly as before.
typedef struct {
	char magic[8];            // "GKSLOTTB"
	uint8_t version;          // 1
	uint8_t biosValid;        // 1 once the patcher wrote a BIOS
	uint8_t reserved[6];
	struct {
		uint32_t romSize;     // GameKing ROM size in bytes, 0 = PM game
		uint8_t parent;       // slot holding the start of the game
		uint8_t reserved[3];  // (a GK ROM larger than 512 KB continues
	} slot[GK_SLOT_COUNT];    //  into the following slots)
} gk_slot_table_t;

extern const uint8_t gk_slot_table_storage[4096];
extern const uint8_t gk_bios_storage[524288];

// Returns the slot where the selected GameKing game starts, or -1 if the
// slot holds a Pokemon mini game.
int gkmode_slot_lookup(uint32_t slot, uint32_t *romSize);

// Copy the PM-side frame/audio program (rpfb.h) into RAM. Called while the
// menu is running, so that switching over after the selection is instant.
void gkmode_prepare(void);

// The RAM copy of the PM program (32 KB, 32 KB aligned).
uint8_t *gkmode_pm_rom(void);

// Start the emulator. The cart bus must already be serving gkmode_pm_rom()
// and the write-check state machines must be running. Does not return.
void gkmode_run(PIO pioWE, uint sm_we, uint sm_we_addr,
                const uint8_t *rom, uint32_t romSize) __attribute__((noreturn));

#endif
