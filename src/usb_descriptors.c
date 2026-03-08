#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>

// ─── USB identifiers ───────────────────────────────────────────
// Using pid.codes shared test VID — replace with a real allocation before
// shipping a product.
#define USB_VID     0x1209u
#define USB_PID     0x2040u     // arbitrary product ID
#define USB_BCD     0x0200u

// ─── Device descriptor ─────────────────────────────────────────

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// ─── Configuration descriptor ──────────────────────────────────
// USB MIDI 1.0: Audio Control interface + MIDI Streaming interface.

enum { ITF_NUM_MIDI = 0, ITF_NUM_MIDI_STREAMING, ITF_NUM_TOTAL };

#define EPNUM_MIDI          0x01u
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static uint8_t const desc_configuration[] = {
    // Config header: 1 config, ITF_NUM_TOTAL interfaces, no string, total len, no attr, 100mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // MIDI interface: AC + MIDI streaming, OUT ep 0x01, IN ep 0x81, 64-byte packets
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, EPNUM_MIDI, (0x80u | EPNUM_MIDI), 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ─── String descriptors ────────────────────────────────────────

static char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   // 0: English (0x0409)
    "CozyCactus",                    // 1: Manufacturer
    "ReBirth Sequencer",             // 2: Product
    NULL,                            // 3: Serial (built dynamically)
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        const char *str;

        if (index == 3) {
            // Build serial number from unique board ID (16 hex chars)
            static char serial[17];
            pico_unique_board_id_t id;
            pico_get_unique_board_id(&id);
            static const char hex[] = "0123456789ABCDEF";
            for (int i = 0; i < 8; i++) {
                serial[i * 2]     = hex[id.id[i] >> 4];
                serial[i * 2 + 1] = hex[id.id[i] & 0xF];
            }
            serial[16] = '\0';
            str = serial;
        } else if (index < (uint8_t)(sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            str = string_desc_arr[index];
        } else {
            return NULL;
        }

        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;

        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return _desc_str;
}
