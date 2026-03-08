#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

#include <stdbool.h>

// Check whether a valid save exists in flash.
bool flash_storage_is_valid(void);

// Save all patterns and song to flash. Pauses Core 1 during the write.
// Returns true on success.
bool flash_storage_save(void);

// Load patterns and song from flash. Returns false if no valid save exists.
bool flash_storage_load(void);

#endif // FLASH_STORAGE_H
