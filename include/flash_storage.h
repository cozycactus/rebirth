#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

#include <stdbool.h>

// Check whether a valid save exists in flash.
bool flash_storage_is_valid(void);

// Core 0 only: save while stopped, drain MIDI, then pause Core 1 for the write.
// Returns true after verifying the saved checksum. New saves use version 7.
bool flash_storage_save(void);

// Core 0 only, while stopped. Validates before replacing RAM and refreshing
// prepared playback. Eight-track version 6 saves keep their kit data. Version 5 and version 4 saves still load: kit steps
// come back empty and kit settings at their defaults.
bool flash_storage_load(void);

#endif // FLASH_STORAGE_H
