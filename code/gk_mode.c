// GK mini mode for the PM2040 multi-ROM firmware. See gk_mode.h.
//
// This is the runtime part of GK mini (github: GK mini repo, code/main.c)
// without the cart bus setup, which main.c already owns. Core 0 runs the
// emulator, core 1 converts frames and feeds audio to the PM program.

#include "pico/stdlib.h"
#include <string.h>

#include "hardware/sync.h"
#include "pico/multicore.h"

#include "gk.h"
#include "gk_mode.h"

// The BIOS and slot table live in gk_storage.c: defined in their own file,
// so the compiler cannot fold their unpatched (all-zero) contents here.

#define SLOT_TABLE ((const gk_slot_table_t *)gk_slot_table_storage)
#define SLOT_SIZE 524288

// The PM-side program from GB mini / GK mini. rpfb.h declares it as the
// RAM array `rom`; here it is renamed and kept in flash instead (section
// .gk_rpfb), then copied into RAM by gkmode_prepare(). rpfb.h itself is
// used unmodified.
#define rom __attribute__((section(".gk_rpfb"))) gk_rpfb_image
#include "rpfb.h"
#undef rom

// GK buffers live in .gk_noinit (memmap_16MBFlash.ld): not cleared at boot,
// and laid out so that the small ones fill the gap before the 32 KB
// aligned PM program buffer.
#define GK_NOINIT(sub) __attribute__((section(".gk_noinit." sub)))
static uint8_t GK_NOINIT("pm") __attribute__((aligned(32768))) pmRom[32768];
#define rom pmRom

// RAM copies of hot ROM pages (see GK mini).
#define SHADOW_BIOS_PAGES 2
#define SHADOW_ROM_PAGES  4
static uint8_t GK_NOINIT("big") shadow[SHADOW_BIOS_PAGES + SHADOW_ROM_PAGES][16384];

// Emulator core code: linked to RAM, stored in flash, copied in gkmode_run().
extern uint8_t __gk_ram_start__[], __gk_ram_end__[], __gk_ram_source__[];
// Emulator core .bss, kept out of the boot-time clear (see the linker script).
extern uint8_t __gk_bss_start__[], __gk_bss_end__[];

// ---------------------------------------------------------------------
// Slot lookup
// ---------------------------------------------------------------------
// RAM copy of the slot table (taken in gkmode_prepare()), so the lookup
// after the menu selection needs no flash access.
static gk_slot_table_t slotTable;

int __not_in_flash_func(gkmode_slot_lookup)(uint32_t slot, uint32_t *romSize)
{
	const gk_slot_table_t *t = &slotTable;
	// The table sits at a fixed address; unpatched it is all zero apart
	// from the marker, so version 0 means "no GameKing slots". (The magic
	// is deliberately not compared here: the compiler would place it as a
	// constant in the firmware image, where the patcher could find it.)
	if (t->version != 1)
		return -1;
	if (slot >= GK_SLOT_COUNT)
		return -1;
	uint32_t p = t->slot[slot].parent;
	if (p >= GK_SLOT_COUNT || p > slot)
		return -1;
	uint32_t size = t->slot[p].romSize;
	if (size == 0 || size > (GK_SLOT_COUNT - p) * SLOT_SIZE)
		return -1;
	// A continuation entry must lie within its parent's game.
	if ((slot - p) * SLOT_SIZE >= size)
		return -1;
	*romSize = size;
	return (int)p;
}

uint8_t *__not_in_flash_func(gkmode_pm_rom)(void) { return pmRom; }

void __not_in_flash_func(gkmode_prepare)(void)
{
	memcpy(pmRom, gk_rpfb_image, sizeof pmRom);
	memcpy(&slotTable, SLOT_TABLE, sizeof slotTable);
}

// ---------------------------------------------------------------------
// PM side interface (rpfb.h program)
// ---------------------------------------------------------------------
#define FBOFFSET 0x4000
#define RUMBLE    *((volatile uint8_t *)(rom + 0x4300))
#define AUDIO     *((volatile uint8_t *)(rom + 0x4301))
#define AUDIO_VOL *((volatile uint8_t *)(rom + 0x4302))

