
#include "rom.h"

#include <stdio.h>
#include <string.h>

const unsigned char *bytes;
unsigned int mapper;
static unsigned int rom_bank_count;
static unsigned int ram_bank_count;
static unsigned char cartridge_type;
static unsigned char cartridge_version;
static unsigned char header_checksum;
static char title[17];

extern "C" {

static char *carts[] = {
    /*
          [0x00] = "ROM ONLY",
          [0x01] = "MBC1",
          [0x02] = "MBC1+RAM",
          [0x03] = "MBC1+RAM+BATTERY",
          [0x05] = "MBC2",
          [0x06] = "MBC2+BATTERY",
          [0x08] = "ROM+RAM",
          [0x09] = "ROM+RAM+BATTERY",
          [0x0B] = "MMM01",
          [0x0C] = "MMM01+RAM",
          [0x0D] = "MMM01+RAM+BATTERY",
          [0x0F] = "MBC3+TIMER+BATTERY",
          [0x10] = "MBC3+TIMER+RAM+BATTERY",
          [0x11] = "MBC3",
          [0x12] = "MBC3+RAM",
          [0x13] = "MBC3+RAM+BATTERY",
          [0x15] = "MBC4",
          [0x16] = "MBC4+RAM",
          [0x17] = "MBC4+RAM+BATTERY",
          [0x19] = "MBC5",
          [0x1A] = "MBC5+RAM",
          [0x1B] = "MBC5+RAM+BATTERY",
          [0x1C] = "MBC5+RUMBLE",
          [0x1D] = "MBC5+RUMBLE+RAM",
          [0x1E] = "MBC5+RUMBLE+RAM+BATTERY",
          [0xFC] = "POCKET CAMERA",
          [0xFD] = "BANDAI TAMA5",
          [0xFE] = "HuC3",
          [0xFF] = "HuC1+RAM+BATTERY",
    */
};
}

static const char *banks[] = {" 32KiB", " 64KiB", "128KiB", "256KiB", "512KiB",
                              "  1MiB", "  2MiB", "  4MiB",
                              /* 0x52 */
                              "1.1MiB", "1.2MiB", "1.5MiB", "Unknown"};

static const char *rams[] = {"None", "  2KiB", "  8KiB", " 32KiB", "Unknown"};

static const char *regions[] = {"Japan", "Non-Japan", "Unknown"};

static unsigned char header[] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
    0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
    0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E};

int rom_init(const unsigned char *rombytes) {
  int type, bank_index, ram, region, version, i, pass;
  unsigned char checksum = 0;

  if (memcmp(&rombytes[0x104], header, sizeof(header)) != 0) return 0;

  memcpy(title, &rombytes[0x134], 16);
  title[16] = '\0';
  printf("Rom title: %s\n", title);

  type = rombytes[0x147];
  cartridge_type = type;

  // printf("Cartridge type: %s (%02X)\n", carts[type], type);

  bank_index = rombytes[0x148];
  /* Adjust for the gap in the bank indicies */
  if (bank_index >= 0x52 && bank_index <= 0x54)
    bank_index -= 74;
  else if (bank_index > 7)
    bank_index = 11;

  printf("Rom size: %s\n", banks[bank_index]);
  switch (rombytes[0x148]) {
    case 0x00: rom_bank_count = 2; break;
    case 0x01: rom_bank_count = 4; break;
    case 0x02: rom_bank_count = 8; break;
    case 0x03: rom_bank_count = 16; break;
    case 0x04: rom_bank_count = 32; break;
    case 0x05: rom_bank_count = 64; break;
    case 0x06: rom_bank_count = 128; break;
    case 0x07: rom_bank_count = 256; break;
    case 0x08: rom_bank_count = 512; break;
    case 0x52: rom_bank_count = 72; break;
    case 0x53: rom_bank_count = 80; break;
    case 0x54: rom_bank_count = 96; break;
    default: rom_bank_count = 2; break;
  }

  ram = rombytes[0x149];
  if (ram > 3) ram = 4;

  printf("RAM size: %s\n", rams[ram]);
  switch (rombytes[0x149]) {
    case 0x01:
    case 0x02:
      ram_bank_count = 1;
      break;
    case 0x03:
      ram_bank_count = 4;
      break;
    case 0x04:
      ram_bank_count = 16;
      break;
    case 0x05:
      ram_bank_count = 8;
      break;
    default:
      ram_bank_count = 0;
      break;
  }

  region = rombytes[0x14A];
  if (region > 2) region = 2;
  printf("Region: %s\n", regions[region]);

  version = rombytes[0x14C];
  cartridge_version = version;
  printf("Version: %02X\n", version);

  for (i = 0x134; i <= 0x14C; i++) checksum = checksum - rombytes[i] - 1;

  pass = rombytes[0x14D] == checksum;
  header_checksum = rombytes[0x14D];

  printf("Checksum: %s (%02X)\n", pass ? "OK" : "FAIL", checksum);
  if (!pass) return 0;

  bytes = rombytes;

  switch (type) {
    case 0x00:
    case 0x08:
    case 0x09:
      mapper = NROM;
      break;
    case 0x01:
    case 0x02:
    case 0x03:
      mapper = MBC1;
      break;
    case 0x05:
    case 0x06:
      mapper = MBC2;
      break;
    case 0x0B:
    case 0x0C:
      mapper = MMM01;
      break;
    case 0x0F:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
      mapper = MBC3;
      break;
    case 0x15:
    case 0x16:
    case 0x17:
      mapper = MBC4;
      break;
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
      mapper = MBC5;
      break;
  }

  return 1;
}

unsigned int rom_get_mapper(void) { return mapper; }
unsigned int rom_get_bank_count(void) { return rom_bank_count; }
unsigned int rom_get_ram_bank_count(void) { return ram_bank_count; }
unsigned char rom_get_type(void) { return cartridge_type; }
const char *rom_get_title(void) { return title; }
unsigned char rom_get_version(void) { return cartridge_version; }
unsigned char rom_get_header_checksum(void) { return header_checksum; }

bool rom_has_battery(void) {
  switch (cartridge_type) {
    case 0x03:
    case 0x06:
    case 0x09:
    case 0x0D:
    case 0x0F:
    case 0x10:
    case 0x13:
    case 0x17:
    case 0x1B:
    case 0x1E:
    case 0x22:
    case 0xFF:
      return true;
    default:
      return false;
  }
}

int rom_load(const char *filename) {
  /*
#ifdef _WIN32
        HANDLE f, map;
#else
        int f;
        size_t length;
        struct stat st;
#endif
        unsigned char *bytes;

#ifdef _WIN32
        f = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if(f == INVALID_HANDLE_VALUE)
                return 0;

        map = CreateFileMapping(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if(!map)
                return 0;

        bytes = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
        if(!bytes)
                return 0;
#else
        f = open(filename, O_RDONLY);
        if(f == -1)
                return 0;
        if(fstat(f, &st) == -1)
                return 0;

        bytes = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, f, 0);
        if(!bytes)
                return 0;
#endif
*/

  // return rom_init(bytes);
  return 0;
}
const unsigned char *rom_getbytes(void) { return bytes; }
