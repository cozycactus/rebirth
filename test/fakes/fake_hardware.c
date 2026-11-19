#undef NDEBUG
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "fake_hardware.h"
#include "sequencer.h"

static fake_timer_t timer;
fake_timer_t *timer_hw = &timer;
uint8_t fake_midi[FAKE_MIDI_CAPACITY] = {0};
unsigned fake_midi_length;
bool fake_uart_writable = true;
unsigned fake_uart_budget = UINT_MAX;
bool fake_uart_fifo_enabled = true;
bool fake_uart_tx_irq;
bool fake_uart_rx_irq;
unsigned fake_uart_wait_midi_length;
#define FAKE_UART_RX_CAPACITY 4096
static uint8_t uart_rx[FAKE_UART_RX_CAPACITY];
static unsigned uart_rx_head, uart_rx_tail;   // free-running
uint32_t fake_midi_time_us[FAKE_MIDI_CAPACITY];
uint32_t fake_gates;
unsigned fake_gate_rises;
void (*fake_before_gate_commit)(void);
void (*fake_before_lock)(spin_lock_t *lock);
void (*fake_before_lockout)(void);
void (*fake_irq_handlers[FAKE_IRQ_COUNT])(void);
static spin_lock_t locks[32];
static unsigned next_lock;
_Alignas(256) uint8_t fake_flash[2 * 1024 * 1024] = {0};
unsigned fake_flash_erases;
unsigned fake_flash_programs;
uint32_t fake_flash_last_program_offset;
unsigned fake_flash_program_limit = UINT32_MAX;
static bool uart_drained;
#define FAKE_UART_BYTE_US 320u      // 10 bits at 31,250 baud
static bool wire_time;              // set by fake_uart_run_until()
static uint32_t wire_free_at;       // when the last byte written has left the line
// The PL011 with its FIFO off: a shift register and a one-byte holding register.
// The holding register empties when its byte starts on the line, so the UART is
// writable one byte time before the line is free: two bytes may be committed.
static uint32_t uart_writable_at(void) { return wire_free_at - FAKE_UART_BYTE_US; }

void fake_hardware_reset(void) {
    memset(&timer, 0, sizeof(timer));
    for (int i = 0; i < 32; i++) fake_gpio_level[i] = true;
    memset(fake_gpio_pulled_up, 0, sizeof(fake_gpio_pulled_up));
    memset(fake_gpio_pulls_disabled, 0, sizeof(fake_gpio_pulls_disabled));
    fake_midi_length = 0;
    fake_uart_writable = true;
    fake_uart_budget = UINT_MAX;
    fake_uart_fifo_enabled = true;
    fake_uart_tx_irq = false;
    fake_uart_rx_irq = false;
    fake_uart_wait_midi_length = 0;
    uart_rx_head = uart_rx_tail = 0;
    wire_time = false;
    wire_free_at = 0;
    fake_gates = 0;
    fake_gate_rises = 0;
    fake_before_gate_commit = NULL;
    fake_before_lock = NULL;
    fake_before_lockout = NULL;
    uart_drained = false;
    fake_flash_erases = 0;
    fake_flash_programs = 0;
    fake_flash_program_limit = UINT32_MAX;
}

uint32_t spin_lock_claim_unused(bool required) {
    (void)required;
    assert(next_lock < 32);
#ifdef REBIRTH_THREADED_TEST
    pthread_mutexattr_t attributes;
    assert(pthread_mutexattr_init(&attributes) == 0);
    assert(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK) == 0);
    assert(pthread_mutex_init(&locks[next_lock].mutex, &attributes) == 0);
    assert(pthread_mutexattr_destroy(&attributes) == 0);
