// GameKing (Timetop GM-218, Sitronix ST2204) emulation core.
//
// Platform-independent: the same core runs on the PM2040's RP2040 and on a
// desktop host for testing. Hardware behaviour follows the GameKing MiSTer
// core (rtl/st2204/*), which is measured against real silicon.
//
// The platform provides:
//   void gk_plat_frame(void);          // an LCD refresh completed
//   void gk_plat_audio(uint8_t s);     // one 5-bit sample (0..31) at
//                                      // GK_AUDIO_RATE; may block to
//                                      // throttle emulation to real time
#ifndef GK_H
#define GK_H

#include <stdint.h>

// On the PM2040 the whole of gk.c is placed in RAM by the linker script
// (section .gk_ram), so no per-function placement is needed.
#define GK_FAST(fn) fn

// Emulated master clock (OSCI) and derived rates.
#define GK_CPU_HZ        6000000u
#define GK_AUDIO_DIV     384u                       // CPU cycles per sample
#define GK_AUDIO_RATE    (GK_CPU_HZ / GK_AUDIO_DIV) // 15625 Hz

// LCD geometry.
#define GK_LCD_W 48
#define GK_LCD_H 32

// Button bits for gk_set_buttons() (1 = pressed).
#define GK_BTN_A      0x01
#define GK_BTN_B      0x02
#define GK_BTN_START  0x04
#define GK_BTN_SELECT 0x08
#define GK_BTN_UP     0x10
#define GK_BTN_DOWN   0x20
#define GK_BTN_LEFT   0x40
#define GK_BTN_RIGHT  0x80

// bios: 512 KB internal mask ROM image. rom/romSize: cartridge (may be 0).
void gk_init(const uint8_t *bios, const uint8_t *rom, uint32_t romSize);
void gk_reset(void);

// Serve the 16 KB ROM page starting at `src` (16 KB aligned within the BIOS
// or cartridge image) from the RAM copy `dst` instead. The caller copies.
void gk_add_shadow(const uint8_t *src, const uint8_t *dst);

// Run until at least `cycles` CPU cycles have elapsed.
void gk_run(uint32_t cycles);

void gk_set_buttons(uint8_t buttons);

// Decode the current LCD contents into 48x32 darkness levels (0 = light,
// 3 = dark), row-major, top row first.
void gk_decode_frame(uint8_t *dst);

// Total emulated cycles so far.
uint64_t gk_cycles(void);

// Platform callbacks.
void gk_plat_frame(void);
void gk_plat_audio(uint8_t sample);

// Debug access (host tools).
uint8_t gk_peek(uint16_t addr);
void gk_cpu_state(uint16_t *pc, uint8_t *a, uint8_t *x, uint8_t *y,
                  uint8_t *s, uint8_t *p);

// Bare-CPU test mode: 64 KB flat RAM, no SoC peripherals (for the
// 6502/65C02 functional test suites).
void gk_test_flat(uint8_t *mem64k, uint16_t pc);
void gk_test_step(uint32_t cycles);

#endif
