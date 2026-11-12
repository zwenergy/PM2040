// W65C02S interpreter body (ST2204 core), included by gk.c.
//
// The includer defines:
//   CPU_FUNC            name of the generated function
//   RD(a) / WR(a, v)    memory access
//   STK                 pointer to the 256-byte stack page ($0100)
//   IRQ_LINE            expression: interrupt request asserted
//   CPU_IRQ_ENTER()     hardware IRQ vector fetch -> sets `PC`
//   CPU_BRK_ENTER()     BRK vector fetch -> sets `PC`
//   CPU_RTI_DONE()      hook after RTI
//
// Registers live in the global `cpu`; the budget in `g_left` (cycles left
// until the next scheduled event). The function returns once g_left <= 0.
// I/O handlers called from RD/WR may read or change g_left, so the local
// copy is written back around them (SYNC_OUT/SYNC_IN).

static void CPU_FUNC(void)
{
	uint32_t A = cpu.a, X = cpu.x, Y = cpu.y, S = cpu.s, PC = cpu.pc;
	uint32_t fC = cpu.c, fV = cpu.v, fD = cpu.d, fI = cpu.i;
	uint32_t fN = cpu.n, fZ = cpu.z;   // N = bit 7 of fN, Z = (fZ == 0)
	int32_t left = g_left;

#define SYNC_OUT() do { g_left = left; } while (0)
#define SYNC_IN()  do { left = g_left; } while (0)
#define FETCH()    ({ uint32_t _v = RD(PC); PC = (PC + 1) & 0xFFFF; _v; })
#define FETCH16()  ({ uint32_t _l = FETCH(); uint32_t _h = FETCH(); _l | (_h << 8); })
#define PUSH(v)    do { STK[S] = (uint8_t)(v); S = (S - 1) & 0xFF; } while (0)
#define PULL()     ({ S = (S + 1) & 0xFF; (uint32_t)STK[S]; })
#define GETP()     ((fN & 0x80) | (fV << 6) | 0x30 | (fD << 3) | (fI << 2) | \
                    ((fZ & 0xFF) == 0 ? 2 : 0) | fC)
#define SETP(p)    do { uint32_t _p = (p); fN = _p; fV = (_p >> 6) & 1; \
                        fD = (_p >> 3) & 1; fI = (_p >> 2) & 1; \
                        fZ = (_p & 2) ? 0 : 1; fC = _p & 1; } while (0)
#define SETNZ(v)   do { fN = fZ = (v); } while (0)
#define CYC(n)     do { left -= (n); } while (0)

	// Effective addresses.
#define EA_ZP()    (FETCH())
#define EA_ZPX()   ((FETCH() + X) & 0xFF)
#define EA_ZPY()   ((FETCH() + Y) & 0xFF)
#define EA_ABS()   (FETCH16())
#define ZP16(z)    ({ uint32_t _z = (z); uint32_t _l = RD(_z); \
                      _l | (RD((_z + 1) & 0xFF) << 8); })
#define EA_IZX()   (ZP16((FETCH() + X) & 0xFF))
#define EA_IZP()   (ZP16(FETCH()))
	// Indexed with page-cross penalty (reads).
#define EA_ABSX_P() ({ uint32_t _b = FETCH16(); uint32_t _e = _b + X; \
                       if ((_b ^ _e) & 0x100) CYC(1); _e & 0xFFFF; })
#define EA_ABSY_P() ({ uint32_t _b = FETCH16(); uint32_t _e = _b + Y; \
                       if ((_b ^ _e) & 0x100) CYC(1); _e & 0xFFFF; })
#define EA_IZY_P()  ({ uint32_t _b = EA_IZP(); uint32_t _e = _b + Y; \
                       if ((_b ^ _e) & 0x100) CYC(1); _e & 0xFFFF; })
#define EA_ABSX()   ((FETCH16() + X) & 0xFFFF)
#define EA_ABSY()   ((FETCH16() + Y) & 0xFFFF)
#define EA_IZY()    ((EA_IZP() + Y) & 0xFFFF)

	// ALU.
