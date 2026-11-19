/*
 * Portable MCS-48 (8049) core for the SH-101 emulator.
 * Decoder from cozycactus/sh101_midi, commit 8142852 (src/mcs48_core.h).
 * ============================================================================
 * The decoder here is platform-independent and IDENTICAL on every target
 * (validated on the host via test/emu_trace.c). Each platform provides:
 *   - cpu and the ROM image (definitions)
 *   - the I/O hooks bus_out / p1_in / p2_out / t0_in / t1_in
 *   - pace(mc)  (the cycle-exact wait; also ticks the internal timer)
 * See sh101_emu.c (Raspberry Pi Pico) and sh101_emu_stm32.c (STM32F103).
 */
#ifndef MCS48_CORE_H
#define MCS48_CORE_H

#include <stdint.h>
#include <stdbool.h>
extern uint8_t sh101_rom[2048];

typedef struct {
    uint8_t  ram[128];      // 0-7/24-31 = reg banks, 8-23 = 8-level stack, 32+ = data
    uint16_t pc;            // 11-bit PC (2 KB ROM)
    uint8_t  a, psw;        // accumulator; PSW = CY AC F0 BS 1 SP2 SP1 SP0
    uint8_t  p1_latch, p2, bus;
    uint8_t  t;             // timer/counter
    uint16_t tprescale;
    bool     f1, timer_run, counter_run, tf, tcnti_en, in_irq, dbf, last_t1;
    uint64_t mcycles;
} cpu_t;

extern cpu_t cpu;                      // defined by the platform

// ---- platform hooks (defined per platform) ----------------------------------
void bus_out(uint8_t v);
uint8_t p1_in(void);
void p2_out(uint8_t v);
bool t0_in(void);
bool t1_in(void);
void pace(uint8_t mc);

// ---- core helpers -----------------------------------------------------------
static inline uint8_t *regs(void) { return &cpu.ram[(cpu.psw & 0x10) ? 24 : 0]; }  // bank by PSW.BS
static inline uint8_t *atR(int n) { return &cpu.ram[regs()[n] & 0x7F]; }           // @R0/@R1
#define CY (cpu.psw & 0x80)
static inline void set_cy(int c) { cpu.psw = (uint8_t)((cpu.psw & ~0x80) | (c ? 0x80 : 0)); }
static inline void set_ac(int c) { cpu.psw = (uint8_t)((cpu.psw & ~0x40) | (c ? 0x40 : 0)); }
static inline void do_add(uint8_t v, int ci) {
    unsigned lo = (cpu.a & 0xF) + (v & 0xF) + (unsigned)ci, full = cpu.a + v + (unsigned)ci;
    set_ac(lo > 0xF); set_cy(full > 0xFF); cpu.a = (uint8_t)full;
}
static inline void push(void) {
    uint8_t sp = cpu.psw & 7;
    cpu.ram[8 + sp * 2]     = (uint8_t)cpu.pc;
    cpu.ram[8 + sp * 2 + 1] = (uint8_t)((cpu.psw & 0xF0) | ((cpu.pc >> 8) & 0x0F));
    cpu.psw = (uint8_t)((cpu.psw & 0xF8) | ((sp + 1) & 7));
}
static inline void pop_pc(int restore_psw) {
    uint8_t sp = (cpu.psw - 1) & 7;
    uint8_t lo = cpu.ram[8 + sp * 2], hi = cpu.ram[8 + sp * 2 + 1];
    cpu.pc = (uint16_t)(((hi & 0x0F) << 8) | lo);
    cpu.psw = restore_psw ? (uint8_t)((hi & 0xF0) | 0x08 | sp) : (uint8_t)((cpu.psw & 0xF8) | sp);
}
static inline uint8_t fetch(void) { return sh101_rom[cpu.pc++ & 0x7FF]; }
#define JREL(c)  do { uint8_t a = fetch(); if (c) cpu.pc = (uint16_t)((cpu.pc & 0x700) | a); pace(2); } while (0)

