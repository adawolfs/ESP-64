// If INTER_MODULE_OPT macro is defined,
// this file is included into cpu.cpp
// to make inter module optimizations possible
#ifndef INTER_MODULE_OPT

#include "mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_config.h"
#include "cpu.h"
#include "gb_time.h"
#include "interrupt.h"
#include "lcd.h"
#include "mbc.h"
#include "rom.h"
#include "sdl.h"
#include "timer.h"

#if GB_ENABLE_AUDIO
#include "apu.h"
#endif

static unsigned char *mem;
static unsigned char *cart_ram;
static unsigned int cart_ram_bank_count;
static const unsigned char *switchable_rom;
static unsigned int current_rom_bank = 1;
static int DMA_pending = 0;
static unsigned char joypad_select = 0x30;
static uint32_t bank_switches = 0;
static bool cart_ram_dirty = false;
static bool cart_ram_flush_requested = false;
static uint32_t last_cart_ram_write_ms = 0;

namespace {

constexpr const char *kSavePath = "/spiffs/sram.bin";
constexpr const char *kSaveTempPath = "/spiffs/sram.tmp";
constexpr uint32_t kSaveMagic = 0x53415645u;

struct SaveMetadata {
  uint32_t magic;
  uint32_t ram_size;
  uint32_t rom_bank_count;
  uint8_t cartridge_type;
  uint8_t version;
  uint8_t header_checksum;
  char title[17];
};

bool path_exists(const char *path) {
  struct stat st;
  return ::stat(path, &st) == 0;
}

size_t cart_ram_size_bytes() { return cart_ram_bank_count * 0x2000u; }

bool has_persistent_cart_ram() {
  return cart_ram && cart_ram_bank_count > 0 && rom_has_battery();
}

SaveMetadata build_save_metadata() {
  SaveMetadata metadata = {};
  metadata.magic = kSaveMagic;
  metadata.ram_size = cart_ram_size_bytes();
  metadata.rom_bank_count = rom_get_bank_count();
  metadata.cartridge_type = rom_get_type();
  metadata.version = rom_get_version();
  metadata.header_checksum = rom_get_header_checksum();
  strncpy(metadata.title, rom_get_title(), sizeof(metadata.title) - 1);
  return metadata;
}

bool save_metadata_matches(const SaveMetadata &metadata) {
  const SaveMetadata current = build_save_metadata();
  return metadata.magic == current.magic &&
         metadata.ram_size == current.ram_size &&
         metadata.rom_bank_count == current.rom_bank_count &&
         metadata.cartridge_type == current.cartridge_type &&
         metadata.version == current.version &&
         metadata.header_checksum == current.header_checksum &&
         strncmp(metadata.title, current.title, sizeof(metadata.title)) == 0;
}

long file_size(FILE *fp) {
  if (fseek(fp, 0, SEEK_END) != 0) return -1;
  const long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return size;
}

void load_persistent_cart_ram() {
  if (!has_persistent_cart_ram()) return;
  if (!path_exists(kSavePath)) return;

  FILE *fp = fopen(kSavePath, "rb");
  if (!fp) return;

  const size_t expected_ram = cart_ram_size_bytes();
  const size_t expected_total = sizeof(SaveMetadata) + expected_ram;
  const long actual = file_size(fp);
  if (actual < 0 || static_cast<size_t>(actual) != expected_total) {
    fclose(fp);
    printf("SRAM file size mismatch (%ld vs %u); ignoring\n", actual,
           static_cast<unsigned int>(expected_total));
    return;
  }

  SaveMetadata metadata = {};
  if (fread(&metadata, 1, sizeof(metadata), fp) != sizeof(metadata)) {
    fclose(fp);
    return;
  }
  if (!save_metadata_matches(metadata)) {
    fclose(fp);
    printf("SRAM metadata mismatch for %s; ignoring\n", metadata.title);
    return;
  }

  const size_t loaded = fread(cart_ram, 1, expected_ram, fp);
  fclose(fp);

  if (loaded == expected_ram) {
    printf("Loaded persistent SRAM (%u bytes)\n",
           static_cast<unsigned int>(loaded));
  }
}

bool save_persistent_cart_ram() {
  if (!has_persistent_cart_ram()) return false;

  const SaveMetadata metadata = build_save_metadata();
  const size_t expected_ram = cart_ram_size_bytes();

  if (path_exists(kSaveTempPath)) ::unlink(kSaveTempPath);

  FILE *fp = fopen(kSaveTempPath, "wb");
  if (!fp) {
    printf("SRAM save failed: cannot open temp file\n");
    return false;
  }

  const size_t meta_written = fwrite(&metadata, 1, sizeof(metadata), fp);
  const size_t data_written = fwrite(cart_ram, 1, expected_ram, fp);
  fflush(fp);
  fclose(fp);

  if (meta_written != sizeof(metadata) || data_written != expected_ram) {
    ::unlink(kSaveTempPath);
    printf("SRAM save failed: short write (%u/%u + %u/%u)\n",
           static_cast<unsigned int>(meta_written),
           static_cast<unsigned int>(sizeof(metadata)),
           static_cast<unsigned int>(data_written),
           static_cast<unsigned int>(expected_ram));
    return false;
  }

  if (path_exists(kSavePath)) ::unlink(kSavePath);
  if (::rename(kSaveTempPath, kSavePath) != 0) {
    ::unlink(kSaveTempPath);
    printf("SRAM save failed: rename failed\n");
    return false;
  }

  printf("Saved persistent SRAM (%u bytes)\n",
         static_cast<unsigned int>(expected_ram));
  return true;
}

}  // namespace