#define OP_ORA(m)  do { A |= (m); SETNZ(A); } while (0)
#define OP_AND(m)  do { A &= (m); SETNZ(A); } while (0)
#define OP_EOR(m)  do { A ^= (m); SETNZ(A); } while (0)
#define OP_CMPR(r, m) do { uint32_t _t = (r) - (m); fC = (_t < 0x100); \
                           SETNZ(_t & 0xFF); } while (0)
#define OP_BIT(m)  do { uint32_t _m = (m); fN = _m; fV = (_m >> 6) & 1; \
                        fZ = A & _m; } while (0)
#define OP_ADC(m)  do { uint32_t _m = (m); \
		if (!fD) { \
			uint32_t _t = A + _m + fC; \
			fV = ((~(A ^ _m) & (A ^ _t)) >> 7) & 1; \
			fC = _t >> 8; A = _t & 0xFF; SETNZ(A); \
		} else { \
			uint32_t _lo = (A & 0x0F) + (_m & 0x0F) + fC; \
			uint32_t _hi = (A & 0xF0) + (_m & 0xF0); \
			if (_lo > 0x09) { _hi += 0x10; _lo += 0x06; } \
			fV = ((~(A ^ _m) & (A ^ _hi)) >> 7) & 1; \
			if (_hi > 0x90) _hi += 0x60; \
			fC = _hi >> 8; A = ((_lo & 0x0F) | _hi) & 0xFF; SETNZ(A); \
			CYC(1); \
		} } while (0)
#define OP_SBC(m)  do { uint32_t _m = (m); \
		uint32_t _t = A - _m - (fC ^ 1); \
		fV = (((A ^ _m) & (A ^ _t)) >> 7) & 1; \
		if (!fD) { \
			fC = (_t < 0x100); A = _t & 0xFF; SETNZ(A); \
		} else { \
			int32_t _lo = (int32_t)(A & 0x0F) - (int32_t)(_m & 0x0F) - (int32_t)(fC ^ 1); \
			int32_t _r = (int32_t)A - (int32_t)_m - (int32_t)(fC ^ 1); \
			if (_r < 0) _r -= 0x60; \
			if (_lo < 0) _r -= 0x06; \
			fC = (_t < 0x100); A = (uint32_t)_r & 0xFF; SETNZ(A); \
			CYC(1); \
		} } while (0)
#define ASL_V(v)   ({ uint32_t _v = (v); fC = _v >> 7; _v = (_v << 1) & 0xFF; SETNZ(_v); _v; })
#define LSR_V(v)   ({ uint32_t _v = (v); fC = _v & 1; _v >>= 1; SETNZ(_v); _v; })
#define ROL_V(v)   ({ uint32_t _v = (v); uint32_t _c = fC; fC = _v >> 7; \
                      _v = ((_v << 1) | _c) & 0xFF; SETNZ(_v); _v; })
#define ROR_V(v)   ({ uint32_t _v = (v); uint32_t _c = fC; fC = _v & 1; \
                      _v = (_v >> 1) | (_c << 7); SETNZ(_v); _v; })
#define INC_V(v)   ({ uint32_t _v = ((v) + 1) & 0xFF; SETNZ(_v); _v; })
#define DEC_V(v)   ({ uint32_t _v = ((v) - 1) & 0xFF; SETNZ(_v); _v; })
#define RMW(ea, fn, cyc) do { uint32_t _re = (ea); uint32_t _rv = RD(_re); \
                              _rv = fn(_rv); WR(_re, _rv); CYC(cyc); } while (0)

#define BRANCH(cond) do { int32_t _o = (int8_t)FETCH(); CYC(2); \
		if (cond) { uint32_t _n = (PC + _o) & 0xFFFF; \
			CYC(((_n ^ PC) & 0x100) ? 2 : 1); PC = _n; } } while (0)