#endif
    return next_lock++;
}
spin_lock_t *spin_lock_instance(uint32_t index) { return &locks[index]; }
uint32_t spin_lock_blocking(spin_lock_t *lock) {
    if (fake_before_lock) fake_before_lock(lock);
#ifdef REBIRTH_THREADED_TEST
    assert(pthread_mutex_lock(&lock->mutex) == 0);
#else
    assert(!lock->held);
    lock->held = true;
#endif
    return 0;
}
void spin_unlock(spin_lock_t *lock, uint32_t state) {
    (void)state;
#ifdef REBIRTH_THREADED_TEST
    assert(pthread_mutex_unlock(&lock->mutex) == 0);
#else
    assert(lock->held);
    lock->held = false;
#endif
}
void hw_set_bits(uint32_t *address, uint32_t bits) { *address |= bits; }
void hw_clear_bits(uint32_t *address, uint32_t bits) { *address &= ~bits; }
void gpio_init(uint8_t pin) { (void)pin; }
void gpio_set_dir(uint8_t pin, bool output) { (void)pin; (void)output; }
void gpio_set_function(uint8_t pin, int function) { (void)pin; (void)function; }
void spi_init(void *spi, uint32_t rate) { (void)spi; (void)rate; }
void spi_write_blocking(void *spi, const uint8_t *data, int length) {
    (void)spi; (void)data; (void)length;
}
void sleep_ms(uint32_t milliseconds) { timer.timerawl += milliseconds * 1000; }
void gpio_put(uint8_t pin, bool value) { (void)pin; (void)value; }
bool fake_gpio_level[32] = {
    true, true, true, true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true, true, true, true, true,
    true, true, true, true, true, true, true, true, true, true
};
bool gpio_get(uint8_t pin) { return fake_gpio_level[pin & 31]; }
bool fake_gpio_pulled_up[32];
bool fake_gpio_pulls_disabled[32];
void gpio_pull_up(uint8_t pin) {
    fake_gpio_pulled_up[pin & 31] = true;
    fake_gpio_pulls_disabled[pin & 31] = false;
}
void gpio_disable_pulls(uint8_t pin) {
    fake_gpio_pulled_up[pin & 31] = false;
    fake_gpio_pulls_disabled[pin & 31] = true;
}
uint64_t get_absolute_time(void) { return timer.timerawl; }
uint32_t to_ms_since_boot(uint64_t time) { return (uint32_t)(time / 1000); }
void fake_wait_for_interrupt(void) { abort(); }
void multicore_lockout_victim_init(void) {}
void irq_set_exclusive_handler(int irq, void (*handler)(void)) { fake_irq_handlers[irq] = handler; }
void irq_set_enabled(int irq, bool enabled) { (void)irq; (void)enabled; }
void irq_clear(int irq) { (void)irq; }
// As in the SDK, uart_init() leaves the FIFOs enabled.
void uart_init(void *uart, uint32_t baud) { (void)uart; (void)baud; fake_uart_fifo_enabled = true; }
void uart_set_format(void *uart, int bits, int stop, int parity) {
    (void)uart; (void)bits; (void)stop; (void)parity;
}
void uart_set_fifo_enabled(void *uart, bool enabled) { (void)uart; fake_uart_fifo_enabled = enabled; }
void uart_set_irq_enables(void *uart, bool rx, bool tx) {
    (void)uart; fake_uart_rx_irq = rx; fake_uart_tx_irq = tx;
}
void fake_uart_rx_push(const uint8_t *bytes, unsigned count) {
    assert(uart_rx_head - uart_rx_tail + count <= FAKE_UART_RX_CAPACITY);
    for (unsigned i = 0; i < count; i++) uart_rx[uart_rx_head++ % FAKE_UART_RX_CAPACITY] = bytes[i];
}
unsigned fake_uart_rx_pending(void) { return uart_rx_head - uart_rx_tail; }
bool uart_is_readable(void *uart) { (void)uart; return uart_rx_head != uart_rx_tail; }
uint8_t uart_getc(void *uart) {
    assert(uart_is_readable(uart));
    return uart_rx[uart_rx_tail++ % FAKE_UART_RX_CAPACITY];
}
bool uart_is_writable(void *uart) {
    (void)uart;
    if (!fake_uart_writable || fake_uart_budget == 0) return false;
    return !wire_time || (int32_t)(timer.timerawl - uart_writable_at()) >= 0;
}
void uart_putc_raw(void *uart, uint8_t value) {
    assert(uart_is_writable(uart));
    assert(fake_midi_length < FAKE_MIDI_CAPACITY);
    // A byte starts on the line when the byte before it has left, or now.
    uint32_t start = timer.timerawl;
    if (wire_time && (int32_t)(wire_free_at - start) > 0) start = wire_free_at;
    fake_midi_time_us[fake_midi_length] = start;
    fake_midi[fake_midi_length++] = value;
    if (fake_uart_budget != UINT_MAX) fake_uart_budget--;
    wire_free_at = start + FAKE_UART_BYTE_US;
    uart_drained = false;
}
void fake_uart_run_until(uint32_t until_us) {
    wire_time = true;
    // The TX interrupt fires when the holding register empties: when the byte in it
    // starts on the line, one byte time before the line is free.
    while (fake_uart_tx_irq && (int32_t)(until_us - uart_writable_at()) > 0) {
        uint32_t writable_at = uart_writable_at();
        if ((int32_t)(writable_at - timer.timerawl) > 0) timer.timerawl = writable_at;
        unsigned written = fake_midi_length;
        assert(fake_irq_handlers[UART0_IRQ]);
        fake_irq_handlers[UART0_IRQ]();
        if (fake_midi_length == written) break; // held back by fake_uart_writable or the budget
    }
    if ((int32_t)(until_us - timer.timerawl) > 0) timer.timerawl = until_us;
}

