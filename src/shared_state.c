#include "pico/stdlib.h"
#include "sequencer.h"

#include "hardware/sync.h"

void seq_lock_init(void) {
    g_seq.spinlock_num = spin_lock_claim_unused(true);
    g_seq.spinlock_ready = true;
}

// In RAM: they run inside lock holds the timer waits on (the per-frame
// transport snapshot holds the playback lock across both), so a hold never
// waits on an XIP cache miss.
uint32_t __not_in_flash_func(seq_lock)(void) {
    if (!g_seq.spinlock_ready) return 0;
    return spin_lock_blocking(spin_lock_instance(g_seq.spinlock_num));
}

void __not_in_flash_func(seq_unlock)(uint32_t irq_state) {
    if (!g_seq.spinlock_ready) return;
    spin_unlock(spin_lock_instance(g_seq.spinlock_num), irq_state);
}
