// Virtual RP2040 board for the simulator. It implements the Pico SDK calls the
// firmware makes (declared by test/fakes) on one virtual microsecond clock, with
// register-level models of the I2C chips, the PL011 UART, the timer alarm, the
// two SPI buses and the flash. Nothing here knows what a pad or a pattern is.

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "hardware/uart.h"

#include "sim_internal.h"

// ─── Clock ──────────────────────────────────────────────────────

uint64_t sim_now_us;
static fake_timer_t timer;
fake_timer_t *timer_hw = &timer;

static void apply_inputs_until(uint64_t t_us);

// Time only moves here. The outside world keeps moving with it: every input
// scheduled up to the new time is applied, also when the firmware is stalled
// inside a call (a flash erase, a bus timeout).
void hal_set_now(uint64_t t_us) {
    if (t_us < sim_now_us) return;
    apply_inputs_until(t_us);
    sim_now_us = t_us;
    timer.timerawl = (uint32_t)t_us;
}

uint64_t get_absolute_time(void) { return sim_now_us; }
uint32_t to_ms_since_boot(uint64_t time) { return (uint32_t)(time / 1000u); }
void sleep_ms(uint32_t milliseconds) { hal_set_now(sim_now_us + (uint64_t)milliseconds * 1000u); }
void stdio_init_all(void) {}

// ─── Fatal errors ───────────────────────────────────────────────

static char fatal_text[160];
static bool fatal_set;
static uint32_t lock_errors;

void hal_fatal(const char *message) {
    if (!fatal_set) {
        snprintf(fatal_text, sizeof(fatal_text), "%s (t=%.3f s)", message, (double)sim_now_us / 1e6);
        fatal_set = true;
        fprintf(stderr, "SIM FATAL: %s\n", fatal_text);
    }
}
const char *sim_dbg_fatal(void) { return fatal_set ? fatal_text : NULL; }
uint32_t sim_dbg_lock_errors(void) { return lock_errors; }

// ─── Output events ──────────────────────────────────────────────

#define EVENT_CAPACITY 16384
static sim_event_t events[EVENT_CAPACITY];
static int event_count;
static uint32_t events_dropped;

static void emit(uint64_t t_us, uint32_t kind, uint32_t value) {
    if (event_count == EVENT_CAPACITY) {
        events_dropped++;
        return;
    }
    events[event_count++] = (sim_event_t){ (double)t_us, kind, value };
}
const sim_event_t *sim_events(void) { return events; }
int sim_events_count(void) { return event_count; }
uint32_t sim_events_dropped(void) { return events_dropped; }
void sim_events_clear(void) { event_count = 0; }

// ─── Spin locks: one thread, so a lock taken twice is a bug in the
// simulator's scheduling (an interrupt dispatched inside a critical section). ──

static spin_lock_t locks[32];
static unsigned next_lock;

uint32_t spin_lock_claim_unused(bool required) {
    (void)required;
    if (next_lock >= 32) {
        hal_fatal("out of spin locks");
        return 31;
    }
    return next_lock++;
}
spin_lock_t *spin_lock_instance(uint32_t index) { return &locks[index & 31]; }
uint32_t spin_lock_blocking(spin_lock_t *lock) {
    if (lock->held) {
        lock_errors++;
        hal_fatal("spin lock taken while held");
    }
    lock->held = true;
    return 0;
}
void spin_unlock(spin_lock_t *lock, uint32_t state) {
    (void)state;
    lock->held = false;
}
static bool any_lock_held(void) {
    for (unsigned i = 0; i < 32; i++) {
        if (locks[i].held) return true;
    }
    return false;
}
void hw_set_bits(uint32_t *address, uint32_t bits) { *address |= bits; }
void hw_clear_bits(uint32_t *address, uint32_t bits) { *address &= ~bits; }
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t state) { (void)state; }

// ─── Interrupts and the timer alarm ─────────────────────────────

static void (*irq_handlers[FAKE_IRQ_COUNT])(void);
static bool irq_enabled[FAKE_IRQ_COUNT];

void irq_set_exclusive_handler(int irq, void (*handler)(void)) {
    if (irq < 0 || irq >= FAKE_IRQ_COUNT) { hal_fatal("handler for an unmodelled interrupt"); return; }
    irq_handlers[irq] = handler;
}
void irq_set_enabled(int irq, bool enabled) {
    if (irq < 0 || irq >= FAKE_IRQ_COUNT) { hal_fatal("unmodelled interrupt enabled"); return; }
    irq_enabled[irq] = enabled;
}
void irq_clear(int irq) { (void)irq; }