static uint8_t fake_midi_message_length(uint8_t status) {
    return (status & 0xE0) == 0xC0 ? 2 : 3; // Program Change and Channel Pressure: one data byte
}

unsigned fake_midi_decode(fake_midi_msg_t *out, unsigned max) {
    unsigned count = 0;
    uint8_t running = 0;
    uint8_t data[2];
    unsigned have = 0;
    for (unsigned i = 0; i < fake_midi_length && count < max; i++) {
        uint8_t byte = fake_midi[i];
        if (byte >= 0xF8) {              // real-time: in place, running status kept
            out[count++] = (fake_midi_msg_t){byte, 0, 0};
            continue;
        }
        assert(byte < 0xF0);             // the firmware sends no system common messages
        if (byte & 0x80) {               // a status byte drops any unfinished message
            running = byte;
            have = 0;
            continue;
        }
        assert(running != 0);            // a data byte needs a status before it
        data[have++] = byte;
        if (have + 1u == fake_midi_message_length(running)) {
            out[count++] = (fake_midi_msg_t){running, data[0], have > 1 ? data[1] : 0};
            have = 0;
        }
    }
    return count;
}

unsigned fake_midi_count(uint8_t status) {
    static fake_midi_msg_t messages[FAKE_MIDI_CAPACITY];
    unsigned decoded = fake_midi_decode(messages, FAKE_MIDI_CAPACITY);
    unsigned count = 0;
    for (unsigned i = 0; i < decoded; i++) count += messages[i].status == status;
    return count;
}
void uart_tx_wait_blocking(void *uart) {
    (void)uart;
    uart_drained = true;
    fake_uart_wait_midi_length = fake_midi_length;
}
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t state) { (void)state; }
void multicore_lockout_start_blocking(void) {
    // Core 1 may still run its timer ISR until it is parked here.
    if (fake_before_lockout) fake_before_lockout();
    assert(g_seq.transport.state == TRANSPORT_STOPPED);
}
void multicore_lockout_end_blocking(void) {}
void flash_range_erase(uint32_t offset, size_t count) {
    // UART interrupts are off from here until the write ends.
    assert(uart_drained);
    assert(!fake_uart_tx_irq);
    assert(offset + count <= sizeof(fake_flash));
    assert(offset % 4096 == 0 && count % 4096 == 0);
    memset(fake_flash + offset, 0xFF, count);
    fake_flash_erases++;
}
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    assert(offset + count <= sizeof(fake_flash));
    assert(offset % 256 == 0 && count % 256 == 0);
    if (fake_flash_programs++ >= fake_flash_program_limit) return;
    for (size_t i = 0; i < count; i++) fake_flash[offset + i] &= data[i];
    fake_flash_last_program_offset = offset;
}
void cv_set_note(uint8_t channel, uint8_t note) { (void)channel; (void)note; }
void cv_set_voltage(uint8_t channel, uint16_t value) { (void)channel; (void)value; }
void gate_on(uint8_t channel) { fake_gates |= 1u << channel; fake_gate_rises++; }
void gate_off(uint8_t channel) { fake_gates &= ~(1u << channel); }
void gate_commit(uint32_t on, uint32_t off) {
    if (fake_before_gate_commit) fake_before_gate_commit();
    fake_gates = (fake_gates & ~off) | on;
    if (on) fake_gate_rises++;
}
void cv_gate_all_off(void) { fake_gates = 0; }