#define PM_A     0x01
#define PM_B     0x02
#define PM_C     0x04
#define PM_UP    0x08
#define PM_DOWN  0x10
#define PM_LEFT  0x20
#define PM_RIGHT 0x40
#define PM_POWER 0x80

#define FRAMEBLEND 2
static volatile unsigned frameBlendCnt = 0;

#define FBDEPTH 3
static uint8_t GK_NOINIT("small") gkFrame[FBDEPTH][GK_LCD_W * GK_LCD_H];   // cleared in gkmode_run()
static volatile uint32_t readPtr = 0;
static volatile uint32_t writePtr = 1;

static PIO pioWE_g;
static uint sm_we_g, sm_we_addr_g;
static volatile uint32_t pmKeys = 0;
static volatile uint32_t pmKeyReports = 0;

// Mute: hold C while selecting the game (C has no function in the menu).
#define MUTE_WAIT_US 300000
static volatile uint32_t audioMuted = 0;
static uint32_t ignoreC = 0;

#define AUDIO_RING 1024
#define AUDIO_HIGH 512
static uint8_t audioRing[AUDIO_RING];
static volatile uint32_t audioHead = 0, audioTail = 0;

// ---------------------------------------------------------------------
// Emulator callbacks (core 0)
// ---------------------------------------------------------------------
static uint8_t mapKeys(uint32_t k)
{
	if (ignoreC) {
		if (k & PM_C)
			k &= ~PM_C;
		else
			ignoreC = 0;
	}
	uint8_t b = 0;
	if (k & PM_A)     b |= GK_BTN_A;
	if (k & PM_B)     b |= GK_BTN_B;
	if (k & PM_C)     b |= GK_BTN_START;
	if (k & PM_POWER) b |= GK_BTN_SELECT;
	if (k & PM_UP)    b |= GK_BTN_UP;
	if (k & PM_DOWN)  b |= GK_BTN_DOWN;
	if (k & PM_LEFT)  b |= GK_BTN_LEFT;
	if (k & PM_RIGHT) b |= GK_BTN_RIGHT;
	return b;
}

void __not_in_flash_func(gk_plat_frame)(void)
{
	gk_decode_frame(gkFrame[writePtr]);
	__dmb();
	uint32_t next = writePtr + 1;
	if (next == FBDEPTH)
		next = 0;
	if (next != readPtr)
		writePtr = next;
}

void __not_in_flash_func(gk_plat_audio)(uint8_t s)
{
	while ((audioHead - audioTail) >= AUDIO_HIGH)
		tight_loop_contents();
	audioRing[audioHead & (AUDIO_RING - 1)] = s;
	__dmb();
	audioHead++;

	if ((audioHead & 63) == 0)
		gk_set_buttons(mapKeys(pmKeys));
}

// ---------------------------------------------------------------------
// Core 1: audio output and frame conversion
// ---------------------------------------------------------------------
static bool __not_in_flash_func(doAudio)(repeating_timer_t *t)
{
	(void)t;
	if (audioTail != audioHead) {
		if (!audioMuted)
			AUDIO = audioRing[audioTail & (AUDIO_RING - 1)];
		__dmb();
		audioTail++;
	}
	return true;
}

