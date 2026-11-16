#ifndef REBIRTH_FAKE_SYNC_H
#define REBIRTH_FAKE_SYNC_H
#include <stdint.h>
#include <stdbool.h>
#ifdef REBIRTH_THREADED_TEST
#include <pthread.h>
typedef struct { pthread_mutex_t mutex; } spin_lock_t;
#else
typedef struct { bool held; } spin_lock_t;
#endif
uint32_t spin_lock_claim_unused(bool required);
spin_lock_t *spin_lock_instance(uint32_t index);
uint32_t spin_lock_blocking(spin_lock_t *lock);
void spin_unlock(spin_lock_t *lock, uint32_t state);
void hw_set_bits(uint32_t *address, uint32_t bits);
void hw_clear_bits(uint32_t *address, uint32_t bits);
uint32_t save_and_disable_interrupts(void);
void restore_interrupts(uint32_t state);
#endif
