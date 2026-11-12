// GameKing (ST2204) SoC emulation. See gk.h.
//
// Timing model: one global cycle counter at the 6 MHz OSCI rate. The CPU
// runs in bursts up to the next scheduled event; peripherals with internal
// counters (prescaler, timers, PSG) are advanced lazily ("synced") when the
// CPU touches them or when one of their events is due.
//
// Register map and behaviour follow the GameKing MiSTer RTL:
//   $00-$0F GPIO     $10-$17 PSG       $20-$27 timers   $28-$2D,$36-$37 DMA
//   $30,$38-$3B SYS  $31-$35 banking   $3C-$3F IRQ      $40-$4F LCDC
//   $50-$5F SPI      $60-$67 UART      $0080-$27FF RAM
//   $4000-$7FFF program window (PRR / IRR)   $8000-$FFFF data window (DRR)

#include "gk.h"
#include <string.h>

// ---------------------------------------------------------------------
// State
// ---------------------------------------------------------------------
static struct {
	uint32_t a, x, y, s, pc;
	uint32_t c, v, d, i, n, z;
	uint32_t wai;
} cpu;

static uint8_t g_ram[0x4000];                // 16 KB array, $0080-$27FF used
static const uint8_t *g_bios;
static const uint8_t *g_rom;
static uint32_t g_rom_mask;                  // mirror mask (pow2 - 1)
static uint8_t g_ff_page[256];               // open bus source

// 16 KB quarter windows; [0] unused (handled by explicit decode).
static const uint8_t *g_win[4];
static uint32_t g_win_mask[4];

// Scheduler.
enum { EV_32K, EV_SAMPLE, EV_LCD, EV_TIMERS, EV_N };
#define EV_NEVER 0xFFFFFFFFFFFFFFFFull
static uint64_t ev_t[EV_N];
static uint64_t g_evt_time;                  // time when g_left hits 0
static int32_t g_left;
static uint32_t g_irq_line;                  // (IREQ & IENA) != 0

static inline uint64_t now(void) { return g_evt_time - (int64_t)g_left; }

// Bus / banking.
static uint8_t irr, prrl, prrh, drrl, drrh;
static uint8_t irq_service;

// SYS.
static uint8_t sys_r, misc_r, r39, r3a;

// IRQ.
static uint32_t ireq, iena;
#define IREQ_MASK 0x77Fu

// GPIO.
static uint8_t pdata[5], pctrl[5], psc, pfc, pfd, pmcr;
static uint8_t pa_last, pa_armed;
static uint8_t ext_pa = 0xFF, ext_pb = 0xFF;

// Timers.
static uint8_t bten, btsr, prs, t0m, t0c, t1m, t1c;
static uint8_t t0_count, t1_count, t1_oscx_div, t1_oscx_256, wdt_count;
static uint32_t bt_chain;
static uint64_t pres_p;                      // prescaler increments (low 16 = PRS)
static uint64_t pres_t;                      // time of last timer sync
static uint64_t t32k_n;                      // 32.768 kHz ticks so far

// PSG.
static uint32_t psg0, psg1;
static uint8_t dac, psgc, vol;
static uint32_t sq0_count, sq1_count, sq0_phase, sq1_phase;
static uint32_t lfsr;
static uint32_t pwm_r;                       // audio clocks to next PWM count
static uint32_t pwm_count, smp_count;
static uint64_t psg_t, psg_32k;
static uint32_t psg_rem;

// DMA.
static uint32_t dms, dmd, dcnth, dmam, dmr;

// LCDC.
static uint32_t lssa, lvpw, lxmax, lymax, lpan, lar, lctr, lckr, lfra, lac,
	lpwm, lmisc, lena;
static uint32_t lcd_scan_phase;
static uint32_t lcd_period;

// Misc.
static uint8_t spi53;
static uint8_t uctr, usr, irctr, bctr, brs, bdiv;

static void sched(void);
static void sched_timers(void);
static void update_windows(void);
void gk_add_shadow(const uint8_t *src, const uint8_t *dst);

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------
static inline void update_irq(void)
{
	g_irq_line = (ireq & iena & IREQ_MASK) != 0;
}

static inline void raise_irq(uint32_t level)
{
	ireq |= (1u << level) & IREQ_MASK;
	update_irq();
}

// ---------------------------------------------------------------------
// Bank decode (st_bus.sv)
// ---------------------------------------------------------------------
enum { T_IROM, T_CE0, T_CE2, T_CE3, T_CE4, T_CE8, T_NONE };

static int cs_decode_prog(uint32_t mode)
{
	switch (mode & 7) {
	case 0: return T_IROM;
	case 1: return T_CE0;
	case 2: return T_CE4;
	case 4: return T_CE3;
	case 5: return T_CE2;
	case 6: return T_CE8;
	default: return T_NONE;
	}
}

static int cs_decode_data(uint32_t mode7, uint32_t mode)
{
	if (mode7) {
		switch (mode & 7) {
		case 0: return T_CE0;
		case 2: return T_CE2;
		case 3: return T_CE3;
		case 4: return T_CE4;
		case 7: return T_IROM;
		default: return T_NONE;
		}
	}
	return (mode & 7) == 0 ? T_IROM : T_CE8;
}

// RAM shadows of hot 16 KB ROM pages (the ROMs may live in slow memory,
// e.g. the RP2040's XIP flash).
#define GK_MAX_SHADOW 8
static const uint8_t *sh_src[GK_MAX_SHADOW];
static const uint8_t *sh_dst[GK_MAX_SHADOW];
static int sh_n;