// ---- full MCS-48 instruction decode (cycle counts per the manual) -----------
static void step(void) {
    uint8_t op = fetch(), *R = regs();
    switch (op) {
    case 0x00: pace(1); break;                                                            // NOP
    case 0x23: cpu.a = fetch(); pace(2); break;                                           // MOV A,#data
    case 0x27: cpu.a = 0; pace(1); break;                                                 // CLR A
    case 0x37: cpu.a = (uint8_t)~cpu.a; pace(1); break;                                    // CPL A
    case 0x17: cpu.a++; pace(1); break;                                                    // INC A
    case 0x07: cpu.a--; pace(1); break;                                                    // DEC A
    case 0x03: do_add(fetch(), 0); pace(2); break;                                         // ADD A,#data
    case 0x13: do_add(fetch(), CY ? 1 : 0); pace(2); break;                                // ADDC A,#data
    case 0x60: case 0x61: do_add(*atR(op & 1), 0); pace(1); break;                         // ADD A,@Rr
    case 0x70: case 0x71: do_add(*atR(op & 1), CY ? 1 : 0); pace(1); break;                // ADDC A,@Rr
    case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e: case 0x6f: do_add(R[op & 7], 0); pace(1); break;        // ADD A,Rr
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: do_add(R[op & 7], CY ? 1 : 0); pace(1); break; // ADDC A,Rr
    case 0x53: cpu.a &= fetch(); pace(2); break;                                           // ANL A,#data
    case 0x43: cpu.a |= fetch(); pace(2); break;                                           // ORL A,#data
    case 0xd3: cpu.a ^= fetch(); pace(2); break;                                           // XRL A,#data
    case 0x50: case 0x51: cpu.a &= *atR(op & 1); pace(1); break;                           // ANL A,@Rr
    case 0x40: case 0x41: cpu.a |= *atR(op & 1); pace(1); break;                           // ORL A,@Rr
    case 0xd0: case 0xd1: cpu.a ^= *atR(op & 1); pace(1); break;                           // XRL A,@Rr
    case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: cpu.a &= R[op & 7]; pace(1); break;          // ANL A,Rr
    case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: cpu.a |= R[op & 7]; pace(1); break;          // ORL A,Rr
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: cpu.a ^= R[op & 7]; pace(1); break;          // XRL A,Rr
    case 0x47: cpu.a = (uint8_t)((cpu.a << 4) | (cpu.a >> 4)); pace(1); break;             // SWAP A
    case 0xe7: cpu.a = (uint8_t)((cpu.a << 1) | (cpu.a >> 7)); pace(1); break;             // RL A
    case 0x77: cpu.a = (uint8_t)((cpu.a >> 1) | (cpu.a << 7)); pace(1); break;             // RR A
    case 0xf7: { int c = cpu.a >> 7; cpu.a = (uint8_t)((cpu.a << 1) | (CY ? 1 : 0)); set_cy(c); pace(1); } break;   // RLC A
    case 0x67: { int c = cpu.a & 1; cpu.a = (uint8_t)((cpu.a >> 1) | (CY ? 0x80 : 0)); set_cy(c); pace(1); } break; // RRC A
    case 0x57: { uint8_t a = cpu.a; int c = CY ? 1 : 0;                                    // DA A
                 if ((a & 0xF) > 9 || (cpu.psw & 0x40)) { if ((unsigned)a + 6 > 0xFF) c = 1; a = (uint8_t)(a + 6); }
                 if ((a >> 4) > 9 || c) { a = (uint8_t)(a + 0x60); c = 1; }
                 cpu.a = a; set_cy(c); pace(1); } break;
    case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: R[op & 7]++; pace(1); break;  // INC Rr
    case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf: R[op & 7]--; pace(1); break;  // DEC Rr
    case 0x10: case 0x11: (*atR(op & 1))++; pace(1); break;                                // INC @Rr
    case 0xf8: case 0xf9: case 0xfa: case 0xfb: case 0xfc: case 0xfd: case 0xfe: case 0xff: cpu.a = R[op & 7]; pace(1); break;  // MOV A,Rr
    case 0xa8: case 0xa9: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: R[op & 7] = cpu.a; pace(1); break;  // MOV Rr,A
    case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: R[op & 7] = fetch(); pace(2); break; // MOV Rr,#data
    case 0xf0: case 0xf1: cpu.a = *atR(op & 1); pace(1); break;                            // MOV A,@Rr
    case 0xa0: case 0xa1: *atR(op & 1) = cpu.a; pace(1); break;                            // MOV @Rr,A
    case 0xb0: case 0xb1: *atR(op & 1) = fetch(); pace(2); break;                          // MOV @Rr,#data
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: { uint8_t t = cpu.a; cpu.a = R[op & 7]; R[op & 7] = t; pace(1); } break; // XCH A,Rr
    case 0x20: case 0x21: { uint8_t *m = atR(op & 1), t = cpu.a; cpu.a = *m; *m = t; pace(1); } break;  // XCH A,@Rr
    case 0x30: case 0x31: { uint8_t *m = atR(op & 1), la = cpu.a & 0xF; cpu.a = (uint8_t)((cpu.a & 0xF0) | (*m & 0xF)); *m = (uint8_t)((*m & 0xF0) | la); pace(1); } break; // XCHD A,@Rr
    case 0x02: bus_out(cpu.a); pace(2); break;                                             // OUTL BUS,A
    case 0x08: cpu.a = cpu.bus; pace(2); break;                                            // INS A,BUS (bus output-only here)
    case 0x09: cpu.a = p1_in(); pace(2); break;                                            // IN A,P1
    case 0x0a: cpu.a = cpu.p2; pace(2); break;                                             // IN A,P2
    case 0x39: cpu.p1_latch = cpu.a; pace(2); break;                                       // OUTL P1,A
    case 0x3a: p2_out(cpu.a); pace(2); break;                                              // OUTL P2,A
    case 0x88: bus_out((uint8_t)(cpu.bus | fetch())); pace(2); break;                      // ORL BUS,#data
    case 0x98: bus_out((uint8_t)(cpu.bus & fetch())); pace(2); break;                      // ANL BUS,#data
    case 0x89: cpu.p1_latch |= fetch(); pace(2); break;                                    // ORL P1,#data
    case 0x99: cpu.p1_latch &= fetch(); pace(2); break;                                    // ANL P1,#data
    case 0x8a: p2_out((uint8_t)(cpu.p2 | fetch())); pace(2); break;                        // ORL P2,#data
    case 0x9a: p2_out((uint8_t)(cpu.p2 & fetch())); pace(2); break;                        // ANL P2,#data
    case 0x42: cpu.a = cpu.t; pace(1); break;                                              // MOV A,T
    case 0x62: cpu.t = cpu.a; pace(1); break;                                              // MOV T,A
    case 0x55: cpu.timer_run = true;  cpu.counter_run = false; pace(1); break;             // STRT T
    case 0x45: cpu.counter_run = true; cpu.timer_run = false; cpu.last_t1 = t1_in(); pace(1); break; // STRT CNT
    case 0x65: cpu.timer_run = cpu.counter_run = false; pace(1); break;                    // STOP TCNT
    case 0x25: cpu.tcnti_en = true; pace(1); break;                                        // EN TCNTI
    case 0x35: cpu.tcnti_en = false; pace(1); break;                                       // DIS TCNTI
    case 0x97: set_cy(0); pace(1); break;                                                  // CLR C
    case 0xa7: set_cy(CY ? 0 : 1); pace(1); break;                                         // CPL C
    case 0x85: cpu.psw &= ~0x20; pace(1); break;                                           // CLR F0
    case 0x95: cpu.psw ^= 0x20; pace(1); break;                                            // CPL F0
    case 0xa5: cpu.f1 = false; pace(1); break;                                             // CLR F1
    case 0xb5: cpu.f1 = !cpu.f1; pace(1); break;                                           // CPL F1
    case 0xc7: cpu.a = cpu.psw; pace(1); break;                                            // MOV A,PSW
    case 0xd7: cpu.psw = cpu.a | 0x08; pace(1); break;                                     // MOV PSW,A
    case 0xc5: cpu.psw &= ~0x10; pace(1); break;                                           // SEL RB0
    case 0xd5: cpu.psw |= 0x10; pace(1); break;                                            // SEL RB1
    case 0xe5: cpu.dbf = false; pace(1); break;                                            // SEL MB0
    case 0xf5: cpu.dbf = true; pace(1); break;                                             // SEL MB1
    case 0x05: case 0x15: case 0x75: pace(1); break;                                       // EN I / DIS I / ENT0 CLK
    case 0xa3: cpu.a = sh101_rom[(cpu.pc & 0x700) | cpu.a]; pace(2); break;                // MOVP A,@A
    case 0xe3: cpu.a = sh101_rom[0x300 | cpu.a]; pace(2); break;                           // MOVP3 A,@A
    case 0xb3: cpu.pc = (uint16_t)((cpu.pc & 0x700) | sh101_rom[(cpu.pc & 0x700) | cpu.a]); pace(2); break; // JMPP @A
    case 0x04: case 0x24: case 0x44: case 0x64: case 0x84: case 0xa4: case 0xc4: case 0xe4:
        { uint8_t a = fetch(); cpu.pc = (uint16_t)(((op & 0xe0) << 3) | a); pace(2); } break;            // JMP addr
    case 0x14: case 0x34: case 0x54: case 0x74: case 0x94: case 0xb4: case 0xd4: case 0xf4:
        { uint8_t a = fetch(); push(); cpu.pc = (uint16_t)(((op & 0xe0) << 3) | a); pace(2); } break;    // CALL addr
    case 0x83: pop_pc(0); pace(2); break;                                                  // RET
    case 0x93: pop_pc(1); cpu.in_irq = false; pace(2); break;                              // RETR
    case 0xc6: JREL(cpu.a == 0); break;                                                    // JZ
    case 0x96: JREL(cpu.a != 0); break;                                                    // JNZ
    case 0xf6: JREL(CY); break;                                                            // JC
    case 0xe6: JREL(!CY); break;                                                           // JNC
    case 0xb6: JREL(cpu.psw & 0x20); break;                                                // JF0
    case 0x76: JREL(cpu.f1); break;                                                        // JF1
    case 0x36: JREL(t0_in()); break;                                                       // JT0
    case 0x26: JREL(!t0_in()); break;                                                      // JNT0
    case 0x56: JREL(t1_in()); break;                                                       // JT1
    case 0x46: JREL(!t1_in()); break;                                                      // JNT1
    case 0x86: JREL(0); break;                                                             // JNI (no ext int)
    case 0x16: { uint8_t a = fetch(); if (cpu.tf) { cpu.tf = false; cpu.pc = (uint16_t)((cpu.pc & 0x700) | a); } pace(2); } break; // JTF
    case 0x12: JREL(cpu.a & 0x01); break;  case 0x32: JREL(cpu.a & 0x02); break;           // JB0 / JB1
    case 0x52: JREL(cpu.a & 0x04); break;  case 0x72: JREL(cpu.a & 0x08); break;           // JB2 / JB3
    case 0x92: JREL(cpu.a & 0x10); break;  case 0xb2: JREL(cpu.a & 0x20); break;           // JB4 / JB5
    case 0xd2: JREL(cpu.a & 0x40); break;  case 0xf2: JREL(cpu.a & 0x80); break;           // JB6 / JB7
    case 0xe8: case 0xe9: case 0xea: case 0xeb: case 0xec: case 0xed: case 0xee: case 0xef:
        { uint8_t a = fetch(); if (--R[op & 7] != 0) cpu.pc = (uint16_t)((cpu.pc & 0x700) | a); pace(2); } break;  // DJNZ Rr,addr
    default: pace(1); break;   // illegal / MOVX / MOVD-expander (unused in the SH-101)
    }
}

#endif // MCS48_CORE_H
