#ifndef ROM_H
#define ROM_H
#ifdef __cplusplus

extern "C" {

#endif
int rom_load(const char *);
int rom_init(const unsigned char *);
const unsigned char *rom_getbytes(void);
unsigned int rom_get_mapper(void);
unsigned int rom_get_bank_count(void);
unsigned int rom_get_ram_bank_count(void);
unsigned char rom_get_type(void);
const char *rom_get_title(void);
unsigned char rom_get_version(void);
unsigned char rom_get_header_checksum(void);
bool rom_has_battery(void);

enum {
  NROM,
  MBC1,
  MBC2,
  MMM01,
  MBC3,
  MBC4,
  MBC5,
};
#ifdef __cplusplus
}

#endif /* end of __cplusplus */
#endif