// Source pointer for one 16 KB quarter, given target and flat offsets.
static void map_quarter(int q, int tgt, uint32_t irom_off, uint32_t ext_off)
{
	const uint8_t *p;
	if (tgt == T_IROM && g_bios) {
		p = g_bios + (irom_off & 0x7FFFF);
		g_win_mask[q] = 0x3FFF;
	} else if (tgt == T_CE0 && g_rom) {
		p = g_rom + (ext_off & g_rom_mask & ~0x3FFFu);
		g_win_mask[q] = g_rom_mask < 0x3FFF ? g_rom_mask : 0x3FFF;
	} else {
		g_win[q] = g_ff_page;
		g_win_mask[q] = 0;
		return;
	}
	for (int i = 0; i < sh_n; i++)
		if (sh_src[i] == p) { p = sh_dst[i]; break; }
	g_win[q] = p;
}

void gk_add_shadow(const uint8_t *src, const uint8_t *dst)
{
	if (sh_n < GK_MAX_SHADOW) {
		sh_src[sh_n] = src;
		sh_dst[sh_n] = dst;
		sh_n++;
		update_windows();
	}
}

static void update_windows(void)
{
	// Program window, A14 inverted through bit 0 of the bank.
	int use_irr = (sys_r & 2) && irq_service;
	uint32_t bank = use_irr ? irr : prrl;
	int tgt = use_irr ? T_IROM : cs_decode_prog(prrh);
	uint32_t a14 = (~bank & 1u) << 14;
	map_quarter(1, tgt, (((bank >> 1) & 0xF) << 15) | a14,
		((bank >> 1) << 15) | a14);

	// Data window: 32 KB.
	tgt = cs_decode_data(drrl & 0x80, drrh);
	map_quarter(2, tgt, (drrl & 0xF) << 15, (drrl & 0x7F) << 15);
	map_quarter(3, tgt, ((drrl & 0xF) << 15) | 0x4000,
		((drrl & 0x7F) << 15) | 0x4000);
}

// DMA banked source read (data-window rules via DMR).
static uint8_t dma_banked_read(uint32_t a)
{
	int tgt = cs_decode_data(dmr & 0x80, (dmr >> 8) & 7);
	uint32_t off = a & 0x7FFF;
	if (tgt == T_IROM && g_bios)
		return g_bios[(((dmr & 0xF) << 15) | off) & 0x7FFFF];
	if (tgt == T_CE0 && g_rom)
		return g_rom[(((dmr & 0x7F) << 15) | off) & g_rom_mask];
	return 0xFF;
}

// ---------------------------------------------------------------------
// GPIO (st_gpio.sv / mainboard.sv)
// ---------------------------------------------------------------------
static inline uint8_t pin_level(int port)
{
	static const uint8_t *const ext[5] = { &ext_pa, &ext_pb, 0, 0, 0 };
	uint8_t e = ext[port] ? *ext[port] : 0xFF;
	if (port == 3) {
		// PD0-5 carry /CS1-6 where PFD selects them (idle high).
		uint8_t alt = pfd & 0x3F;
		uint8_t oe = pctrl[3] | alt;
		uint8_t d = (pdata[3] & ~alt) | alt;
		return (oe & d) | (~oe & e);
	}
	return (pctrl[port] & pdata[port]) | (~pctrl[port] & e);
}

static void pa_check(void)
{
	uint8_t p = pin_level(0);
	if (pa_armed && p != pa_last) {
		pa_armed = 0;
		raise_irq(4);
	}
	pa_last = p;
}

// ---------------------------------------------------------------------
// Timers (st_timer.sv)
// ---------------------------------------------------------------------
static const uint8_t tap_shift[8] = { 16, 15, 13, 11, 8, 5, 3, 1 };

// Advance an 8-bit up-counter by `n` taps with overflow reload.
// Returns nonzero if it overflowed at least once.
static int count_taps(uint8_t *cnt, uint64_t n, int reload_en, uint8_t reload)
{
	uint32_t c = *cnt;
	if (n < 0x100 - c) {
		*cnt = c + n;
		return 0;
	}
	n -= 0x100 - c;
	uint32_t r = reload_en ? reload : 0;
	uint32_t per = 0x100 - r;
	*cnt = (uint8_t)(r + (n % per));
	return 1;
}

static inline int prs_run(void) { return (prs & 0x60) == 0x40; }
static inline int t1_prs_mode(void) { return !(t1m & 8) && (t1m & 3) != 3; }

static void timers_sync(void)
{
	uint64_t t = now();
	if (prs_run() && t > pres_t) {
		uint64_t d = t - pres_t;
		uint64_t p0 = pres_p, p1 = p0 + d;
		if (t0m & 0x20) {
			uint32_t sh = tap_shift[t0m & 7];
			uint64_t n = (p1 >> sh) - (p0 >> sh);
			if (n && count_taps(&t0_count, n,
					t0m & 0x10, t0c))
				raise_irq(2);
		}
		if (t1_prs_mode()) {
			uint32_t sh = tap_shift[t1m & 7];
			uint64_t n = (p1 >> sh) - (p0 >> sh);
			if (n && count_taps(&t1_count, n,
					t1m & 0x10, t1c))
				raise_irq(3);
		}
		pres_p = p1;
	}
	pres_t = t;
}

