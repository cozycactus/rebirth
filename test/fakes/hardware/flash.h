#ifndef REBIRTH_FAKE_FLASH_H
#define REBIRTH_FAKE_FLASH_H
#include <stdint.h>
#include <stddef.h>
extern uint8_t fake_flash[2 * 1024 * 1024];
extern unsigned fake_flash_erases;
extern unsigned fake_flash_programs;
extern uint32_t fake_flash_last_program_offset;
extern unsigned fake_flash_program_limit;
#define XIP_BASE ((uintptr_t)fake_flash)
void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);
#endif