#define CASE_ALU(base, OP) \
	case base + 0x01: OP(RD(EA_IZX()));   CYC(6); break; \
	case base + 0x05: OP(RD(EA_ZP()));    CYC(3); break; \
	case base + 0x09: OP(FETCH());        CYC(2); break; \
	case base + 0x0D: OP(RD(EA_ABS()));   CYC(4); break; \
	case base + 0x11: OP(RD(EA_IZY_P())); CYC(5); break; \
	case base + 0x12: OP(RD(EA_IZP()));   CYC(5); break; \
	case base + 0x15: OP(RD(EA_ZPX()));   CYC(4); break; \
	case base + 0x19: OP(RD(EA_ABSY_P())); CYC(4); break; \
	case base + 0x1D: OP(RD(EA_ABSX_P())); CYC(4); break;

#define CASE_SHIFT(base, FN) \
	case base + 0x06: RMW(EA_ZP(), FN, 5);  break; \
	case base + 0x0A: A = FN(A); CYC(2);    break; \
	case base + 0x0E: RMW(EA_ABS(), FN, 6); break; \
	case base + 0x16: RMW(EA_ZPX(), FN, 6); break; \
	case base + 0x1E: RMW(EA_ABSX(), FN, 6); break;

#define CASE_BIT_OPS(n) \
	case 0x07 + (n) * 0x10: { uint32_t _e = EA_ZP(); WR(_e, RD(_e) & ~(1u << (n))); CYC(5); break; } \
	case 0x87 + (n) * 0x10: { uint32_t _e = EA_ZP(); WR(_e, RD(_e) | (1u << (n))); CYC(5); break; } \
	case 0x0F + (n) * 0x10: { uint32_t _v = RD(EA_ZP()); CYC(3); BRANCH(!(_v & (1u << (n)))); break; } \
	case 0x8F + (n) * 0x10: { uint32_t _v = RD(EA_ZP()); CYC(3); BRANCH(_v & (1u << (n))); break; }

	while (left > 0) {
		if (IRQ_LINE) {
			cpu.wai = 0;
			if (!fI) {
				PUSH(PC >> 8); PUSH(PC & 0xFF);
				PUSH(GETP() & ~0x10);
				fI = 1; fD = 0;
				SYNC_OUT();
				CPU_IRQ_ENTER();
				SYNC_IN();
				CYC(7);
				continue;
			}
		} else if (cpu.wai) {
			left = 0;   // sleep until the next event
			break;
		}

		uint32_t op = FETCH();
		switch (op) {
		CASE_ALU(0x00, OP_ORA)
		CASE_ALU(0x20, OP_AND)
		CASE_ALU(0x40, OP_EOR)
		CASE_ALU(0x60, OP_ADC)
		CASE_ALU(0xE0, OP_SBC)
		case 0xC1: OP_CMPR(A, RD(EA_IZX()));   CYC(6); break;
		case 0xC5: OP_CMPR(A, RD(EA_ZP()));    CYC(3); break;
		case 0xC9: OP_CMPR(A, FETCH());        CYC(2); break;
		case 0xCD: OP_CMPR(A, RD(EA_ABS()));   CYC(4); break;
		case 0xD1: OP_CMPR(A, RD(EA_IZY_P())); CYC(5); break;
		case 0xD2: OP_CMPR(A, RD(EA_IZP()));   CYC(5); break;
		case 0xD5: OP_CMPR(A, RD(EA_ZPX()));   CYC(4); break;
		case 0xD9: OP_CMPR(A, RD(EA_ABSY_P())); CYC(4); break;
		case 0xDD: OP_CMPR(A, RD(EA_ABSX_P())); CYC(4); break;
		case 0xE0: OP_CMPR(X, FETCH());        CYC(2); break;
		case 0xE4: OP_CMPR(X, RD(EA_ZP()));    CYC(3); break;
		case 0xEC: OP_CMPR(X, RD(EA_ABS()));   CYC(4); break;
		case 0xC0: OP_CMPR(Y, FETCH());        CYC(2); break;
		case 0xC4: OP_CMPR(Y, RD(EA_ZP()));    CYC(3); break;
		case 0xCC: OP_CMPR(Y, RD(EA_ABS()));   CYC(4); break;

		// STA
		case 0x81: WR(EA_IZX(), A);  CYC(6); break;
		case 0x85: WR(EA_ZP(), A);   CYC(3); break;
		case 0x8D: WR(EA_ABS(), A);  CYC(4); break;
		case 0x91: WR(EA_IZY(), A);  CYC(6); break;
		case 0x92: WR(EA_IZP(), A);  CYC(5); break;
		case 0x95: WR(EA_ZPX(), A);  CYC(4); break;
		case 0x99: WR(EA_ABSY(), A); CYC(5); break;
		case 0x9D: WR(EA_ABSX(), A); CYC(5); break;
		// STX / STY / STZ
		case 0x86: WR(EA_ZP(), X);   CYC(3); break;
		case 0x8E: WR(EA_ABS(), X);  CYC(4); break;
		case 0x96: WR(EA_ZPY(), X);  CYC(4); break;
		case 0x84: WR(EA_ZP(), Y);   CYC(3); break;
		case 0x8C: WR(EA_ABS(), Y);  CYC(4); break;
		case 0x94: WR(EA_ZPX(), Y);  CYC(4); break;
		case 0x64: WR(EA_ZP(), 0);   CYC(3); break;
		case 0x74: WR(EA_ZPX(), 0);  CYC(4); break;
		case 0x9C: WR(EA_ABS(), 0);  CYC(4); break;
		case 0x9E: WR(EA_ABSX(), 0); CYC(5); break;

		// LDA
		case 0xA1: A = RD(EA_IZX());    SETNZ(A); CYC(6); break;
		case 0xA5: A = RD(EA_ZP());     SETNZ(A); CYC(3); break;
		case 0xA9: A = FETCH();         SETNZ(A); CYC(2); break;
		case 0xAD: A = RD(EA_ABS());    SETNZ(A); CYC(4); break;
		case 0xB1: A = RD(EA_IZY_P());  SETNZ(A); CYC(5); break;
		case 0xB2: A = RD(EA_IZP());    SETNZ(A); CYC(5); break;
		case 0xB5: A = RD(EA_ZPX());    SETNZ(A); CYC(4); break;
		case 0xB9: A = RD(EA_ABSY_P()); SETNZ(A); CYC(4); break;
		case 0xBD: A = RD(EA_ABSX_P()); SETNZ(A); CYC(4); break;
		// LDX
		case 0xA2: X = FETCH();         SETNZ(X); CYC(2); break;
		case 0xA6: X = RD(EA_ZP());     SETNZ(X); CYC(3); break;
		case 0xAE: X = RD(EA_ABS());    SETNZ(X); CYC(4); break;
		case 0xB6: X = RD(EA_ZPY());    SETNZ(X); CYC(4); break;
		case 0xBE: X = RD(EA_ABSY_P()); SETNZ(X); CYC(4); break;
		// LDY
		case 0xA0: Y = FETCH();         SETNZ(Y); CYC(2); break;
		case 0xA4: Y = RD(EA_ZP());     SETNZ(Y); CYC(3); break;
		case 0xAC: Y = RD(EA_ABS());    SETNZ(Y); CYC(4); break;
		case 0xB4: Y = RD(EA_ZPX());    SETNZ(Y); CYC(4); break;
		case 0xBC: Y = RD(EA_ABSX_P()); SETNZ(Y); CYC(4); break;

		// BIT
		case 0x24: OP_BIT(RD(EA_ZP()));     CYC(3); break;
		case 0x2C: OP_BIT(RD(EA_ABS()));    CYC(4); break;
		case 0x34: OP_BIT(RD(EA_ZPX()));    CYC(4); break;
		case 0x3C: OP_BIT(RD(EA_ABSX_P())); CYC(4); break;
		case 0x89: fZ = A & FETCH();        CYC(2); break;   // imm: Z only

		// Shifts / rotates / inc / dec
		CASE_SHIFT(0x00, ASL_V)
		CASE_SHIFT(0x20, ROL_V)
		CASE_SHIFT(0x40, LSR_V)
		CASE_SHIFT(0x60, ROR_V)
		case 0xC6: RMW(EA_ZP(), DEC_V, 5);   break;
		case 0xCE: RMW(EA_ABS(), DEC_V, 6);  break;
		case 0xD6: RMW(EA_ZPX(), DEC_V, 6);  break;
		case 0xDE: RMW(EA_ABSX(), DEC_V, 7); break;
		case 0xE6: RMW(EA_ZP(), INC_V, 5);   break;
		case 0xEE: RMW(EA_ABS(), INC_V, 6);  break;
		case 0xF6: RMW(EA_ZPX(), INC_V, 6);  break;
		case 0xFE: RMW(EA_ABSX(), INC_V, 7); break;
		case 0x1A: A = (A + 1) & 0xFF; SETNZ(A); CYC(2); break;
		case 0x3A: A = (A - 1) & 0xFF; SETNZ(A); CYC(2); break;
		case 0xE8: X = (X + 1) & 0xFF; SETNZ(X); CYC(2); break;
		case 0xCA: X = (X - 1) & 0xFF; SETNZ(X); CYC(2); break;
		case 0xC8: Y = (Y + 1) & 0xFF; SETNZ(Y); CYC(2); break;
		case 0x88: Y = (Y - 1) & 0xFF; SETNZ(Y); CYC(2); break;

		// TSB / TRB
		case 0x04: { uint32_t e = EA_ZP();  uint32_t v = RD(e); fZ = v & A; WR(e, v | A);  CYC(5); break; }
		case 0x0C: { uint32_t e = EA_ABS(); uint32_t v = RD(e); fZ = v & A; WR(e, v | A);  CYC(6); break; }
		case 0x14: { uint32_t e = EA_ZP();  uint32_t v = RD(e); fZ = v & A; WR(e, v & ~A); CYC(5); break; }
		case 0x1C: { uint32_t e = EA_ABS(); uint32_t v = RD(e); fZ = v & A; WR(e, v & ~A); CYC(6); break; }

		// Transfers
		case 0xAA: X = A; SETNZ(X); CYC(2); break;
		case 0x8A: A = X; SETNZ(A); CYC(2); break;
		case 0xA8: Y = A; SETNZ(Y); CYC(2); break;
		case 0x98: A = Y; SETNZ(A); CYC(2); break;
		case 0xBA: X = S; SETNZ(X); CYC(2); break;
		case 0x9A: S = X; CYC(2); break;

		// Stack
		case 0x48: PUSH(A); CYC(3); break;
		case 0xDA: PUSH(X); CYC(3); break;
		case 0x5A: PUSH(Y); CYC(3); break;
		case 0x08: PUSH(GETP()); CYC(3); break;
		case 0x68: A = PULL(); SETNZ(A); CYC(4); break;
		case 0xFA: X = PULL(); SETNZ(X); CYC(4); break;
		case 0x7A: Y = PULL(); SETNZ(Y); CYC(4); break;
		case 0x28: SETP(PULL()); CYC(4); break;

		// Flags
		case 0x18: fC = 0; CYC(2); break;
		case 0x38: fC = 1; CYC(2); break;
		case 0x58: fI = 0; CYC(2); break;
		case 0x78: fI = 1; CYC(2); break;
		case 0xB8: fV = 0; CYC(2); break;
		case 0xD8: fD = 0; CYC(2); break;
		case 0xF8: fD = 1; CYC(2); break;

		// Branches
		case 0x10: BRANCH(!(fN & 0x80)); break;
		case 0x30: BRANCH(fN & 0x80);    break;
		case 0x50: BRANCH(!fV);          break;
		case 0x70: BRANCH(fV);           break;
		case 0x90: BRANCH(!fC);          break;
		case 0xB0: BRANCH(fC);           break;
		case 0xD0: BRANCH(fZ & 0xFF);    break;
		case 0xF0: BRANCH(!(fZ & 0xFF)); break;
		case 0x80: BRANCH(1);            break;

		CASE_BIT_OPS(0) CASE_BIT_OPS(1) CASE_BIT_OPS(2) CASE_BIT_OPS(3)
		CASE_BIT_OPS(4) CASE_BIT_OPS(5) CASE_BIT_OPS(6) CASE_BIT_OPS(7)

		// Jumps / calls
		case 0x4C: PC = FETCH16(); CYC(3); break;
		case 0x6C: { uint32_t p = FETCH16(); uint32_t l = RD(p);
			PC = l | (RD((p + 1) & 0xFFFF) << 8); CYC(6); break; }
		case 0x7C: { uint32_t p = (FETCH16() + X) & 0xFFFF; uint32_t l = RD(p);
			PC = l | (RD((p + 1) & 0xFFFF) << 8); CYC(6); break; }
		case 0x20: { uint32_t l = FETCH(); PUSH(PC >> 8); PUSH(PC & 0xFF);
			uint32_t h = RD(PC); PC = l | (h << 8); CYC(6); break; }
		case 0x60: { uint32_t l = PULL(); uint32_t h = PULL();
			PC = ((l | (h << 8)) + 1) & 0xFFFF; CYC(6); break; }
		case 0x40: { SETP(PULL()); uint32_t l = PULL(); uint32_t h = PULL();
			PC = l | (h << 8); CYC(6);
			SYNC_OUT(); CPU_RTI_DONE(); SYNC_IN(); break; }
		case 0x00: {
			PC = (PC + 1) & 0xFFFF;
			PUSH(PC >> 8); PUSH(PC & 0xFF); PUSH(GETP() | 0x10);
			fI = 1; fD = 0;
			SYNC_OUT(); CPU_BRK_ENTER(); SYNC_IN();
			CYC(7); break; }

		case 0xEA: CYC(2); break;   // NOP
		case 0xCB:                   // WAI
			CYC(3);
			if (!(IRQ_LINE)) { cpu.wai = 1; left = 0; }
			break;
		case 0xDB: CYC(3); break;   // STP: executed as NOP (no power switch)

		// Undefined opcodes: W65C02S NOPs.
		case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xC2: case 0xE2:
			PC = (PC + 1) & 0xFFFF; CYC(2); break;
		case 0x44: RD(EA_ZP()); CYC(3); break;
		case 0x54: case 0xD4: case 0xF4: RD(EA_ZPX()); CYC(4); break;
		case 0x5C: PC = (PC + 2) & 0xFFFF; CYC(8); break;
		case 0xDC: case 0xFC: PC = (PC + 2) & 0xFFFF; CYC(4); break;
		default: CYC(1); break;     // $x3 / $xB: one-cycle NOPs
		}
	}

	cpu.a = A; cpu.x = X; cpu.y = Y; cpu.s = S; cpu.pc = PC;
	cpu.c = fC; cpu.v = fV; cpu.d = fD; cpu.i = fI; cpu.n = fN; cpu.z = fZ;
	g_left = left;

#undef SYNC_OUT
#undef SYNC_IN
#undef FETCH
#undef FETCH16
#undef PUSH
#undef PULL
#undef GETP
#undef SETP
#undef SETNZ
#undef CYC
#undef EA_ZP
#undef EA_ZPX
#undef EA_ZPY
#undef EA_ABS
#undef ZP16
#undef EA_IZX
#undef EA_IZP
#undef EA_ABSX_P
#undef EA_ABSY_P
#undef EA_IZY_P
#undef EA_ABSX
#undef EA_ABSY
#undef EA_IZY
#undef OP_ORA
#undef OP_AND
#undef OP_EOR
#undef OP_CMPR
#undef OP_BIT
#undef OP_ADC
#undef OP_SBC
#undef ASL_V
#undef LSR_V
#undef ROL_V
#undef ROR_V
#undef INC_V
#undef DEC_V
#undef RMW
#undef BRANCH
#undef CASE_ALU
#undef CASE_SHIFT
#undef CASE_BIT_OPS
}
