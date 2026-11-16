#include "rgb_leds.h"

#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "ws2812.pio.h"

#define LED_FREQ_HZ      800000.0f
#define FRAME_TIME_US    3000u   // 82 x 30 us on the wire plus the reset gap

// Logical index -> chain position. Identity until the board exists.
static const uint8_t chain_order[LED_COUNT] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 81,
};

static PIO led_pio;
static uint led_sm;
static int led_dma = -1;
static uint32_t frame_words[LED_COUNT];
static absolute_time_t frame_accepted;
static bool frame_sent;

static bool previous_finished(void) {
    if (!frame_sent) return true;
    if (dma_channel_is_busy(led_dma)) return false;
    if (!pio_sm_is_tx_fifo_empty(led_pio, led_sm)) return false;
    return absolute_time_diff_us(frame_accepted, get_absolute_time()) >= (int64_t)FRAME_TIME_US;
}

static bool start_frame(void) {
    dma_channel_set_read_addr(led_dma, frame_words, false);
    dma_channel_set_trans_count(led_dma, LED_COUNT, true);
    frame_accepted = get_absolute_time();
    frame_sent = true;
    return true;
}

void rgb_leds_init(void) {
    uint offset;
    PIO pio = pio0;
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        pio = pio1;
        sm = pio_claim_unused_sm(pio, true);
    }
    led_pio = pio;
    led_sm = (uint)sm;
    offset = pio_add_program(led_pio, &ws2812_program);
    ws2812_program_init(led_pio, led_sm, offset, RGB_LEDS_PIN, LED_FREQ_HZ, false);

    led_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config((uint)led_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(led_pio, led_sm, true));
    dma_channel_configure((uint)led_dma, &c, &led_pio->txf[led_sm], frame_words, LED_COUNT, false);

    frame_sent = false;
    for (int i = 0; i < LED_COUNT; i++) frame_words[i] = 0;
    start_frame();
}

bool rgb_leds_show(const rgb_t frame[LED_COUNT]) {
    if (led_dma < 0 || !previous_finished()) return false;
    for (int i = 0; i < LED_COUNT; i++) {
        const rgb_t *p = &frame[i];
        // The PIO program shifts left from bit 23 after autopull, so the
        // 24-bit GRB word sits in the top of the 32-bit FIFO word.
        frame_words[chain_order[i]] =
            ((uint32_t)p->g << 24) | ((uint32_t)p->r << 16) | ((uint32_t)p->b << 8);
    }
    return start_frame();
}
