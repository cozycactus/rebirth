/* Firmware, 64-track project, MIDI Song data, legacy eight-track project. */
ASSERT(__flash_binary_end <= 0x100b0000, "Firmware overlaps the ReBirth 64-track save slot.");
ASSERT(LENGTH(FLASH) >= 0x00200000, "ReBirth requires 2 MiB of flash for its fixed save slots.");