// Alarm 1 is never armed: its interrupt is the external clock's doorbell, which
// core 0 raises by setting bit 1 of INTF. It is due at once while that bit is
// set and enabled; the handler must clear it.
uint64_t hal_doorbell_due(void) {
    if (!(timer.intf & timer.inte & 2u) || !irq_handlers[TIMER_IRQ_1] || !irq_enabled[TIMER_IRQ_1]) {
        return UINT64_MAX;
    }
    return sim_now_us;
}

void hal_run_doorbell(void) {
    irq_handlers[TIMER_IRQ_1]();
    if (timer.intf & 2u) hal_fatal("the forced timer interrupt was not cleared");
}

// The firmware arms alarm 0 by writing the register; the alarm fires when the
// low 32 bits of the clock reach it. A value that already fired stays quiet
// until it is rewritten. A value written when it is already behind the clock
// does not fire at once: as on the RP2040 it waits for the counter to come
// round again, about 71 minutes. A write is noticed the next time the
// scheduler looks, which is before time moves on (firmware calls take no time
// except the stalls below).
static uint32_t fired_alarm;
static bool fired_valid;
static uint32_t seen_alarm;
static bool seen_valid;
static uint64_t alarm_at;       // absolute time the alarm seen last will match

uint64_t hal_alarm_due(void) {
    if (!(timer.inte & 1u) || !irq_handlers[TIMER_IRQ_0] || !irq_enabled[TIMER_IRQ_0]) return UINT64_MAX;
    uint32_t alarm = timer.alarm[0];
    if (!seen_valid || alarm != seen_alarm) {
        seen_alarm = alarm;
        seen_valid = true;
        fired_valid = false;
        alarm_at = sim_now_us + (uint32_t)(alarm - (uint32_t)sim_now_us);    // 0 .. 2^32 - 1 ahead
    }
    if (fired_valid && alarm == fired_alarm) return UINT64_MAX;
    return alarm_at > sim_now_us ? alarm_at : sim_now_us;
}

void hal_run_alarm(void) {
    fired_alarm = timer.alarm[0];
    fired_valid = true;
    timer.intr |= 1u;
    irq_handlers[TIMER_IRQ_0]();
}

// Core 0 is waiting inside a call (a bus timeout) with no lock held. Core 1's
// timer and core 0's own UART interrupt keep running meanwhile.
static bool waiting;

static bool any_lock_held(void);

static void core0_wait_until(uint64_t until) {
    if (waiting || any_lock_held()) {
        hal_set_now(until);
        return;
    }
    waiting = true;
    for (;;) {
        // An input is applied at its own time, so a MIDI byte raises the UART
        // interrupt then, not when the wait ends.
        uint64_t alarm = hal_alarm_due();
        uint64_t bell = hal_doorbell_due();
        uint64_t uart = hal_uart_irq_due();
        uint64_t input = hal_input_due();
        uint64_t next = alarm;
        if (bell < next) next = bell;
        if (uart < next) next = uart;
        if (input < next) next = input;
        if (next > until) break;
        hal_set_now(next);
        if (next == alarm) hal_run_alarm();
        else if (next == bell) hal_run_doorbell();
        else if (next == uart) hal_run_uart_irq();
    }
    waiting = false;
    hal_set_now(until);
}

// ─── Core 1: its entry registers the timer handler, then waits for
// interrupts forever. The wait returns control here. ─────────────

static jmp_buf core1_parked;
static bool core1_launching;

void multicore_launch_core1(void (*entry)(void)) {
    core1_launching = true;
    if (setjmp(core1_parked) == 0) entry();
    core1_launching = false;
}
void fake_wait_for_interrupt(void) {
    if (core1_launching) longjmp(core1_parked, 1);
    hal_fatal("wait-for-interrupt outside core 1 entry");
}
void multicore_lockout_victim_init(void) {}
void multicore_lockout_start_blocking(void) {}
void multicore_lockout_end_blocking(void) {}

// ─── GPIO ───────────────────────────────────────────────────────

bool fake_gpio_level[32];
bool fake_gpio_pulled_up[32];
bool fake_gpio_pulls_disabled[32];

#define GATE_FIRST_PIN 14
#define GATE_PINS 4

