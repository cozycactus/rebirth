#ifndef MOCK_HARDWARE
#include "pico/stdlib.h"
#include "hardware/spi.h"
#else
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
// Mock stubs for host-side testing
void gpio_init(uint8_t pin) { (void)pin; }
void gpio_set_dir(uint8_t pin, bool out) { (void)pin; (void)out; }
void gpio_put(uint8_t pin, bool val) { (void)pin; (void)val; }
void spi_init(void *spi, uint32_t rate) { (void)spi; (void)rate; }
void spi_write_blocking(void *spi, const uint8_t *buf, int len) {
    (void)spi; (void)buf; (void)len;
}
void gpio_set_function(uint8_t pin, int func) { (void)pin; (void)func; }
void sleep_ms(uint32_t ms) { (void)ms; }
#define spi0 ((void *)0)
#define GPIO_FUNC_SPI 0
#define GPIO_OUT 1
#endif

#include <string.h>
#include "ssd1309.h"
#include "font5x7.h"

// ─── SPI instance ──────────────────────────────────────────────
#define OLED_SPI        spi0
#define OLED_SPI_FREQ   10000000    // 10 MHz

// ─── Framebuffer ───────────────────────────────────────────────

static uint8_t framebuf[OLED_BUF_SIZE];

uint8_t *ssd1309_get_framebuffer(void) {
    return framebuf;
}

// ─── Low-Level SPI Helpers ─────────────────────────────────────

static inline void oled_cs(bool active) {
    gpio_put(OLED_CS_PIN, !active);     // CS is active-low
}

static inline void oled_dc(bool data) {
    gpio_put(OLED_DC_PIN, data);        // 0 = command, 1 = data
}

static void oled_cmd(uint8_t cmd) {
    oled_dc(false);
    oled_cs(true);
    spi_write_blocking(OLED_SPI, &cmd, 1);
    oled_cs(false);
}

static void oled_cmd2(uint8_t cmd, uint8_t arg) {
    oled_dc(false);
    oled_cs(true);
    uint8_t buf[2] = {cmd, arg};
    spi_write_blocking(OLED_SPI, buf, 2);
    oled_cs(false);
}

// ─── Initialization ────────────────────────────────────────────

void ssd1309_init(void) {
    // Configure SPI0
    spi_init(OLED_SPI, OLED_SPI_FREQ);
    gpio_set_function(OLED_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(OLED_MOSI_PIN, GPIO_FUNC_SPI);

    // CS, DC, RST as GPIO outputs
    gpio_init(OLED_CS_PIN);
    gpio_set_dir(OLED_CS_PIN, GPIO_OUT);
    gpio_put(OLED_CS_PIN, 1);      // CS inactive

    gpio_init(OLED_DC_PIN);
    gpio_set_dir(OLED_DC_PIN, GPIO_OUT);
    gpio_put(OLED_DC_PIN, 0);

    gpio_init(OLED_RST_PIN);
    gpio_set_dir(OLED_RST_PIN, GPIO_OUT);

    // Hardware reset
    gpio_put(OLED_RST_PIN, 1);
    sleep_ms(1);
    gpio_put(OLED_RST_PIN, 0);
    sleep_ms(10);
    gpio_put(OLED_RST_PIN, 1);
    sleep_ms(10);

    // SSD1309 initialization sequence (compatible with SSD1306)
    oled_cmd(0xAE);             // Display OFF

    oled_cmd2(0xD5, 0x80);      // Set clock divide ratio / oscillator freq
    oled_cmd2(0xA8, 0x3F);      // Set multiplex ratio (64 lines)
    oled_cmd2(0xD3, 0x00);      // Set display offset = 0

    oled_cmd(0x40);             // Set start line = 0
    oled_cmd2(0x8D, 0x14);      // Enable charge pump (SSD1306) / internal DC-DC (SSD1309)

    oled_cmd(0xA1);             // Segment remap (column 127 = SEG0)
    oled_cmd(0xC8);             // COM scan direction: remapped (bottom-to-top)

    oled_cmd2(0xDA, 0x12);      // COM pins hardware config: alternative, no remap
    oled_cmd2(0x81, 0xCF);      // Set contrast = 207
    oled_cmd2(0xD9, 0xF1);      // Set pre-charge period
    oled_cmd2(0xDB, 0x40);      // Set VCOMH deselect level

    oled_cmd(0xA4);             // Entire display ON (follow RAM)
    oled_cmd(0xA6);             // Normal display (not inverted)

    oled_cmd(0x20);             // Set memory addressing mode
    oled_cmd(0x00);             // Horizontal addressing mode

    // Clear and turn on
    ssd1309_clear();
    ssd1309_flush();
    oled_cmd(0xAF);             // Display ON
}

// ─── Framebuffer Operations ────────────────────────────────────

void ssd1309_clear(void) {
    memset(framebuf, 0, OLED_BUF_SIZE);
}

void ssd1309_flush(void) {
    // Set column range 0–127
    oled_cmd(0x21);
    oled_cmd(0x00);
    oled_cmd(0x7F);

    // Set page range 0–7
    oled_cmd(0x22);
    oled_cmd(0x00);
    oled_cmd(0x07);

    // Send framebuffer as data
    oled_dc(true);
    oled_cs(true);
    spi_write_blocking(OLED_SPI, framebuf, OLED_BUF_SIZE);
    oled_cs(false);
}

// ─── Pixel Operations ──────────────────────────────────────────

void ssd1309_set_pixel(uint8_t x, uint8_t y, bool on) {
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) return;
    uint16_t idx = (y / 8) * OLED_WIDTH + x;
    uint8_t bit = 1u << (y & 7);
    if (on) {
        framebuf[idx] |= bit;
    } else {
        framebuf[idx] &= ~bit;
    }
}