// Cycles until the next overflow of a prescaler-driven timer.
static uint64_t timer_next(uint32_t mode, uint8_t cnt)
{
	uint32_t sh = tap_shift[mode & 7];
	uint64_t k = 0x100 - cnt;
	uint64_t target = ((pres_p >> sh) + k) << sh;
	return target - pres_p;
}

// ---------------------------------------------------------------------
// PSG (st_psg.sv)
// ---------------------------------------------------------------------
static inline uint32_t psg_mode(void) { return psgc & 0xF; }
static inline int psg_pwm_mode(void)
{
	uint32_t m = psg_mode();
	return m == 1 || m == 5 || m == 9 || m == 0xD;
}
static inline int psg_noise_mode(void)
{
	uint32_t m = psg_mode();
	return m == 0xC || m == 0xE;
}
static inline int psg_int_en(void)
{
	switch (psg_mode()) {
	case 0x0: case 0x2: case 0x8: case 0xA: case 0xC: case 0xE: case 0xF:
		return 0;
	default:
		return 1;
	}
}
static inline uint32_t psg_div(void)
{
	switch ((psgc >> 4) & 7) {
	case 1: return 2;
	case 2: return 4;
	case 3: return 6;
	default: return 1;
	}
}

// Advance a square counter by `ac` audio clocks; returns wraps.
static uint32_t sq_advance(uint32_t *count, uint32_t reload, uint32_t ac)
{
	uint32_t c = *count;
	if (ac < 0x1000 - c) {
		*count = c + ac;
		return 0;
	}
	ac -= 0x1000 - c;
	uint32_t per = 0x1000 - reload;
	*count = reload + ac % per;
	return 1 + ac / per;
}

static void psg_advance(uint32_t ac)
{
	if (!ac)
		return;
	sq0_phase += sq_advance(&sq0_count, psg0, ac);

	if (!psg_pwm_mode() && !psg_noise_mode()) {
		uint32_t p0 = sq1_phase;
		uint32_t w = sq_advance(&sq1_count, psg1, ac);
		sq1_phase += w;
		// The interrupt fires on wraps taken from an odd phase.
		if (w && psg_int_en() && (w >= 2 || (p0 & 1)))
			raise_irq(1);
	} else {
		// PWM prescaler -> 128-count PWM period -> sample counter.
		uint32_t per = 0x40 - (psg1 & 0x3F);
		uint32_t incs;
		if (ac < pwm_r) {
			pwm_r -= ac;
			incs = 0;
		} else {
			ac -= pwm_r;
			incs = 1 + ac / per;
			pwm_r = per - ac % per;
		}
		if (incs) {
			uint32_t tot = pwm_count + incs;
			uint32_t samples = tot >> 7;
			pwm_count = tot & 127;
			if (samples) {
				uint32_t n = samples > 32 ? 32 : samples;
				while (n--)
					lfsr = ((lfsr << 1) | (((lfsr >> 13) ^ (lfsr >> 14)) & 1)) & 0x7FFF;
				uint32_t reload = ((psg1 >> 8) & 0xF) << 2 | ((psg1 >> 6) & 3);
				uint32_t toint = 0x40 - smp_count;
				if (samples < toint) {
					smp_count += samples;
				} else {
					uint32_t r = samples - toint;
					uint32_t sp = 0x40 - reload;
					smp_count = reload + r % sp;
					if (psg_int_en())
						raise_irq(1);
				}
			}
		}
	}
}

static void psg_sync(void)
{
	uint64_t t = now();
	uint32_t ac;
	if (((psgc >> 4) & 7) == 7) {
		uint64_t n = (t * 256) / 46875;   // 32.768 kHz ticks at time t
		ac = (uint32_t)(n - psg_32k);
		psg_32k = n;
	} else {
		uint64_t d = t - psg_t + psg_rem;
		uint32_t div = psg_div();
		if (div == 1) {
			ac = (uint32_t)d;
			psg_rem = 0;
		} else {
			ac = (uint32_t)(d / div);
			psg_rem = (uint32_t)(d - (uint64_t)ac * div);
		}
	}
	if (((psgc >> 4) & 7) != 7)
		psg_32k = (t * 256) / 46875;
	psg_t = t;
	psg_advance(ac);
}

// Cycles until the next DAC-reload interrupt (or EV_NEVER).
static uint64_t psg_next_int(void)
{
	if (!psg_int_en())
		return EV_NEVER;
	uint64_t ac;
	if (!psg_pwm_mode() && !psg_noise_mode()) {
		uint64_t first = 0x1000 - sq1_count;
		uint64_t per = 0x1000 - psg1;
		ac = (sq1_phase & 1) ? first : first + per;
	} else {
		uint64_t per = 0x40 - (psg1 & 0x3F);
		uint64_t to_sample = pwm_r + (uint64_t)(127 - pwm_count) * per;
		uint64_t samples = 0x40 - smp_count;
		ac = to_sample + (samples - 1) * 128 * per;
	}
	uint64_t cyc;
	if (((psgc >> 4) & 7) == 7)
		cyc = ac * 183;
	else
		cyc = ac * psg_div();
	return cyc < 8 ? 8 : cyc;
}