void mem_request_save_flush(void) {
  if (cart_ram_dirty) cart_ram_flush_requested = true;
}

static void release_memory_buffers() {
  free(mem);
  free(cart_ram);
  mem = nullptr;
  cart_ram = nullptr;
}

uint32_t mem_get_bank_switches() { return bank_switches; }

void mem_bank_switch(unsigned int n) {
  const unsigned char *b = rom_getbytes();
  const unsigned int bank_count = rom_get_bank_count();

  if (bank_count) n %= bank_count;
  if (n == current_rom_bank) return;

  switchable_rom = &b[n * 0x4000];
  current_rom_bank = n;
  bank_switches++;
}

/* LCD's access to VRAM */
const unsigned char *mem_get_raw() { return mem; }

unsigned char mem_get_joypad_register(void) {
  unsigned char lower = 0x0F;
  if (!(joypad_select & 0x10)) lower &= 0x0F ^ (sdl_get_directions() & 0x0F);
  if (!(joypad_select & 0x20)) lower &= 0x0F ^ (sdl_get_buttons() & 0x0F);
  return 0xC0 | joypad_select | lower;
}

static bool has_cart_ram() { return cart_ram && cart_ram_bank_count > 0; }

static unsigned int active_cart_ram_bank() {
  if (!cart_ram_bank_count) return 0;
  return MBC_get_ram_bank() % cart_ram_bank_count;
}

static unsigned char cart_ram_read(unsigned short i) {
  if (!has_cart_ram()) return 0xFF;
  if (rom_get_mapper() != NROM && !MBC_is_ram_enabled()) return 0xFF;
  return cart_ram[active_cart_ram_bank() * 0x2000 + (i - 0xA000)];
}

static void cart_ram_write(unsigned short d, unsigned char i) {
  if (!has_cart_ram()) return;
  if (rom_get_mapper() != NROM && !MBC_is_ram_enabled()) return;
  const size_t offset = active_cart_ram_bank() * 0x2000u + (d - 0xA000);
  if (cart_ram[offset] == i) return;

  cart_ram[offset] = i;
  if (has_persistent_cart_ram()) {
    cart_ram_dirty = true;
    last_cart_ram_write_ms = millis();
  }
}

unsigned char mem_get_byte(unsigned short i) {
  unsigned long elapsed;

  if (DMA_pending && i < 0xFF80) {
    elapsed = cpu_get_cycles() - DMA_pending;
    if (elapsed >= 160)
      DMA_pending = 0;
    else {
      return mem[0xFE00 + elapsed];
    }
  }

  if (i < 0x4000) return mem[i];
  if (i < 0x8000) return switchable_rom[i - 0x4000];
  if (i >= 0xA000 && i < 0xC000) return cart_ram_read(i);
  if (i < 0xFF00) return mem[i];
#if GB_ENABLE_AUDIO
  if (i >= 0xFF10 && i <= 0xFF3F) return apu_read_register(i);
#endif

  switch (i) {
    case 0xFF00: /* Joypad */
      return mem_get_joypad_register();
    case 0xFF04:
      return timer_get_div();
      break;
    case 0xFF05:
      return timer_get_counter();
      break;
    case 0xFF06:
      return timer_get_modulo();
      break;
    case 0xFF07:
      return timer_get_tac();
      break;
    case 0xFF0F:
      return interrupt_get_IF();
      break;
    case 0xFF41:
      return lcd_get_stat();
      break;
    case 0xFF44:
      return lcd_get_line();
      break;
    case 0xFF4D: /* GBC speed switch */
      return 0xFF;
      break;
    case 0xFFFF:
      return interrupt_get_mask();
      break;
  }

  return mem[i];
}

unsigned short mem_get_word(unsigned short i) {
  unsigned long elapsed;

  if (DMA_pending && i < 0xFF80) {
    elapsed = cpu_get_cycles() - DMA_pending;
    if (elapsed >= 160)
      DMA_pending = 0;
    else {
      return mem[0xFE00 + elapsed];
    }
  }
  if (i < 0x3FFF) return mem[i] | (mem[i + 1] << 8);
  if (i >= 0x4000 && i < 0x7FFF) {
    const unsigned int offset = i - 0x4000;
    return switchable_rom[offset] | (switchable_rom[offset + 1] << 8);
  }
  return mem_get_byte(i) | (mem_get_byte(i + 1) << 8);
}

