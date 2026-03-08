#ifndef SSD1309_H
#define SSD1309_H

#include <stdint.h>
#include <stdbool.h>

// ─── SPI0 Pin Assignments ──────────────────────────────────────
#define OLED_SCK_PIN    2
#define OLED_MOSI_PIN   3
#define OLED_CS_PIN     4
#define OLED_DC_PIN     5
#define OLED_RST_PIN    6

// ─── Display Dimensions ────────────────────────────────────────
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      (OLED_HEIGHT / 8)               // 8 pages
#define OLED_BUF_SIZE   (OLED_WIDTH * OLED_PAGES)       // 1024 bytes

// ─── Font Metrics ──────────────────────────────────────────────
#define FONT_CHAR_W     6       // 5px glyph + 1px spacing
#define FONT_CHAR_H     8       // 7px glyph + 1px spacing
#define FONT_COLS       (OLED_WIDTH / FONT_CHAR_W)      // 21 chars per line
#define FONT_ROWS       (OLED_HEIGHT / FONT_CHAR_H)     // 8 text lines

// ─── Lifecycle ─────────────────────────────────────────────────

// Initialize SPI0, reset display, send SSD1309 init sequence
void ssd1309_init(void);

// ─── Framebuffer Operations ────────────────────────────────────

// Clear framebuffer (all pixels off)
void ssd1309_clear(void);

// Push entire framebuffer to display via SPI
void ssd1309_flush(void);

// Direct access to the 1024-byte framebuffer
uint8_t *ssd1309_get_framebuffer(void);

// ─── Pixel Operations ──────────────────────────────────────────

void ssd1309_set_pixel(uint8_t x, uint8_t y, bool on);
bool ssd1309_get_pixel(uint8_t x, uint8_t y);

// ─── Drawing Primitives ────────────────────────────────────────

void ssd1309_draw_hline(uint8_t x0, uint8_t x1, uint8_t y, bool on);
void ssd1309_draw_vline(uint8_t x, uint8_t y0, uint8_t y1, bool on);
void ssd1309_draw_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);
void ssd1309_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);

// ─── Text ──────────────────────────────────────────────────────

// Draw single character at pixel position. invert=true for highlight.
void ssd1309_draw_char(uint8_t x, uint8_t y, char c, bool invert);

// Draw null-terminated string. Wraps are NOT handled — caller clips.
void ssd1309_draw_string(uint8_t x, uint8_t y, const char *str, bool invert);

// ─── Display Control ───────────────────────────────────────────

void ssd1309_set_contrast(uint8_t val);

#endif // SSD1309_H
