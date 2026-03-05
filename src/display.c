#include "sequencer.h"

// TODO: implement OLED driver (SSD1306 over I2C/SPI)

void display_init(void) {}
void display_clear(void) {}
void display_update(void) {}
void display_draw_pattern_view(const pattern_t *p, uint8_t track, uint8_t step) {
    (void)p; (void)track; (void)step;
}
void display_draw_song_view(const song_t *s, uint16_t pos) {
    (void)s; (void)pos;
}
void display_draw_bpm(float bpm) { (void)bpm; }
void display_draw_text(uint8_t x, uint8_t y, const char *text) {
    (void)x; (void)y; (void)text;
}
