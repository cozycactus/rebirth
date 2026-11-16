#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

#include <stdbool.h>

// Check whether a valid save exists in flash.
bool flash_storage_is_valid(void);

// Core 0 only: save while stopped, drain MIDI, then pause Core 1 for the write.
// Returns true after verifying the saved checksum. New saves use version 5.
bool flash_storage_save(void);

// Core 0 only, while stopped. Validates before replacing RAM and refreshing
// prepared playback. Version 4 saves can still be loaded (without a checksum).
bool flash_storage_load(void);

#endif // FLASH_STORAGE_H
