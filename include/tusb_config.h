#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

// ─── Common ─────────────────────────────────────────────────────
// CFG_TUSB_MCU is defined by the pico-sdk cmake target (OPT_MCU_RP2040)

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUSB_OS             OPT_OS_PICO
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG          0
#endif

#define CFG_TUD_ENDPOINT0_SIZE  64

// ─── Device classes ─────────────────────────────────────────────
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            1
#define CFG_TUD_VENDOR          0
#define CFG_TUD_AUDIO           0

// MIDI FIFO sizes
#define CFG_TUD_MIDI_RX_BUFSIZE 64
#define CFG_TUD_MIDI_TX_BUFSIZE 128

#endif // TUSB_CONFIG_H