static uint32_t gate_mask(void) {
    uint32_t mask = 0;
    for (int i = 0; i < GATE_PINS; i++) {
        if (fake_gpio_level[GATE_FIRST_PIN + i]) mask |= 1u << i;
    }
    return mask;
}

static void write_pin(uint8_t pin, bool value) {
    pin &= 31;
    if (fake_gpio_level[pin] == value) return;
    uint32_t gates_before = gate_mask();
    fake_gpio_level[pin] = value;
    uint32_t gates_after = gate_mask();
    if (gates_after != gates_before) emit(sim_now_us, SIM_EV_GATE, gates_after);
}

void gpio_init(uint8_t pin) { (void)pin; }
void gpio_set_dir(uint8_t pin, bool output) { (void)pin; (void)output; }
void gpio_set_function(uint8_t pin, int function) { (void)pin; (void)function; }
void gpio_put(uint8_t pin, bool value) { write_pin(pin, value); }
bool gpio_get(uint8_t pin) { return fake_gpio_level[pin & 31]; }
void gpio_pull_up(uint8_t pin) {
    fake_gpio_pulled_up[pin & 31] = true;
    fake_gpio_pulls_disabled[pin & 31] = false;
}
void gpio_disable_pulls(uint8_t pin) {
    fake_gpio_pulled_up[pin & 31] = false;
    fake_gpio_pulls_disabled[pin & 31] = true;
}
// All pins of a mask change in the same instant: one gate event, not one per pin.
static void write_mask(uint32_t mask, bool value) {
    uint32_t gates_before = gate_mask();
    for (int pin = 0; pin < 32; pin++) {
        if (mask & (1u << pin)) fake_gpio_level[pin] = value;
    }
    uint32_t gates_after = gate_mask();
    if (gates_after != gates_before) emit(sim_now_us, SIM_EV_GATE, gates_after);
}
void gpio_set_mask(uint32_t mask) { write_mask(mask, true); }
void gpio_clr_mask(uint32_t mask) { write_mask(mask, false); }

uint32_t sim_gpio_levels(void) {
    uint32_t levels = 0;
    for (int pin = 0; pin < 32; pin++) {
        if (fake_gpio_level[pin]) levels |= 1u << pin;
    }
    return levels;
}

// ─── SPI: OLED on spi0 (data when DC is high), two MCP4822 on spi1 ─

#define OLED_DC 5
#define DAC_CS0 12
#define DAC_CS1 13

static uint8_t oled_frame[1024];
static uint32_t oled_frames;
static bool oled_display_on;
static uint16_t dac_codes[4];

void spi_init(void *spi, uint32_t rate) { (void)spi; (void)rate; }
void spi_write_blocking(void *spi, const uint8_t *data, int length) {
    if (spi == spi0) {
        if (fake_gpio_level[OLED_DC]) {
            if (length == (int)sizeof(oled_frame)) {
                memcpy(oled_frame, data, sizeof(oled_frame));
                oled_frames++;
            }
        } else {
            for (int i = 0; i < length; i++) {
                if (data[i] == 0xAF) oled_display_on = true;
                if (data[i] == 0xAE) oled_display_on = false;
            }
        }
        return;
    }
    if (length != 2) return;
    int dac;
    if (!fake_gpio_level[DAC_CS0]) dac = 0;
    else if (!fake_gpio_level[DAC_CS1]) dac = 1;
    else return;                        // no chip selected: the word goes nowhere
    uint16_t word = (uint16_t)((data[0] << 8) | data[1]);
    if (!(word & (1u << 12))) return;   // shutdown bit clear: output off
    int channel = dac * 2 + ((word >> 15) & 1);
    uint16_t code = word & 0x0FFF;
    if (dac_codes[channel] != code) {
        dac_codes[channel] = code;
        emit(sim_now_us, SIM_EV_DAC, ((uint32_t)channel << 16) | code);
    }
}
const uint8_t *sim_oled(void) { return oled_frame; }
uint32_t sim_oled_seq(void) { return oled_frames; }
int sim_oled_on(void) { return oled_display_on; }
const uint16_t *sim_dac(void) { return dac_codes; }

// ─── UART (PL011, FIFO off): a shift register and a one-byte holding
// register. A byte takes 320 us on the wire at 31,250 baud. The UART is
// writable once the previous byte has moved to the shift register. ─