// Current speaker level as a 5-bit sample (speaker.sv: PSGO - PSGOB).
static uint8_t psg_sample(void)
{
	int32_t v;   // in 1/128 units, -128..128
	uint32_t m = psg_mode();
	switch (m) {
	case 0x0: v = (vol & 0x80) ? 128 : 0; break;
	case 0x8: v = (vol & 0x80) ? 0 : 128; break;
	case 0x1: case 0x9:
		v = (int32_t)((64 + dac) & 127) * 2 - 128;
		break;
	case 0x5: case 0xD: {
		int32_t mag = (dac & 0x80) ? 128 - (dac & 0x7F) : (dac & 0x7F);
		v = (dac & 0x80) ? mag : -mag;
		break;
	}
	case 0x3: case 0xB: case 0xF: v = 0; break;
	case 0x7: v = 128; break;
	default: {
		// Square / noise sources through the interleaved volume PWM.
		uint32_t div = (((psgc >> 4) & 7) == 7) ? 183 : psg_div();
		uint32_t s0, s1, sn = (lfsr >> 14) & 1;
		// Squares faster than the sample rate average out to half.
		s0 = ((0x1000 - psg0) * 4 * div < 2 * GK_AUDIO_DIV) ? 1
			: ((sq0_phase >> 1) & 1) * 2;
		s1 = ((0x1000 - psg1) * 4 * div < 2 * GK_AUDIO_DIV) ? 1
			: ((sq1_phase >> 1) & 1) * 2;
		uint32_t c1 = 0, c2 = 0;
		switch (m) {
		case 0x2: case 0xA: c1 = s0; c2 = s0; break;
		case 0x4: c1 = s1; c2 = s1; break;
		case 0x6: c1 = s0; c2 = s1; break;
		case 0xC: c1 = sn * 2; c2 = sn * 2; break;
		case 0xE: c1 = s0; c2 = sn * 2; break;
		}
		uint32_t v1 = vol & 0xF, v2 = vol >> 4;
		v1 = v1 == 15 ? 16 : v1;
		v2 = v2 == 15 ? 16 : v2;
		v = (int32_t)((c1 * v1 + c2 * v2) * 4) - 128;
		break;
	}
	}
	return (uint8_t)(((v + 128) * 31 + 128) >> 8);
}

// ---------------------------------------------------------------------
// LCDC (st_lcdc.sv)
// ---------------------------------------------------------------------
static uint32_t lcd_calc_period(void)
{
	static const uint8_t pd[4] = { 1, 2, 4, 6 };
	uint32_t line_px = lxmax * 8 + (lfra + 1) * 8;
	uint32_t lines = lymax ? lymax * 2 : 512;
	return pd[(lckr >> 2) & 3] * line_px * lines;
}

static void lcd_regs_changed(void)
{
	uint32_t running = !(lctr & 0x80);
	uint32_t p = running ? lcd_calc_period() : 0;
	if (p != lcd_period) {
		lcd_period = p;
		ev_t[EV_LCD] = p ? now() + p : EV_NEVER;
	}
}

void gk_decode_frame(uint8_t *dst)
{
	memset(dst, 0, GK_LCD_W * GK_LCD_H);
	if (!(lena & 1))
		return;
	uint32_t w = lxmax * 8;
	if (w > GK_LCD_W)
		w = GK_LCD_W;
	uint32_t lines = lymax ? lymax * 2 : 512;
	if (lines > GK_LCD_H)
		lines = GK_LCD_H;
	uint32_t stride = lvpw * 4;
	uint32_t bpp2 = lctr & 4;
	uint32_t rev = (lctr & 0x20) ? 3 : 0;
	uint32_t base = lssa;
	for (uint32_t i = 0; i < lines; i++, base += stride) {
		// The panel scans bottom-up.
		uint8_t *row = dst + (GK_LCD_H - 1 - i) * GK_LCD_W;
		if (bpp2) {
			for (uint32_t x = 0; x < w; x += 4) {
				uint32_t b = g_ram[(base + (x >> 2)) & 0x3FFF];
				row[x + 0] = ((b >> 6) & 3) ^ rev;
				row[x + 1] = ((b >> 4) & 3) ^ rev;
				row[x + 2] = ((b >> 2) & 3) ^ rev;
				row[x + 3] = (b & 3) ^ rev;
			}
		} else {
			for (uint32_t x = 0; x < w; x++) {
				uint32_t b = g_ram[(base + (x >> 3)) & 0x3FFF];
				row[x] = (((b >> (7 - (x & 7))) & 1) ? 3 : 0) ^ rev;
			}
		}
	}
}

// ---------------------------------------------------------------------
// DMA (st_dma.sv) - executed instantly, CPU charged 2 cycles per byte.
// ---------------------------------------------------------------------
static uint8_t io_read(uint32_t a);
static void io_write(uint32_t a, uint8_t v);
static uint8_t bus_read(uint32_t a);
static void bus_write(uint32_t a, uint8_t v);

static void dma_run(uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		uint8_t b;
		if (dms & 0x8000)
			b = dma_banked_read(dms);
		else
			b = bus_read(dms);
		if ((dms & 0x7FFF) == 0x7FFF) {
			dms &= 0x8000;
			if (dms & 0x8000)
				dmr = (dmr + 1) & 0x7FF;
		} else {
			dms = (dms & 0x8000) | ((dms + 1) & 0x7FFF);
		}
		bus_write(dmd & 0x7FFF, b);
		if (!dmam)
			dmd = (dmd + 1) & 0xFFFF;
	}
	g_left -= (int32_t)(2 * count);
}

