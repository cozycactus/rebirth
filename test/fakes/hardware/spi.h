#include <stdint.h>
#define spi0 ((void *)0)
#define spi1 ((void *)1)
void spi_init(void *spi, uint32_t rate);
void spi_write_blocking(void *spi, const uint8_t *data, int length);