#define UART_BYTE_US 320u
#define BUSY_WAIT_SPINS 256

static uint64_t wire_free_at;       // when the last byte written leaves the line
static bool uart_tx_irq;
static bool uart_rx_irq;
static bool uart_rx_full;
static uint8_t uart_rx_byte;
static uint32_t uart_rx_overruns;
static uint64_t spin_time;
static unsigned spins;

static uint64_t uart_writable_at(void) {
    return wire_free_at > UART_BYTE_US ? wire_free_at - UART_BYTE_US : 0;
}

void uart_init(void *uart, uint32_t baud) { (void)uart; (void)baud; }
void uart_set_format(void *uart, int bits, int stop, int parity) {
    (void)uart; (void)bits; (void)stop; (void)parity;
}
void uart_set_fifo_enabled(void *uart, bool enabled) { (void)uart; (void)enabled; }
void uart_set_irq_enables(void *uart, bool rx, bool tx) { (void)uart; uart_rx_irq = rx; uart_tx_irq = tx; }

// Receive side, FIFO off: one holding register. A byte that arrives while the
// last one is still unread overwrites nothing and is lost (overrun), as when
// the firmware keeps interrupts off through a flash erase.
static void uart_receive(uint8_t byte) {
    if (uart_rx_full) {
        uart_rx_overruns++;
        return;
    }
    uart_rx_byte = byte;
    uart_rx_full = true;
}
bool uart_is_readable(void *uart) { (void)uart; return uart_rx_full; }
uint8_t uart_getc(void *uart) {
    (void)uart;
    uart_rx_full = false;
    return uart_rx_byte;
}
uint32_t sim_dbg_midi_in_overruns(void) { return uart_rx_overruns; }
// The firmware listens once it has enabled the receive interrupt.
int sim_midi_in_listening(void) { return uart_rx_irq; }

bool uart_is_writable(void *uart) {
    (void)uart;
    if (sim_now_us >= uart_writable_at()) {
        spins = 0;
        return true;
    }
    // Code that polls the UART in a loop (midi_wait_output_idle before a flash
    // save) waits for real time to pass. Here time passes only when the clock
    // is moved: after many polls at one instant, move it to the next byte slot.
    if (spin_time != sim_now_us) {
        spin_time = sim_now_us;
        spins = 0;
    }
    if (++spins > BUSY_WAIT_SPINS) {
        spins = 0;
        hal_set_now(uart_writable_at());
        return true;
    }
    return false;
}
void uart_putc_raw(void *uart, uint8_t value) {
    (void)uart;
    uint64_t start = sim_now_us > wire_free_at ? sim_now_us : wire_free_at;
    wire_free_at = start + UART_BYTE_US;
    emit(wire_free_at, SIM_EV_MIDI, value);
}
void uart_tx_wait_blocking(void *uart) {
    (void)uart;
    if (wire_free_at > sim_now_us) hal_set_now(wire_free_at);
}

uint64_t hal_uart_irq_due(void) {
    if (!irq_handlers[UART0_IRQ] || !irq_enabled[UART0_IRQ]) return UINT64_MAX;
    // A received byte keeps the interrupt asserted until it is read.
    if (uart_rx_irq && uart_rx_full) return sim_now_us;
    if (!uart_tx_irq) return UINT64_MAX;
    uint64_t writable = uart_writable_at();
    return writable > sim_now_us ? writable : sim_now_us;
}
void hal_run_uart_irq(void) { irq_handlers[UART0_IRQ](); }

// ─── Flash ──────────────────────────────────────────────────────

#define FLASH_BYTES (2u * 1024u * 1024u)
#define STORE_BYTES (192u * 1024u)
#define ERASE_SECTOR_US 40000u      // typical 4 KiB sector erase; not measured on this board
#define PROGRAM_PAGE_US 400u        // typical 256 byte page program

_Alignas(256) uint8_t fake_flash[FLASH_BYTES];
unsigned fake_flash_erases;
unsigned fake_flash_programs;
uint32_t fake_flash_last_program_offset;
unsigned fake_flash_program_limit = UINT32_MAX;