// ---------------------------------------------------------------------
// Register window
// ---------------------------------------------------------------------
static uint8_t io_read(uint32_t a)
{
	if (a >= 0x80)
		return 0xFF;   // $2800-$3FFF: open bus
	switch (a) {
	// GPIO
	case 0x00: { uint8_t v = pin_level(0); pa_armed = 1; return v; }
	case 0x01: return pin_level(1);
	case 0x02: return pin_level(2);
	case 0x03: return pin_level(3);
	case 0x04: return pin_level(4);
	case 0x05: return psc;
	case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
		return pctrl[a - 8];
	case 0x0D: return pfc;
	case 0x0E: return pfd;
	case 0x0F: return pmcr;
	// PSG
	case 0x10: psg_sync(); return sq0_count & 0xFF;
	case 0x11: psg_sync(); return 0xF0 | (sq0_count >> 8);
	case 0x12: psg_sync(); return sq1_count & 0xFF;
	case 0x13: psg_sync(); return 0xF0 | (sq1_count >> 8);
	case 0x14: return dac;
	case 0x16: return 0x80 | psgc;
	case 0x17: return vol;
	// Timers
	case 0x20: return 0xE0 | bten;
	case 0x21: return 0xE0 | btsr;
	case 0x23: timers_sync(); return pres_p & 0xFF;
	case 0x24: return 0xC0 | (t0m & 0x3F);
	case 0x25: timers_sync(); return t0_count;
	case 0x26: return 0xE0 | (t1m & 0x1F);
	case 0x27: timers_sync(); return t1_count;
	// DMA
	case 0x28: return dms & 0xFF;
	case 0x29: return dms >> 8;
	case 0x2A: return dmd & 0xFF;
	case 0x2B: return dmd >> 8;
	case 0x36: return dmr & 0xFF;
	case 0x37: return 0xF8 | (dmr >> 8);
	// SYS
	case 0x30: return sys_r | 1;
	case 0x38: return 0xE0 | misc_r;
	case 0x39: return 0xF0 | r39;
	case 0x3A: return r3a;
	// Banking
	case 0x31: return irr;
	case 0x32: return prrl;
	case 0x33: return prrh;
	case 0x34: return drrl;
	case 0x35: return drrh;
	// IRQ
	case 0x3C: timers_sync(); psg_sync(); return ireq & 0xFF;
	case 0x3D: timers_sync(); psg_sync(); return 0xF0 | (ireq >> 8);
	case 0x3E: return iena & 0xFF;
	case 0x3F: return 0xF0 | (iena >> 8);
	// LCDC
	case 0x43: return lxmax;
	case 0x44: return lymax;
	case 0x45: return 0xF8 | lpan;
	case 0x47: return lctr;
	case 0x48: return lckr;
	case 0x4A: return 0xE0 | lac;
	case 0x4B: return 0xC0 | lpwm;
	case 0x4C: return (lmisc & 0xF6) | (lcd_scan_phase & 1);
	case 0x4E: return lena;
	// SPI
	case 0x51: return 0xBE;
	case 0x53: return 0x80 | spi53;
	// UART
	case 0x60: return 0xF0 | uctr;
	case 0x61: return usr | 0x80;
	case 0x62: return irctr | 0x38;
	case 0x63: return bctr | 0x78;
	case 0x66: return brs;
	case 0x67: return bdiv;
	default: return 0xFF;
	}
}