static void __not_in_flash_func(handleBuffer)(void)
{
	uint8_t *fb = rom + FBOFFSET;

	static repeating_timer_t t;
	alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(4);
	alarm_pool_add_repeating_timer_us(pool, -(int64_t)(1000000 / GK_AUDIO_RATE),
		doAudio, 0, &t);

	static const uint8_t colorPattern[4][3] = {
		{ 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
	};

	while (1) {
		if (frameBlendCnt == 0) {
			uint32_t nextReadPtr = readPtr + 1;
			if (nextReadPtr == FBDEPTH)
				nextReadPtr = 0;
			if (nextReadPtr != writePtr)
				readPtr = nextReadPtr;
		}
		__dmb();
		const uint8_t *src = gkFrame[readPtr];

		for (unsigned y = 0; y < 8; ++y) {
			for (unsigned x = 0; x < 96; ++x) {
				unsigned tmp = 0;
				for (unsigned i = 0; i < 8; ++i) {
					unsigned row = y * 8 + i;
					unsigned lvl = src[(row >> 1) * GK_LCD_W + (x >> 1)];
					unsigned pat = (x + row + frameBlendCnt) % (FRAMEBLEND + 1);
					tmp |= colorPattern[lvl][pat] << i;
				}
				fb[x + y * 96] = tmp;
			}
		}

		if (frameBlendCnt == FRAMEBLEND)
			frameBlendCnt = 0;
		else
			++frameBlendCnt;

		// The PM writes its key state to cart address 0 after each frame.
		while (1) {
			if (!pio_sm_is_rx_fifo_empty(pioWE_g, sm_we_g)) {
				pmKeys = pio_sm_get(pioWE_g, sm_we_g);
				uint32_t addrData = pio_sm_get(pioWE_g, sm_we_addr_g);
				if (addrData == 0x000) {
					pmKeyReports++;
					break;
				}
			}
		}
	}
}

static void __not_in_flash_func(handleNoBios)(void)
{
	// Checkerboard: a GameKing slot was chosen, but no BIOS was patched in.
	uint8_t *fb = rom + FBOFFSET;
	for (unsigned i = 0; i < 96 * 8; i++)
		fb[i] = (((i / 8) ^ (i / 96)) & 1) ? 0xF0 : 0x0F;
	while (1) {
		if (!pio_sm_is_rx_fifo_empty(pioWE_g, sm_we_g)) {
			pio_sm_get(pioWE_g, sm_we_g);
			pio_sm_get(pioWE_g, sm_we_addr_g);
		}
	}
}

// ---------------------------------------------------------------------
// Core 0: the emulator
// ---------------------------------------------------------------------
void gkmode_run(PIO pioWE, uint sm_we, uint sm_we_addr,
                const uint8_t *romPtr, uint32_t romSize)
{
	pioWE_g = pioWE;
	sm_we_g = sm_we;
	sm_we_addr_g = sm_we_addr;

	// Drop writes left over from the menu.
	while (!pio_sm_is_rx_fifo_empty(pioWE, sm_we))
		pio_sm_get(pioWE, sm_we);
	while (!pio_sm_is_rx_fifo_empty(pioWE, sm_we_addr))
		pio_sm_get(pioWE, sm_we_addr);

	memset(gkFrame, 0, sizeof gkFrame);
	memset(rom + FBOFFSET, 0, 96 * 8);
	AUDIO = 16;
	AUDIO_VOL = 3;
	RUMBLE = 0;

	if (!slotTable.biosValid) {
		multicore_launch_core1(handleNoBios);
		while (1)
			tight_loop_contents();
	}

	multicore_launch_core1(handleBuffer);

	// Bring the emulator core into RAM (see memmap_16MBFlash.ld).
	memcpy(__gk_ram_start__, __gk_ram_source__,
		(size_t)(__gk_ram_end__ - __gk_ram_start__));
	memset(__gk_bss_start__, 0, (size_t)(__gk_bss_end__ - __gk_bss_start__));
	__dsb();
	__isb();

	// Mute check: wait for a few PM key reports, then look at C.
	uint64_t t0 = time_us_64();
	while (pmKeyReports < 3 && time_us_64() - t0 < MUTE_WAIT_US)
		tight_loop_contents();
	if (pmKeys & PM_C) {
		audioMuted = 1;
		ignoreC = 1;
		AUDIO = 0;
	}

	const uint8_t *bios = gk_bios_storage;
	gk_init(bios, romPtr, romSize);

	unsigned s = 0;
	for (unsigned p = 0; p < SHADOW_BIOS_PAGES; p++, s++) {
		memcpy(shadow[s], bios + p * 16384, 16384);
		gk_add_shadow(bios + p * 16384, shadow[s]);
	}
	for (unsigned p = 0; p < SHADOW_ROM_PAGES && (p + 1) * 16384 <= romSize; p++, s++) {
		memcpy(shadow[s], romPtr + p * 16384, 16384);
		gk_add_shadow(romPtr + p * 16384, shadow[s]);
	}
	gk_reset();

	while (1)
		gk_run(GK_CPU_HZ);
}