void flash_range_erase(uint32_t offset, size_t count) {
    if (offset >= FLASH_BYTES || count > FLASH_BYTES - offset) {
        hal_fatal("flash erase out of range");
        return;
    }
    memset(fake_flash + offset, 0xFF, count);
    fake_flash_erases++;
    // Both cores are stalled for the erase: time passes, nothing runs.
    hal_set_now(sim_now_us + (uint64_t)(count / 4096u) * ERASE_SECTOR_US);
}
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    if (offset >= FLASH_BYTES || count > FLASH_BYTES - offset) {
        hal_fatal("flash program out of range");
        return;
    }
    for (size_t i = 0; i < count; i++) fake_flash[offset + i] &= data[i];   // bits only clear
    fake_flash_programs++;
    fake_flash_last_program_offset = offset;
    hal_set_now(sim_now_us + (uint64_t)((count + 255u) / 256u) * PROGRAM_PAGE_US);
}
uint8_t *sim_flash_store(void) { return fake_flash + 0x000b0000u; }
int sim_flash_store_size(void) { return (int)STORE_BYTES; }

// ─── I2C bus with five chips ────────────────────────────────────

#define ADDR_BUTTONS 0x20       // U701: steps 1-7 on GPA0-6, 9-15 on GPB0-6
#define ADDR_STEP_LEDS 0x21
#define ADDR_KEY_ROWS 0x22      // U801: ROW0-6 on GPA0-6, ROW7 on GPB0, COL0-6 on GPB1-7, COL7 on GPA7
#define ADDR_KEY_COLS 0x23      // U802: COL8 on GPA0, COL9 on GPA1
#define ADDR_STEPS_8_16 0x24    // U703: step 8 on GPA0, step 16 on GPA1

i2c_inst_t fake_i2c0_inst;
static uint8_t i2c_fault[128];

// MCP23017, BANK = 0, sequential addressing.
#define MCP_REGS 0x16
#define MCP_IODIRA 0x00
#define MCP_IPOLA 0x02
#define MCP_GPPUA 0x0C
#define MCP_GPIOA 0x12
#define MCP_OLATA 0x14

typedef struct {
    uint8_t reg[MCP_REGS];
    uint8_t pointer;
    uint16_t pins;          // levels the outside world drives on input pins
} mcp_t;

static mcp_t mcp_buttons;
static mcp_t mcp_leds;
static mcp_t mcp_key_rows;
static mcp_t mcp_key_cols;
static mcp_t mcp_steps_8_16;

static void mcp_reset(mcp_t *chip) {
    memset(chip->reg, 0, sizeof(chip->reg));
    chip->reg[MCP_IODIRA] = 0xFF;
    chip->reg[MCP_IODIRA + 1] = 0xFF;
    chip->pointer = 0;
}
static uint8_t mcp_read(mcp_t *chip, uint8_t reg) {
    if (reg == MCP_GPIOA || reg == MCP_GPIOA + 1) {
        int port = reg - MCP_GPIOA;
        uint8_t dir = chip->reg[MCP_IODIRA + port];
        uint8_t in = (uint8_t)(chip->pins >> (8 * port)) ^ chip->reg[MCP_IPOLA + port];
        return (uint8_t)((in & dir) | (chip->reg[MCP_OLATA + port] & (uint8_t)~dir));
    }
    return chip->reg[reg];
}
static void mcp_write(mcp_t *chip, uint8_t reg, uint8_t value) {
    if (reg == MCP_GPIOA || reg == MCP_GPIOA + 1) reg = (uint8_t)(MCP_OLATA + (reg - MCP_GPIOA));
    chip->reg[reg] = value;
}

// The 8 x 10 key matrix of the PR #10 board, scanned by U801 and U802: a row
// reads low when a closed key sits in a column driven low (an output pin with
// its latch at 0). A row with no such key reads high on its pull-up, or low
// when the pull-up is off (a chip that reset): the worst case, every key in
// the column seen down, which the firmware must not take for presses.
#define KEY_ROWS 8
#define KEY_COLS 10

static bool key_down[KEY_ROWS][KEY_COLS];