static void io_write(uint32_t a, uint8_t v)
{
	if (a >= 0x80)
		return;
	switch (a) {
	// GPIO
	case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
		pdata[a] = v;
		if (a == 0) pa_check();
		break;
	case 0x05: psc = v; break;
	case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
		pctrl[a - 8] = v;
		if (a == 8) pa_check();
		break;
	case 0x0D: pfc = v; break;
	case 0x0E: pfd = v; break;
	case 0x0F: pmcr = v; break;

	// PSG
	case 0x10: psg_sync(); psg0 = (psg0 & 0xF00) | v; break;
	case 0x11: psg_sync(); psg0 = (psg0 & 0x0FF) | ((v & 0xF) << 8); break;
	case 0x12: psg_sync(); psg1 = (psg1 & 0xF00) | v; sched_timers(); sched(); break;
	case 0x13: psg_sync(); psg1 = (psg1 & 0x0FF) | ((v & 0xF) << 8); sched_timers(); sched(); break;
	case 0x14: dac = v; break;
	case 0x16: psg_sync(); psgc = v; sched_timers(); sched(); break;
	case 0x17: vol = v; break;

	// Timers
	case 0x20: bten = v & 0x1F; break;
	case 0x21: if (v & 0x80) btsr = 0; break;
	case 0x23:
		timers_sync();
		prs = v & 0x60;
		if (v & 0x80) pres_p = 0;
		sched_timers(); sched();
		break;
	case 0x24: timers_sync(); t0m = v & 0x37; sched_timers(); sched(); break;
	case 0x25: timers_sync(); t0c = v; t0_count = v; sched_timers(); sched(); break;
	case 0x26: timers_sync(); t1m = v & 0x1F; sched_timers(); sched(); break;
	case 0x27: timers_sync(); t1c = v; t1_count = v; sched_timers(); sched(); break;

	// DMA
	case 0x28: dms = (dms & 0xFF00) | v; break;
	case 0x29: dms = (dms & 0x00FF) | (v << 8); break;
	case 0x2A: dmd = (dmd & 0xFF00) | v; break;
	case 0x2B: dmd = (dmd & 0x00FF) | (v << 8); break;
	case 0x2C: dma_run(((dcnth << 8) | v) + 1); break;
	case 0x2D: dcnth = v & 0xF; dmam = (v >> 4) & 1; break;
	case 0x36: dmr = (dmr & 0x700) | v; break;
	case 0x37: dmr = (dmr & 0x0FF) | ((v & 7) << 8); break;

	// SYS
	case 0x30: sys_r = v; update_windows(); break;
	case 0x38: misc_r = v & 0x1F; wdt_count = 0; break;
	case 0x39: r39 = v & 0xF; break;
	case 0x3A: r3a = v; break;

	// Banking
	case 0x31: irr = v; update_windows(); break;
	case 0x32: prrl = v; update_windows(); break;
	case 0x33: prrh = v & 7; update_windows(); break;
	case 0x34: drrl = v; update_windows(); break;
	case 0x35: drrh = v & 7; update_windows(); break;

	// IRQ: writing 0 bits clears pending requests.
	case 0x3C: timers_sync(); psg_sync(); ireq &= 0xF00 | v; update_irq(); break;
	case 0x3D: timers_sync(); psg_sync(); ireq &= 0x0FF | ((v & 0xF) << 8); update_irq(); break;
	case 0x3E:
		timers_sync(); psg_sync();
		iena = (iena & 0xF00) | (v & IREQ_MASK & 0xFF);
		update_irq(); sched_timers(); sched();
		break;
	case 0x3F:
		iena = (iena & 0x0FF) | ((v & 0xF) << 8 & IREQ_MASK);
		update_irq();
		break;

	// LCDC
	case 0x40: lssa = (lssa & 0xFF00) | v; break;
	case 0x41: lssa = (lssa & 0x00FF) | (v << 8); break;
	case 0x42: lvpw = v; break;
	case 0x43: lxmax = v; lcd_regs_changed(); sched(); break;
	case 0x44: lymax = v; lcd_regs_changed(); sched(); break;
	case 0x45: lpan = v & 7; break;
	case 0x46: lar = v; break;
	case 0x47: lctr = v; lcd_regs_changed(); sched(); break;
	case 0x48: lckr = v; lcd_regs_changed(); sched(); break;
	case 0x49: lfra = v & 0x3F; lcd_regs_changed(); sched(); break;
	case 0x4A: lac = v & 0x1F; break;
	case 0x4B: lpwm = v & 0x3F; break;
	case 0x4C: lmisc = v; break;
	case 0x4E: lena = v; break;

	// SPI / UART latches
	case 0x53: spi53 = v & 0x7F; break;
	case 0x60: uctr = v & 0xF; break;
	case 0x62: irctr = v & 0xC7; break;
	case 0x63: bctr = v & 0x87; break;
	case 0x66: brs = v; break;
	case 0x67: bdiv = v; break;
	default: break;
	}
}

// Generic bus access (DMA, debug).
static uint8_t bus_read(uint32_t a)
{
	a &= 0xFFFF;
	if (a >= 0x4000)
		return g_win[a >> 14][a & g_win_mask[a >> 14]];
	if (a >= 0x80 && a < 0x2800)
		return g_ram[a];
	return io_read(a);
}

static void bus_write(uint32_t a, uint8_t v)
{
	a &= 0xFFFF;
	if (a >= 0x80 && a < 0x2800)
		g_ram[a] = v;
	else if (a < 0x80)
		io_write(a, v);
}

// ---------------------------------------------------------------------
// Interrupt plumbing (st_irq.sv)
// ---------------------------------------------------------------------
static uint32_t vec_fetch(uint32_t vaddr)
{
	// Vectors read through the program window ($7Fxx).
	uint32_t a = vaddr & 0x3FFF;
	uint32_t l = g_win[1][a & g_win_mask[1]];
	uint32_t h = g_win[1][(a + 1) & g_win_mask[1]];
	return l | (h << 8);
}

static uint32_t irq_enter(void)
{
	timers_sync();
	psg_sync();
	uint32_t act = ireq & iena & IREQ_MASK;
	uint32_t level = 7;   // spurious -> reserved slot
	for (uint32_t l = 8; l < 12; l++)
		if (act & (1u << l)) { level = l; goto found; }
	for (uint32_t l = 0; l < 7; l++)
		if (act & (1u << l)) { level = l; break; }
found:
	irq_service = 1;
	update_windows();
	ireq &= ~(1u << level);
	update_irq();
	return vec_fetch(0x7FF8 - 2 * level);
}

static uint32_t brk_enter(void)
{
	irq_service = 1;
	update_windows();
	return vec_fetch(0x7FFE);
}

static void rti_done(void)
{
	if (irq_service) {
		irq_service = 0;
		update_windows();
	}
}

// ---------------------------------------------------------------------
// CPU instantiation: SoC memory map
// ---------------------------------------------------------------------
#define CPU_FUNC cpu_exec_soc
#define RD(addr) ({ uint32_t _a = (addr); uint32_t _r; \
	if (_a >= 0x4000) _r = g_win[_a >> 14][_a & g_win_mask[_a >> 14]]; \
	else if (_a >= 0x80 && _a < 0x2800) _r = g_ram[_a]; \
	else { SYNC_OUT(); _r = io_read(_a); SYNC_IN(); } _r; })
#define WR(addr, val) do { uint32_t _a = (addr); uint8_t _v = (uint8_t)(val); \
	if (_a >= 0x80 && _a < 0x2800) g_ram[_a] = _v; \
	else if (_a < 0x80) { SYNC_OUT(); io_write(_a, _v); SYNC_IN(); } } while (0)
