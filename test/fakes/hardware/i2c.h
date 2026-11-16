#ifndef REBIRTH_FAKE_I2C_H
#define REBIRTH_FAKE_I2C_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_ERROR_TIMEOUT (-2)

typedef struct i2c_inst { int unused; } i2c_inst_t;
extern i2c_inst_t fake_i2c0_inst;
#define i2c0 (&fake_i2c0_inst)

unsigned i2c_init(i2c_inst_t *i2c, unsigned baudrate);
int i2c_write_timeout_us(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len,
                         bool nostop, unsigned timeout_us);
int i2c_read_timeout_us(i2c_inst_t *i2c, uint8_t addr, uint8_t *dst, size_t len,
                        bool nostop, unsigned timeout_us);

#endif