static bool pin_driven_low(const mcp_t *chip, int bit) {
    int port = bit / 8;
    uint8_t mask = (uint8_t)(1u << (bit % 8));
    return !(chip->reg[MCP_IODIRA + port] & mask) && !(chip->reg[MCP_OLATA + port] & mask);
}
static bool column_low(int col) {
    if (col < 7) return pin_driven_low(&mcp_key_rows, 9 + col);    // GPB1-7
    if (col == 7) return pin_driven_low(&mcp_key_rows, 7);         // GPA7
    return pin_driven_low(&mcp_key_cols, col - 8);                 // GPA0-1
}
// The levels on U801's row pins (bits 0-6 and 8); its column pins are outputs.
static uint16_t key_row_pins(void) {
    uint16_t pins = 0xFFFF;
    for (int row = 0; row < KEY_ROWS; row++) {
        int bit = row < 7 ? row : 8;
        bool low = !(mcp_key_rows.reg[MCP_GPPUA + bit / 8] & (1u << (bit % 8)));
        for (int col = 0; col < KEY_COLS && !low; col++) low = key_down[row][col] && column_low(col);
        if (low) pins &= (uint16_t)~(1u << bit);
    }
    return pins;
}

void sim_key(int row, int col, int down) {
    if (row < 0 || row >= KEY_ROWS || col < 0 || col >= KEY_COLS) return;
    key_down[row][col] = down != 0;
}
void sim_step_button(int index, int down) {
    if (index < 0 || index > 15) return;
    // Steps 8 and 16 sit on U703; the others on U701, one pin per step.
    mcp_t *chip = (index == 7 || index == 15) ? &mcp_steps_8_16 : &mcp_buttons;
    int bit = index == 7 ? 0 : index == 15 ? 1 : index;
    if (down) chip->pins &= (uint16_t)~(1u << bit);     // the switch pulls the pin low
    else chip->pins |= (uint16_t)(1u << bit);
}
void sim_gpio_in(int pin, int level) { fake_gpio_level[pin & 31] = level != 0; }

// ─── Scheduled inputs: what the hands on the panel do, at their own times ──

#define INPUT_CAPACITY 16384
typedef struct { uint64_t t; uint8_t kind; uint8_t level; int16_t a, b; } input_t;
static input_t inputs[INPUT_CAPACITY];
static int input_head, input_count;

static void apply_input(const input_t *in) {
    if (in->kind == SIM_IN_KEY) sim_key(in->a, in->b, in->level);
    else if (in->kind == SIM_IN_STEP) sim_step_button(in->a, in->level);
    else if (in->kind == SIM_IN_GPIO) sim_gpio_in(in->a, in->level);
    else if (in->kind == SIM_IN_MIDI) uart_receive((uint8_t)in->a);
}
static void apply_inputs_until(uint64_t t_us) {
    while (input_count && inputs[input_head].t <= t_us) {
        const input_t *in = &inputs[input_head];
        // The input happens at its own time, even inside a stall.
        if (in->t > sim_now_us) {
            sim_now_us = in->t;
            timer.timerawl = (uint32_t)in->t;
        }
        input_head = (input_head + 1) % INPUT_CAPACITY;
        input_count--;
        apply_input(in);
    }
}
void sim_input_at(double t_us, int kind, int a, int b, int level) {
    input_t in = { t_us <= 0 ? 0 : (uint64_t)t_us, (uint8_t)kind, level != 0, (int16_t)a, (int16_t)b };
    if (in.t <= sim_now_us) {
        apply_input(&in);
        return;
    }
    if (input_count == INPUT_CAPACITY) {
        hal_fatal("too many scheduled inputs");
        return;
    }
    // Kept sorted by time; equal times keep the order they were given in.
    int i = input_count;
    while (i > 0 && inputs[(input_head + i - 1) % INPUT_CAPACITY].t > in.t) {
        inputs[(input_head + i) % INPUT_CAPACITY] = inputs[(input_head + i - 1) % INPUT_CAPACITY];
        i--;
    }
    inputs[(input_head + i) % INPUT_CAPACITY] = in;
    input_count++;
}
void sim_inputs_clear(void) { input_count = 0; }
int sim_inputs_pending(void) { return input_count; }
uint64_t hal_input_due(void) { return input_count ? inputs[input_head].t : UINT64_MAX; }