#define STK (g_ram + 0x100)
#define IRQ_LINE g_irq_line
#define CPU_IRQ_ENTER() do { PC = irq_enter(); } while (0)
#define CPU_BRK_ENTER() do { PC = brk_enter(); } while (0)
#define CPU_RTI_DONE()  rti_done()
static void GK_FAST(cpu_exec_soc)(void);
#include "gk_cpu.inc"
#undef CPU_FUNC
#undef RD
#undef WR
#undef STK
#undef IRQ_LINE
#undef CPU_IRQ_ENTER
#undef CPU_BRK_ENTER
#undef CPU_RTI_DONE

// ---------------------------------------------------------------------
// CPU instantiation: flat 64 KB test memory (host builds only)
// ---------------------------------------------------------------------
#ifndef GK_PICO
static uint8_t *g_flat;
#define CPU_FUNC cpu_exec_flat
#define RD(addr) (g_flat[(addr)])
#define WR(addr, val) do { g_flat[(addr)] = (uint8_t)(val); } while (0)
#define STK (g_flat + 0x100)
#define IRQ_LINE 0
#define CPU_IRQ_ENTER() do { } while (0)
#define CPU_BRK_ENTER() do { PC = g_flat[0xFFFE] | (g_flat[0xFFFF] << 8); } while (0)
#define CPU_RTI_DONE()  do { } while (0)
#include "gk_cpu.inc"
#undef CPU_FUNC
#undef RD
#undef WR
#undef STK
#undef IRQ_LINE
#undef CPU_IRQ_ENTER
#undef CPU_BRK_ENTER
#undef CPU_RTI_DONE
#endif

// ---------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------
static inline uint64_t t32k_time(uint64_t n)
{
	// 6e6 / 32768 = 46875 / 256 cycles per tick.
	return (n * 46875u) >> 8;
}

static void soc_reset(int cold);

static void tick_32k(void)
{
	// One 32.768 kHz tick: base timer, timer 1 OSCX path, watchdog.
	uint32_t c = bt_chain;
	uint32_t fire = 0;
	if ((c & 0xF) == 0xF)    fire |= 0x10;
	if ((c & 0x7F) == 0x7F)  fire |= 0x08;
	if ((c & 0x1FF) == 0x1FF) fire |= 0x04;
	if ((c & 0xFFF) == 0xFFF) fire |= 0x02;
	int tap2 = (c & 0x3FFF) == 0x3FFF;
	if (tap2) fire |= 0x01;
	fire &= bten;
	if (fire) {
		if (btsr == 0)
			raise_irq(5);
		btsr |= fire;
	}
	bt_chain = (c + 1) & 0x7FFF;

	if ((t1m & 8) && (t1m & 3) != 3) {
		uint32_t target = 255u >> (t1m & 3);
		if (t1_oscx_div >= target) {
			t1_oscx_div = 0;
			if (t1m & 0x10) {
				raise_irq(3);
			} else {
				if (t1_oscx_256 == 0xFF)
					raise_irq(3);
				t1_oscx_256++;
			}
		} else {
			t1_oscx_div++;
		}
	}

	if (misc_r & 8) {
		if (tap2) {
			if (wdt_count == ((misc_r & 4) ? 15 : 3)) {
				wdt_count = 0;
				soc_reset(0);
			} else {
				wdt_count++;
			}
		}
	} else {
		wdt_count = 0;
	}

	t32k_n++;
}

// The 32.768 kHz domain is processed in blocks of 16 ticks (2048 Hz, the
// fastest base-timer tap), which keeps the event rate low.
#define T32K_BATCH 16
static void ev_32k(void)
{
	for (int i = 0; i < T32K_BATCH; i++)
		tick_32k();
	ev_t[EV_32K] = t32k_time(t32k_n + T32K_BATCH);
}

static void ev_sample(void)
{
	psg_sync();
	gk_plat_audio(psg_sample());
	ev_t[EV_SAMPLE] += GK_AUDIO_DIV;
}

static void ev_lcd(void)
{
	lcd_scan_phase = (lcd_scan_phase + 1) % 3;
	raise_irq(6);
	gk_plat_frame();
	ev_t[EV_LCD] += lcd_period;
}

static void sched_timers(void)
{
	uint64_t best = EV_NEVER;
	uint64_t t = now();
	if (prs_run()) {
		if ((t0m & 0x20) && (iena & 4)) {
			uint64_t d = timer_next(t0m, t0_count);
			if (d < best) best = d;
		}
		if (t1_prs_mode() && (iena & 8)) {
			uint64_t d = timer_next(t1m, t1_count);
			if (d < best) best = d;
		}
		// timer_next is relative to the last sync point.
		if (best != EV_NEVER) best += pres_t;
	}
	if (iena & 2) {
		uint64_t d = psg_next_int();
		if (d != EV_NEVER && psg_t + d < best)
			best = psg_t + d;
	}
	if (best != EV_NEVER && best < t + 1)
		best = t + 1;
	ev_t[EV_TIMERS] = best;
}

static void sched(void)
{
	uint64_t t = now();
	uint64_t m = ev_t[0];
	for (int i = 1; i < EV_N; i++)
		if (ev_t[i] < m) m = ev_t[i];
	g_evt_time = m;
	g_left = (int32_t)((int64_t)m - (int64_t)t);
}