bool ssd1309_get_pixel(uint8_t x, uint8_t y) {
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT) return false;
    uint16_t idx = (y / 8) * OLED_WIDTH + x;
    uint8_t bit = 1u << (y & 7);
    return (framebuf[idx] & bit) != 0;
}

// ─── Drawing Primitives ────────────────────────────────────────

void ssd1309_draw_hline(uint8_t x0, uint8_t x1, uint8_t y, bool on) {
    if (y >= OLED_HEIGHT) return;
    if (x0 > x1) { uint8_t t = x0; x0 = x1; x1 = t; }
    if (x1 >= OLED_WIDTH) x1 = OLED_WIDTH - 1;
    uint16_t page_base = (y / 8) * OLED_WIDTH;
    uint8_t bit = 1u << (y & 7);
    for (uint8_t x = x0; x <= x1; x++) {
        if (on) {
            framebuf[page_base + x] |= bit;
        } else {
            framebuf[page_base + x] &= ~bit;
        }
    }
}

void ssd1309_draw_vline(uint8_t x, uint8_t y0, uint8_t y1, bool on) {
    if (x >= OLED_WIDTH) return;
    if (y0 > y1) { uint8_t t = y0; y0 = y1; y1 = t; }
    if (y1 >= OLED_HEIGHT) y1 = OLED_HEIGHT - 1;
    for (uint8_t y = y0; y <= y1; y++) {
        uint16_t idx = (y / 8) * OLED_WIDTH + x;
        uint8_t bit = 1u << (y & 7);
        if (on) {
            framebuf[idx] |= bit;
        } else {
            framebuf[idx] &= ~bit;
        }
    }
}

void ssd1309_draw_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on) {
    if (w == 0 || h == 0) return;
    uint8_t x1 = x + w - 1;
    uint8_t y1 = y + h - 1;
    ssd1309_draw_hline(x, x1, y, on);      // Top
    ssd1309_draw_hline(x, x1, y1, on);     // Bottom
    ssd1309_draw_vline(x, y, y1, on);       // Left
    ssd1309_draw_vline(x1, y, y1, on);      // Right
}

void ssd1309_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on) {
    if (w == 0 || h == 0) return;

    uint8_t x1 = x + w - 1;
    uint8_t y1 = y + h - 1;
    if (x1 >= OLED_WIDTH) x1 = OLED_WIDTH - 1;
    if (y1 >= OLED_HEIGHT) y1 = OLED_HEIGHT - 1;

    // Optimised path: process whole pages where possible
    uint8_t page_start = y / 8;
    uint8_t page_end = y1 / 8;

    for (uint8_t page = page_start; page <= page_end; page++) {
        // Which bits in this page byte are covered?
        uint8_t top = (page == page_start) ? (y & 7) : 0;
        uint8_t bot = (page == page_end) ? (y1 & 7) : 7;

        // Build mask for bits top..bot
        uint8_t mask = 0;
        for (uint8_t b = top; b <= bot; b++) {
            mask |= (1u << b);
        }

        uint16_t base = page * OLED_WIDTH;
        for (uint8_t px = x; px <= x1; px++) {
            if (on) {
                framebuf[base + px] |= mask;
            } else {
                framebuf[base + px] &= ~mask;
            }
        }
    }
}

// ─── Text ──────────────────────────────────────────────────────

void ssd1309_draw_char(uint8_t x, uint8_t y, char c, bool invert) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];

    for (uint8_t col = 0; col < 5; col++) {
        uint8_t coldata = glyph[col];
        for (uint8_t row = 0; row < 7; row++) {
            bool pixel = (coldata >> row) & 1;
            if (invert) pixel = !pixel;
            ssd1309_set_pixel(x + col, y + row, pixel);
        }
        // Bottom spacing row
        if (invert) {
            ssd1309_set_pixel(x + col, y + 7, true);
        }
    }
    // Right spacing column (6th column, all clear or filled if inverted)
    for (uint8_t row = 0; row < 8; row++) {
        ssd1309_set_pixel(x + 5, y + row, invert);
    }
}

void ssd1309_draw_string(uint8_t x, uint8_t y, const char *str, bool invert) {
    uint8_t cx = x;
    while (*str) {
        if (cx + FONT_CHAR_W > OLED_WIDTH) break;  // Clip at right edge
        ssd1309_draw_char(cx, y, *str, invert);
        cx += FONT_CHAR_W;
        str++;
    }
}

// ─── Display Control ───────────────────────────────────────────

void ssd1309_set_contrast(uint8_t val) {
    oled_cmd2(0x81, val);
}