uint32_t sim_step_leds(void) {
    uint16_t dir = (uint16_t)(mcp_leds.reg[MCP_IODIRA] | (mcp_leds.reg[MCP_IODIRA + 1] << 8));
    uint16_t latch = (uint16_t)(mcp_leds.reg[MCP_OLATA] | (mcp_leds.reg[MCP_OLATA + 1] << 8));
    return latch & (uint16_t)~dir;      // a pin left as input drives no LED
}
void sim_i2c_fault(int addr, int mode) { i2c_fault[addr & 127] = (uint8_t)mode; }
static mcp_t *mcp_at(uint8_t addr) {
    if (addr == ADDR_BUTTONS) return &mcp_buttons;
    if (addr == ADDR_STEP_LEDS) return &mcp_leds;
    if (addr == ADDR_KEY_ROWS) return &mcp_key_rows;
    if (addr == ADDR_KEY_COLS) return &mcp_key_cols;
    if (addr == ADDR_STEPS_8_16) return &mcp_steps_8_16;
    return NULL;
}
void sim_chip_reset(int addr) {
    mcp_t *chip = mcp_at((uint8_t)(addr & 127));
    if (chip) mcp_reset(chip);
}
static int i2c_fail(uint8_t addr, unsigned timeout_us) {
    if (i2c_fault[addr & 127] == SIM_I2C_TIMEOUT) {
        core0_wait_until(sim_now_us + timeout_us);  // the caller is stuck for the whole timeout
        return PICO_ERROR_TIMEOUT;
    }
    return -1;                                  // PICO_ERROR_GENERIC: no acknowledge
}

unsigned i2c_init(i2c_inst_t *i2c, unsigned baudrate) { (void)i2c; return baudrate; }

int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len,
                         bool nostop, unsigned timeout_us) {
    (void)i2c; (void)nostop;
    if (i2c_fault[addr & 127] || len == 0) return i2c_fail(addr, timeout_us);
    mcp_t *mcp = mcp_at(addr);
    if (mcp) {
        mcp->pointer = src[0] % MCP_REGS;
        for (size_t i = 1; i < len; i++) {
            mcp_write(mcp, mcp->pointer, src[i]);
            mcp->pointer = (uint8_t)((mcp->pointer + 1) % MCP_REGS);
        }
        return (int)len;
    }
    return -1;
}

int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len,
                        bool nostop, unsigned timeout_us) {
    (void)i2c; (void)nostop;
    if (i2c_fault[addr & 127] || len == 0) return i2c_fail(addr, timeout_us);
    mcp_t *mcp = mcp_at(addr);
    if (mcp) {
        if (mcp == &mcp_key_rows) mcp->pins = key_row_pins();
        for (size_t i = 0; i < len; i++) {
            dst[i] = mcp_read(mcp, mcp->pointer);
            mcp->pointer = (uint8_t)((mcp->pointer + 1) % MCP_REGS);
        }
        return (int)len;
    }
    return -1;
}

// ─── Power-on ───────────────────────────────────────────────────

void hal_reset(bool keep_flash) {
    sim_now_us = 0;
    memset(&timer, 0, sizeof(timer));
    fatal_set = false;
    lock_errors = 0;
    event_count = 0;
    events_dropped = 0;
    memset(locks, 0, sizeof(locks));
    next_lock = 0;
    memset(irq_handlers, 0, sizeof(irq_handlers));
    memset(irq_enabled, 0, sizeof(irq_enabled));
    fired_valid = false;
    seen_valid = false;
    waiting = false;
    input_head = 0;
    input_count = 0;
    core1_launching = false;
    for (int pin = 0; pin < 32; pin++) fake_gpio_level[pin] = true;     // inputs idle high (pull-ups)
    for (int i = 0; i < GATE_PINS; i++) fake_gpio_level[GATE_FIRST_PIN + i] = false;
    fake_gpio_level[25] = false;
    memset(fake_gpio_pulled_up, 0, sizeof(fake_gpio_pulled_up));
    memset(fake_gpio_pulls_disabled, 0, sizeof(fake_gpio_pulls_disabled));
    memset(oled_frame, 0, sizeof(oled_frame));
    oled_frames = 0;
    oled_display_on = false;
    memset(dac_codes, 0, sizeof(dac_codes));
    wire_free_at = 0;
    uart_tx_irq = false;
    uart_rx_irq = false;
    uart_rx_full = false;
    uart_rx_overruns = 0;
    spins = 0;
    if (!keep_flash) memset(fake_flash, 0xFF, sizeof(fake_flash));
    fake_flash_erases = 0;
    fake_flash_programs = 0;
    memset(i2c_fault, 0, sizeof(i2c_fault));
    mcp_t *chips[] = { &mcp_buttons, &mcp_leds, &mcp_key_rows, &mcp_key_cols, &mcp_steps_8_16 };
    for (size_t i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        mcp_reset(chips[i]);
        chips[i]->pins = 0xFFFF;
    }
    memset(key_down, 0, sizeof(key_down));
}