static void run_events(void)
{
	uint64_t t = now();
	if (ev_t[EV_32K] <= t)    ev_32k();
	if (ev_t[EV_SAMPLE] <= t) ev_sample();
	if (ev_t[EV_LCD] <= t)    ev_lcd();
	if (ev_t[EV_TIMERS] <= t) { timers_sync(); psg_sync(); sched_timers(); }
}

// ---------------------------------------------------------------------
// Reset / init
// ---------------------------------------------------------------------
static void soc_reset(int cold)
{
	uint64_t t = now();
	if (cold)
		memset(g_ram, 0, sizeof g_ram);

	irr = prrl = prrh = drrl = drrh = 0;
	irq_service = 0;
	sys_r = 0; misc_r = 0; r39 = 0; r3a = 0xFF;
	ireq = iena = 0;
	for (int i = 0; i < 5; i++) { pdata[i] = 0xFF; pctrl[i] = 0; }
	psc = 0xFF; pfc = 0; pfd = 0; pmcr = 0x80;
	pa_last = 0xFF; pa_armed = 0;

	bten = btsr = prs = t0m = t0c = t1m = t1c = 0;
	t0_count = t1_count = t1_oscx_div = t1_oscx_256 = wdt_count = 0;
	bt_chain = 0;
	pres_p = 0; pres_t = t;

	psg0 = psg1 = 0; dac = 0; psgc = 0x0D; vol = 0;
	sq0_count = sq1_count = sq0_phase = sq1_phase = 0;
	lfsr = 0x7FFF;
	pwm_r = 0x40; pwm_count = 0; smp_count = 0;
	psg_t = t; psg_32k = (t * 256) / 46875; psg_rem = 0;

	dms = dmd = dcnth = dmam = dmr = 0;

	lssa = lvpw = lxmax = lymax = lpan = 0;
	lar = 0xFF; lctr = 0x80; lckr = 0; lfra = 0; lac = 0; lpwm = 0;
	lmisc = 0; lena = 0xF1;
	lcd_scan_phase = 0;
	lcd_period = 0;
	ev_t[EV_LCD] = EV_NEVER;

	spi53 = 0;
	uctr = usr = irctr = bctr = brs = bdiv = 0;

	update_irq();
	update_windows();

	memset(&cpu, 0, sizeof cpu);
	cpu.s = 0xFD;
	cpu.i = 1;
	cpu.z = 1;
	cpu.pc = vec_fetch(0x7FFC);

	sched_timers();
	sched();
}

void gk_init(const uint8_t *bios, const uint8_t *rom, uint32_t romSize)
{
	memset(g_ff_page, 0xFF, sizeof g_ff_page);
	g_bios = bios;
	g_rom = (rom && romSize) ? rom : 0;
	uint32_t m = 1;
	while (m < romSize)
		m <<= 1;
	g_rom_mask = m - 1;

	g_evt_time = 0;
	g_left = 0;
	t32k_n = 0;
	ev_t[EV_32K] = t32k_time(16);
	ev_t[EV_SAMPLE] = GK_AUDIO_DIV;
	ev_t[EV_TIMERS] = EV_NEVER;
	soc_reset(1);
}

void gk_reset(void) { soc_reset(1); }

void GK_FAST(gk_run)(uint32_t cycles)
{
	uint64_t end = now() + cycles;
	while (now() < end) {
		if (g_left > 0)
			cpu_exec_soc();
		run_events();
		sched();
	}
}

void gk_set_buttons(uint8_t b)
{
	// PA0 = Select, PA1 = Start; PB2 = A, PB3 = B, PB4-7 = U/D/L/R.
	// Buttons switch the net to ground.
	ext_pa = 0xFF & ~(((b & GK_BTN_SELECT) ? 1 : 0) | ((b & GK_BTN_START) ? 2 : 0));
	ext_pb = 0xFF & ~(((b & GK_BTN_A) ? 0x04 : 0) | ((b & GK_BTN_B) ? 0x08 : 0) |
		((b & GK_BTN_UP) ? 0x10 : 0) | ((b & GK_BTN_DOWN) ? 0x20 : 0) |
		((b & GK_BTN_LEFT) ? 0x40 : 0) | ((b & GK_BTN_RIGHT) ? 0x80 : 0));
	pa_check();
}

uint64_t gk_cycles(void) { return now(); }

uint8_t gk_peek(uint16_t a)
{
	if (a >= 0x4000)
		return g_win[a >> 14][a & g_win_mask[a >> 14]];
	return g_ram[a & 0x3FFF];
}

void gk_cpu_state(uint16_t *pc, uint8_t *a, uint8_t *x, uint8_t *y,
                  uint8_t *s, uint8_t *p)
{
	*pc = cpu.pc; *a = cpu.a; *x = cpu.x; *y = cpu.y; *s = cpu.s;
	*p = (cpu.n & 0x80) | (cpu.v << 6) | 0x30 | (cpu.d << 3) | (cpu.i << 2) |
		((cpu.z & 0xFF) == 0 ? 2 : 0) | cpu.c;
}

#ifndef GK_PICO
void gk_test_flat(uint8_t *mem, uint16_t pc)
{
	g_flat = mem;
	memset(&cpu, 0, sizeof cpu);
	cpu.pc = pc;
	cpu.s = 0xFF;
	cpu.z = 1;
	cpu.i = 1;
}

void gk_test_step(uint32_t cycles)
{
	g_left = (int32_t)cycles;
	cpu_exec_flat();
}
#endif
