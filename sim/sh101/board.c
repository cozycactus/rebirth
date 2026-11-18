// Virtual I/O around U6. See sh-101 Rev-A (68f0691), and Roland Service Notes pp. 3, 7.
#include <string.h>
#include "board.h"
#include "mcs48_core.h"

enum { MACHINE_HZ = 400000, EVENT_CAPACITY = 512, CLOCK_RESET = 0x10 };

cpu_t cpu;
uint8_t sh101_rom[2048];

static uint32_t keys;
static uint8_t buttons, switches, dac[3];
static bool pedal, external_clock, ext_clock_high;
static double phase, phase_step;

// Two words per change: machine cycles since sh101_run began, output state.
// A full buffer stops the interpreter at an instruction boundary.
static uint32_t events[2 * EVENT_CAPACITY];
static uint32_t count;
static uint64_t start_cycle;

uint8_t *sh101_rom_data(void) { return sh101_rom; }
double sh101_cycles(void) { return (double)cpu.mcycles; }
double sh101_phase(void) { return phase; }
const uint32_t *sh101_events(void) { return events; }

uint32_t sh101_state(void) {
    return dac[0] | (uint32_t)dac[1] << 6 | (uint32_t)dac[2] << 12 | (uint32_t)cpu.p2 << 18;
}

static void record(void) {
    events[2 * count] = (uint32_t)(cpu.mcycles - start_cycle);
    events[2 * count + 1] = sh101_state();
    count++;
}

void sh101_inputs(uint32_t held, uint8_t pressed, uint8_t panel, int hold_down, int clock_high) {
    keys = held;
    buttons = pressed;
    switches = panel;
    pedal = hold_down != 0;
    ext_clock_high = clock_high != 0;
}

// EXT CLK IN switches the T1 source. The analog LFO still runs.
void sh101_clock(double hz, int external) {
    phase_step = hz / MACHINE_HZ;
    external_clock = external != 0;
}

void sh101_reset(void) {
    memset(&cpu, 0, sizeof(cpu));
    memset(dac, 0, sizeof(dac));
    cpu.psw = 0x08;
    cpu.p1_latch = cpu.p2 = cpu.bus = 0xff;
    phase = 0;
    count = 0;
}

void sh101_power_on(void) {
    // Battery backup retains the internal sequence RAM across POWER cycles.
    uint8_t ram[sizeof(cpu.ram)];
    memcpy(ram, cpu.ram, sizeof(ram));
    sh101_reset();
    memcpy(cpu.ram, ram, sizeof(ram));
}

void bus_out(uint8_t value) {
    cpu.bus = value;
    unsigned channel = value >> 6;
    uint8_t code = ~value & 63;
    if (channel < 3 && dac[channel] != code) {
        dac[channel] = code;
        record();
    }
}

uint8_t p1_in(void) {
    uint8_t matrix = 0xff;
    switch (cpu.bus) {
    case 0xfe: matrix = (uint8_t)~keys; break;
    case 0xfd: matrix = (uint8_t)~(keys >> 8); break;
    case 0xfb: matrix = (uint8_t)~(keys >> 16); break;
    case 0xf7: matrix = (uint8_t)~(keys >> 24); break;
    case 0xef: matrix = switches; break;
    case 0xdf: matrix = (uint8_t)~buttons; break;
    }
    return cpu.p1_latch & matrix;
}

void p2_out(uint8_t value) {
    if (cpu.p2 == value) return;
    // A low P24 resets the LFO through TR7 and TR6.
    if (!(value & CLOCK_RESET)) phase = 0;
    cpu.p2 = value;
    record();
}

bool t0_in(void) { return !pedal; }
bool t1_in(void) { return external_clock ? !ext_clock_high : phase >= 0.5; }

void pace(uint8_t mc) {
    cpu.mcycles += mc;
    if (cpu.p2 & CLOCK_RESET) {
        phase += mc * phase_step;
        if (phase >= 1) phase -= 1;
    }
    if (cpu.timer_run) {
        cpu.tprescale += mc;
        while (cpu.tprescale >= 32) {
            cpu.tprescale -= 32;
            if (++cpu.t == 0) cpu.tf = true;
        }
    }
}

uint32_t sh101_run(uint32_t cycles) {
    count = 0;
    start_cycle = cpu.mcycles;
    while (cpu.mcycles - start_cycle < cycles && count < EVENT_CAPACITY) {
        step();
        if (cpu.counter_run) {
            bool t1 = t1_in();
            if (cpu.last_t1 && !t1 && ++cpu.t == 0) cpu.tf = true;
            cpu.last_t1 = t1;
        }
        if (cpu.tf && cpu.tcnti_en && !cpu.in_irq) {
            cpu.in_irq = true;
            push();
            cpu.pc = 7;
            cpu.tf = false;
            pace(2);
        }
    }
    return count;
}
