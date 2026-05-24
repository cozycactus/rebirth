#include "sequencer.h"

#if !defined(REBIRTH_HOST_TEST) && !defined(MOCK_HARDWARE)
#include "hardware/sync.h"
#endif

void seq_lock_init(void) {
#if defined(REBIRTH_HOST_TEST) || defined(MOCK_HARDWARE)
    g_seq.spinlock_num = 0;
    g_seq.spinlock_ready = true;
#else
    g_seq.spinlock_num = spin_lock_claim_unused(true);
    g_seq.spinlock_ready = true;
#endif
}

uint32_t seq_lock(void) {
#if defined(REBIRTH_HOST_TEST) || defined(MOCK_HARDWARE)
    return 0;
#else
    if (!g_seq.spinlock_ready) return 0;
    return spin_lock_blocking(spin_lock_instance(g_seq.spinlock_num));
#endif
}

void seq_unlock(uint32_t irq_state) {
#if defined(REBIRTH_HOST_TEST) || defined(MOCK_HARDWARE)
    (void)irq_state;
#else
    if (!g_seq.spinlock_ready) return;
    spin_unlock(spin_lock_instance(g_seq.spinlock_num), irq_state);
#endif
}
