/* Firmware, MIDI Song data, then the 128 KiB of clip saves. */
ASSERT(__flash_binary_end <= 0x100e0000, "Firmware overlaps the ReBirth MIDI Song slot.");
