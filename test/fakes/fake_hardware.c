#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "fake_hardware.h"
#include "sequencer.h"

static fake_timer_t timer;
fake_timer_t *timer_hw = &timer;
uint8_t fake_midi[FAKE_MIDI_CAPACITY] = {0};
unsigned fake_midi_length;
bool fake_uart_writable = true;
bool fake_uart_tx_irq;
uint32_t fake_gates;
unsigned fake_gate_rises;
void (*fake_before_gate_commit)(void);
void (*fake_before_lock)(spin_lock_t *lock);
void (*fake_before_lockout)(void);
void (*fake_irq_handlers[2])(void);
static spin_lock_t locks[32];
static unsigned next_lock;
_Alignas(256) uint8_t fake_flash[2 * 1024 * 1024] = {0};
unsigned fake_flash_erases;
unsigned fake_flash_programs;
uint32_t fake_flash_last_program_offset;
unsigned fake_flash_program_limit = UINT32_MAX;
static bool uart_drained;

void fake_hardware_reset(void) {
    memset(&timer, 0, sizeof(timer));
    for (int i = 0; i < 32; i++) fake_gpio_level[i] = true;
    fake_midi_length = 0;
    fake_uart_writable = true;
    fake_uart_tx_irq = false;
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
void gpio_pull_up(uint8_t pin) { (void)pin; }
void gpio_disable_pulls(uint8_t pin) { (void)pin; }
uint64_t get_absolute_time(void) { return timer.timerawl; }
uint32_t to_ms_since_boot(uint64_t time) { return (uint32_t)(time / 1000); }
void fake_wait_for_interrupt(void) { abort(); }
void multicore_lockout_victim_init(void) {}
void irq_set_exclusive_handler(int irq, void (*handler)(void)) { fake_irq_handlers[irq] = handler; }
void irq_set_enabled(int irq, bool enabled) { (void)irq; (void)enabled; }
void irq_clear(int irq) { (void)irq; }
void uart_init(void *uart, uint32_t baud) { (void)uart; (void)baud; }
void uart_set_format(void *uart, int bits, int stop, int parity) {
    (void)uart; (void)bits; (void)stop; (void)parity;
}
void uart_set_fifo_enabled(void *uart, bool enabled) { (void)uart; (void)enabled; }
void uart_set_irq_enables(void *uart, bool rx, bool tx) {
    (void)uart; (void)rx; fake_uart_tx_irq = tx;
}
bool uart_is_writable(void *uart) { (void)uart; return fake_uart_writable; }
void uart_putc_raw(void *uart, uint8_t value) {
    (void)uart;
    assert(fake_uart_writable);
    assert(fake_midi_length < FAKE_MIDI_CAPACITY);
    fake_midi[fake_midi_length++] = value;
    uart_drained = false;
}
void uart_tx_wait_blocking(void *uart) { (void)uart; uart_drained = true; }
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