void mem_write_byte(unsigned short d, unsigned char i) {
  unsigned int filtered = 0;

  const bool ram_was_enabled = MBC_is_ram_enabled();

  switch (rom_get_mapper()) {
    case NROM:
      if (d < 0x8000) filtered = 1;
      break;
    case MBC2:
    case MBC3:
      filtered = MBC3_write_byte(d, i);
      break;
    case MBC1:
      filtered = MBC1_write_byte(d, i);
      break;
    case MBC5:
      filtered = MBC5_write_byte(d, i);
      break;
  }

  if (ram_was_enabled && !MBC_is_ram_enabled() && cart_ram_dirty) {
    cart_ram_flush_requested = true;
  }

  if (filtered) return;

  if (d >= 0xA000 && d < 0xC000) {
    cart_ram_write(d, i);
    return;
  }

#if GB_ENABLE_AUDIO
  if (d >= 0xFF10 && d <= 0xFF3F) {
    apu_write_register(d, i);
    mem[d] = i;
    return;
  }
#endif

  switch (d) {
    case 0xFF00: /* Joypad */
      joypad_select = i & 0x30;
      break;
    case 0xFF01: /* Link port data */
      break;
    case 0xFF04:
      timer_set_div(i);
      break;
    case 0xFF05:
      timer_set_counter(i);
      break;
    case 0xFF06:
      timer_set_modulo(i);
      break;
    case 0xFF07:
      timer_set_tac(i);
      break;
    case 0xFF0F:
      interrupt_set_IF(i);
      break;
    case 0xFF40:
      lcd_write_control(i);
      break;
    case 0xFF41:
      lcd_write_stat(i);
      break;
    case 0xFF42:
      lcd_write_scroll_y(i);
      break;
    case 0xFF43:
      lcd_write_scroll_x(i);
      break;
    case 0xFF45:
      lcd_set_ly_compare(i);
      break;
    case 0xFF46: /* OAM DMA */
      for (int offset = 0; offset < 0xA0; ++offset) {
        mem[0xFE00 + offset] = mem_get_byte(((unsigned short)i << 8) + offset);
      }
      DMA_pending = cpu_get_cycles();
      break;
    case 0xFF47:
      lcd_write_bg_palette(i);
      break;
    case 0xFF48:
      lcd_write_spr_palette1(i);
      break;
    case 0xFF49:
      lcd_write_spr_palette2(i);
      break;
    case 0xFF4A:
      lcd_set_window_y(i);
      break;
    case 0xFF4B:
      lcd_set_window_x(i);
      break;
    case 0xFFFF:
      interrupt_set_mask(i);
      return;
      break;
  }

  mem[d] = i;
}

void mem_write_word(unsigned short d, unsigned short i) {
  mem[d] = i & 0xFF;
  mem[d + 1] = i >> 8;
}

bool gameboy_mem_init(void) {
  const unsigned char *bytes = rom_getbytes();

  release_memory_buffers();
  mem = (unsigned char *)calloc(1, 0x10000);
  if (!mem) return false;

  cart_ram_bank_count = rom_get_ram_bank_count();
  cart_ram = cart_ram_bank_count
                 ? (unsigned char *)calloc(cart_ram_bank_count, 0x2000)
                 : nullptr;
  if (cart_ram_bank_count && !cart_ram) {
    release_memory_buffers();
    return false;
  }
  switchable_rom = &bytes[0x4000];
  current_rom_bank = 1;
  joypad_select = 0x30;
  bank_switches = 0;
  cart_ram_dirty = false;
  cart_ram_flush_requested = false;
  last_cart_ram_write_ms = 0;
  MBC_reset();
#if GB_ENABLE_AUDIO
  apu_init();
#endif

  memcpy(&mem[0x0000], &bytes[0x0000], 0x4000);

  mem[0xFF10] = 0x80;
  mem[0xFF11] = 0xBF;
  mem[0xFF12] = 0xF3;
  mem[0xFF14] = 0xBF;
  mem[0xFF16] = 0x3F;
  mem[0xFF19] = 0xBF;
  mem[0xFF1A] = 0x7F;
  mem[0xFF1B] = 0xFF;
  mem[0xFF1C] = 0x9F;
  mem[0xFF1E] = 0xBF;
  mem[0xFF20] = 0xFF;
  mem[0xFF23] = 0xBF;
  mem[0xFF24] = 0x77;
  mem[0xFF25] = 0xF3;
  mem[0xFF26] = 0xF1;
  mem[0xFF40] = 0x91;
  mem[0xFF47] = 0xFC;
  mem[0xFF48] = 0xFF;
  mem[0xFF49] = 0xFF;
  load_persistent_cart_ram();
  return true;
}

void mem_persist(uint32_t now_ms) {
  if (!cart_ram_dirty || !has_persistent_cart_ram()) return;

  const bool debounce_elapsed =
      now_ms >= last_cart_ram_write_ms &&
      now_ms - last_cart_ram_write_ms >= board::SAVE_FLUSH_DEBOUNCE_MS;

  if (!cart_ram_flush_requested && !debounce_elapsed) return;

  if (save_persistent_cart_ram()) {
    cart_ram_dirty = false;
  } else {
    last_cart_ram_write_ms = now_ms;
  }
  cart_ram_flush_requested = false;
}

#endif  // INTER_MODULE_OPT
