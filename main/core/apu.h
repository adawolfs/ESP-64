#ifndef APU_H
#define APU_H

#include <stddef.h>
#include <stdint.h>

void apu_init(void);
void apu_cycle(unsigned int cycles);
uint8_t apu_read_register(uint16_t address);
void apu_write_register(uint16_t address, uint8_t value);
size_t apu_available_samples(void);
size_t apu_read_samples(uint8_t *out, size_t max_samples);
void apu_clear_samples(void);
uint16_t apu_sample_rate(void);

#endif
