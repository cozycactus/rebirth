#include <stdint.h>
typedef struct {
    uint32_t timerawl;
    uint32_t alarm[4];
    uint32_t intr;
    uint32_t inte;
    uint32_t intf;      // forced interrupts, one bit per alarm
} fake_timer_t;
extern fake_timer_t *timer_hw;
#define TIMER_ALARM_IRQ_NUM(timer, alarm_num) ((void)(timer), TIMER_IRQ_0 + (alarm_num))
