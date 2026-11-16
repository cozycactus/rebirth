#include <stdint.h>
typedef struct {
    uint32_t timerawl;
    uint32_t alarm[4];
    uint32_t intr;
    uint32_t inte;
} fake_timer_t;
extern fake_timer_t *timer_hw;
