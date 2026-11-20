#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

#include <stdbool.h>

// What a load did. A failed load is zero and leaves RAM untouched.
typedef enum {
    FLASH_LOAD_FAILED = 0,
    FLASH_LOAD_OK,          // the current save
    FLASH_LOAD_OLD_SAVE,    // the eight-track save: no valid current one exists
} flash_load_t;

// Check whether flash holds a save that flash_storage_load() would load.
bool flash_storage_is_valid(void);

// Core 0 only: save while stopped, drain MIDI, then pause Core 1 for the write.
// Returns true after verifying the saved checksum. New saves use version 7.
bool flash_storage_save(void);

// Core 0 only, while stopped. Validates before replacing RAM and refreshing
// prepared playback. Loads the version 7 save when it is valid and the
// eight-track save otherwise: a save cut short cannot be told from a slot that
// was never written. Eight-track version 6 saves keep their kit data. Version
// 5 and version 4 saves still load: kit steps come back empty and kit settings
// at their defaults.
flash_load_t flash_storage_load(void);

#endif // FLASH_STORAGE_H
