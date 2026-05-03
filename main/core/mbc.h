#ifndef MBC_H
#define MBC_H
void MBC_reset(void);
unsigned int MBC1_write_byte(unsigned short, unsigned char);
unsigned int MBC3_write_byte(unsigned short, unsigned char);
unsigned int MBC5_write_byte(unsigned short, unsigned char);
unsigned int MBC_get_ram_bank(void);
bool MBC_is_ram_enabled(void);
#endif
