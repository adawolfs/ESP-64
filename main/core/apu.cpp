#include "apu.h"

#include <math.h>
#include <string.h>

namespace {

static constexpr uint32_t GB_M_CYCLES_PER_SECOND = 1048576;
static constexpr uint16_t SAMPLE_RATE = 11025;
static constexpr uint32_t SAMPLE_STEP_Q16 =
    static_cast<uint32_t>((static_cast<uint64_t>(GB_M_CYCLES_PER_SECOND) << 16) /
                          SAMPLE_RATE);
static constexpr uint32_t FRAME_SEQUENCER_CYCLES = 2048;
static constexpr size_t SAMPLE_BUFFER_SIZE = 4096;

struct PulseChannel {
  bool enabled = false;
  bool length_enabled = false;
  uint16_t length_counter = 0;
  uint16_t frequency = 0;
  uint8_t duty = 0;
  uint8_t volume = 0;
  uint8_t envelope_period = 0;
  uint8_t envelope_timer = 0;
  int8_t envelope_direction = 0;
  float phase = 0.0f;
};

struct WaveChannel {
  bool enabled = false;
  bool dac_enabled = false;
  bool length_enabled = false;
  uint16_t length_counter = 0;
  uint16_t frequency = 0;
  uint8_t volume_code = 0;
  float phase = 0.0f;
};

struct NoiseChannel {
  bool enabled = false;
  bool length_enabled = false;
  uint16_t length_counter = 0;
  uint8_t volume = 0;
  uint8_t envelope_period = 0;
  uint8_t envelope_timer = 0;
  int8_t envelope_direction = 0;
  uint8_t clock_shift = 0;
  uint8_t divisor_code = 0;
  bool width_mode = false;
  uint16_t lfsr = 0x7FFF;
  float phase = 0.0f;
};

uint8_t regs[0x30];
PulseChannel pulse1;
PulseChannel pulse2;
WaveChannel wave;
NoiseChannel noise;

uint8_t sample_buffer[SAMPLE_BUFFER_SIZE];
size_t sample_read_index = 0;
size_t sample_write_index = 0;
size_t sample_count = 0;

unsigned int prev_cycles = 0;
uint32_t sample_elapsed_q16 = 0;
uint32_t frame_elapsed_cycles = 0;
uint8_t frame_step = 0;

uint8_t &reg(uint16_t address) { return regs[address - 0xFF10]; }

uint16_t pulse_frequency(uint16_t base_address) {
  return reg(base_address + 3) | ((reg(base_address + 4) & 0x07) << 8);
}

float pulse_hz(uint16_t frequency) {
  if (frequency >= 2048) return 0.0f;
  return 131072.0f / static_cast<float>(2048 - frequency);
}

float wave_hz(uint16_t frequency) {
  if (frequency >= 2048) return 0.0f;
  return 65536.0f / static_cast<float>(2048 - frequency);
}

float noise_hz() {
  static constexpr uint8_t divisors[8] = {8, 16, 32, 48, 64, 80, 96, 112};
  const uint32_t divisor = divisors[noise.divisor_code & 0x07];
  return 524288.0f / static_cast<float>(divisor << noise.clock_shift);
}

void push_sample(uint8_t sample) {
  if (sample_count == SAMPLE_BUFFER_SIZE) {
    sample_read_index = (sample_read_index + 1) % SAMPLE_BUFFER_SIZE;
    sample_count--;
  }
  sample_buffer[sample_write_index] = sample;
  sample_write_index = (sample_write_index + 1) % SAMPLE_BUFFER_SIZE;
  sample_count++;
}

void trigger_pulse(PulseChannel &channel, uint16_t base_address) {
  channel.enabled = true;
  channel.duty = reg(base_address + 1) >> 6;
  channel.length_enabled = reg(base_address + 4) & 0x40;
  channel.length_counter = 64 - (reg(base_address + 1) & 0x3F);
  if (channel.length_counter == 0) channel.length_counter = 64;
  channel.frequency = pulse_frequency(base_address);
  channel.volume = reg(base_address + 2) >> 4;
  channel.envelope_direction = (reg(base_address + 2) & 0x08) ? 1 : -1;
  channel.envelope_period = reg(base_address + 2) & 0x07;
  channel.envelope_timer = channel.envelope_period ? channel.envelope_period : 8;
  if ((reg(base_address + 2) & 0xF8) == 0) channel.enabled = false;
}

void trigger_wave() {
  wave.dac_enabled = reg(0xFF1A) & 0x80;
  wave.enabled = wave.dac_enabled;
  wave.length_enabled = reg(0xFF1E) & 0x40;
  wave.length_counter = 256 - reg(0xFF1B);
  if (wave.length_counter == 0) wave.length_counter = 256;
  wave.volume_code = (reg(0xFF1C) >> 5) & 0x03;
  wave.frequency = reg(0xFF1D) | ((reg(0xFF1E) & 0x07) << 8);
  wave.phase = 0.0f;
}

void trigger_noise() {
  noise.enabled = true;
  noise.length_enabled = reg(0xFF23) & 0x40;
  noise.length_counter = 64 - (reg(0xFF20) & 0x3F);
  if (noise.length_counter == 0) noise.length_counter = 64;
  noise.volume = reg(0xFF21) >> 4;
  noise.envelope_direction = (reg(0xFF21) & 0x08) ? 1 : -1;
  noise.envelope_period = reg(0xFF21) & 0x07;
  noise.envelope_timer = noise.envelope_period ? noise.envelope_period : 8;
  noise.clock_shift = reg(0xFF22) >> 4;
  noise.width_mode = reg(0xFF22) & 0x08;
  noise.divisor_code = reg(0xFF22) & 0x07;
  noise.lfsr = 0x7FFF;
  if ((reg(0xFF21) & 0xF8) == 0) noise.enabled = false;
}

void clock_length(PulseChannel &channel) {
  if (channel.enabled && channel.length_enabled && channel.length_counter) {
    channel.length_counter--;
    if (!channel.length_counter) channel.enabled = false;
  }
}

void clock_length(WaveChannel &channel) {
  if (channel.enabled && channel.length_enabled && channel.length_counter) {
    channel.length_counter--;
    if (!channel.length_counter) channel.enabled = false;
  }
}

void clock_length(NoiseChannel &channel) {
  if (channel.enabled && channel.length_enabled && channel.length_counter) {
    channel.length_counter--;
    if (!channel.length_counter) channel.enabled = false;
  }
}

void clock_envelope(PulseChannel &channel) {
  if (!channel.enabled || !channel.envelope_period) return;
  if (--channel.envelope_timer) return;
  channel.envelope_timer = channel.envelope_period;
  const int next_volume = channel.volume + channel.envelope_direction;
  if (next_volume >= 0 && next_volume <= 15) channel.volume = next_volume;
}

void clock_envelope(NoiseChannel &channel) {
  if (!channel.enabled || !channel.envelope_period) return;
  if (--channel.envelope_timer) return;
  channel.envelope_timer = channel.envelope_period;
  const int next_volume = channel.volume + channel.envelope_direction;
  if (next_volume >= 0 && next_volume <= 15) channel.volume = next_volume;
}

void clock_frame_sequencer() {
  if (frame_step == 0 || frame_step == 2 || frame_step == 4 || frame_step == 6) {
    clock_length(pulse1);
    clock_length(pulse2);
    clock_length(wave);
    clock_length(noise);
  }
  if (frame_step == 7) {
    clock_envelope(pulse1);
    clock_envelope(pulse2);
    clock_envelope(noise);
  }
  frame_step = (frame_step + 1) & 0x07;
}

float render_pulse(PulseChannel &channel) {
  if (!channel.enabled || !channel.volume) return 0.0f;
  static constexpr float duty_ratio[4] = {0.125f, 0.25f, 0.5f, 0.75f};
  const float hz = pulse_hz(channel.frequency);
  if (hz <= 0.0f) return 0.0f;

  channel.phase += hz / SAMPLE_RATE;
  while (channel.phase >= 1.0f) channel.phase -= 1.0f;

  const float amp = channel.phase < duty_ratio[channel.duty & 3] ? 1.0f : -1.0f;
  return amp * (static_cast<float>(channel.volume) / 15.0f);
}

float render_wave() {
  if (!wave.enabled || !wave.dac_enabled || wave.volume_code == 0) return 0.0f;
  const float hz = wave_hz(wave.frequency);
  if (hz <= 0.0f) return 0.0f;

  wave.phase += hz / SAMPLE_RATE;
  while (wave.phase >= 1.0f) wave.phase -= 1.0f;

  const uint8_t sample_index = static_cast<uint8_t>(wave.phase * 32.0f) & 31;
  const uint8_t packed = reg(0xFF30 + (sample_index >> 1));
  uint8_t sample = sample_index & 1 ? (packed & 0x0F) : (packed >> 4);

  switch (wave.volume_code) {
    case 1:
      break;
    case 2:
      sample >>= 1;
      break;
    case 3:
      sample >>= 2;
      break;
  }
  return (static_cast<float>(sample) - 7.5f) / 7.5f;
}

float render_noise() {
  if (!noise.enabled || !noise.volume) return 0.0f;
  const float hz = noise_hz();
  if (hz <= 0.0f) return 0.0f;

  noise.phase += hz / SAMPLE_RATE;
  while (noise.phase >= 1.0f) {
    noise.phase -= 1.0f;
    const uint16_t bit = (noise.lfsr ^ (noise.lfsr >> 1)) & 1;
    noise.lfsr = (noise.lfsr >> 1) | (bit << 14);
    if (noise.width_mode) {
      noise.lfsr = (noise.lfsr & ~(1u << 6)) | (bit << 6);
    }
  }

  const float amp = (noise.lfsr & 1) ? -1.0f : 1.0f;
  return amp * (static_cast<float>(noise.volume) / 15.0f);
}

void generate_sample() {
  if (!(reg(0xFF26) & 0x80)) {
    push_sample(128);
    return;
  }

  float mixed = 0.0f;
  mixed += render_pulse(pulse1);
  mixed += render_pulse(pulse2);
  mixed += render_wave();
  mixed += render_noise();
  mixed *= 0.18f;

  if (mixed > 1.0f) mixed = 1.0f;
  if (mixed < -1.0f) mixed = -1.0f;
  push_sample(static_cast<uint8_t>(128.0f + mixed * 110.0f));
}

}  // namespace

void apu_init(void) {
  memset(regs, 0, sizeof(regs));
  regs[0xFF10 - 0xFF10] = 0x80;
  regs[0xFF11 - 0xFF10] = 0xBF;
  regs[0xFF12 - 0xFF10] = 0xF3;
  regs[0xFF14 - 0xFF10] = 0xBF;
  regs[0xFF16 - 0xFF10] = 0x3F;
  regs[0xFF19 - 0xFF10] = 0xBF;
  regs[0xFF1A - 0xFF10] = 0x7F;
  regs[0xFF1B - 0xFF10] = 0xFF;
  regs[0xFF1C - 0xFF10] = 0x9F;
  regs[0xFF1E - 0xFF10] = 0xBF;
  regs[0xFF20 - 0xFF10] = 0xFF;
  regs[0xFF23 - 0xFF10] = 0xBF;
  regs[0xFF24 - 0xFF10] = 0x77;
  regs[0xFF25 - 0xFF10] = 0xF3;
  regs[0xFF26 - 0xFF10] = 0xF1;

  pulse1 = PulseChannel{};
  pulse2 = PulseChannel{};
  wave = WaveChannel{};
  noise = NoiseChannel{};
  sample_read_index = 0;
  sample_write_index = 0;
  sample_count = 0;
  prev_cycles = 0;
  sample_elapsed_q16 = 0;
  frame_elapsed_cycles = 0;
  frame_step = 0;
}

void apu_cycle(unsigned int cycles) {
  const unsigned int delta = cycles - prev_cycles;
  prev_cycles = cycles;

  frame_elapsed_cycles += delta;
  while (frame_elapsed_cycles >= FRAME_SEQUENCER_CYCLES) {
    frame_elapsed_cycles -= FRAME_SEQUENCER_CYCLES;
    clock_frame_sequencer();
  }

  sample_elapsed_q16 += delta << 16;
  while (sample_elapsed_q16 >= SAMPLE_STEP_Q16) {
    sample_elapsed_q16 -= SAMPLE_STEP_Q16;
    generate_sample();
  }
}

uint8_t apu_read_register(uint16_t address) {
  if (address < 0xFF10 || address > 0xFF3F) return 0xFF;
  if (address == 0xFF26) {
    uint8_t status = reg(0xFF26) & 0x80;
    if (pulse1.enabled) status |= 0x01;
    if (pulse2.enabled) status |= 0x02;
    if (wave.enabled) status |= 0x04;
    if (noise.enabled) status |= 0x08;
    return status | 0x70;
  }
  return reg(address);
}

void apu_write_register(uint16_t address, uint8_t value) {
  if (address < 0xFF10 || address > 0xFF3F) return;

  if (address == 0xFF26) {
    reg(address) = (value & 0x80) | (reg(address) & 0x0F);
    if (!(value & 0x80)) {
      pulse1.enabled = false;
      pulse2.enabled = false;
      wave.enabled = false;
      noise.enabled = false;
    }
    return;
  }

  reg(address) = value;

  switch (address) {
    case 0xFF12:
      if ((value & 0xF8) == 0) pulse1.enabled = false;
      break;
    case 0xFF17:
      if ((value & 0xF8) == 0) pulse2.enabled = false;
      break;
    case 0xFF1A:
      wave.dac_enabled = value & 0x80;
      if (!wave.dac_enabled) wave.enabled = false;
      break;
    case 0xFF21:
      if ((value & 0xF8) == 0) noise.enabled = false;
      break;
    case 0xFF14:
      pulse1.frequency = pulse_frequency(0xFF10);
      pulse1.length_enabled = value & 0x40;
      if (value & 0x80) trigger_pulse(pulse1, 0xFF10);
      break;
    case 0xFF19:
      pulse2.frequency = pulse_frequency(0xFF15);
      pulse2.length_enabled = value & 0x40;
      if (value & 0x80) trigger_pulse(pulse2, 0xFF15);
      break;
    case 0xFF1E:
      wave.frequency = reg(0xFF1D) | ((value & 0x07) << 8);
      wave.length_enabled = value & 0x40;
      if (value & 0x80) trigger_wave();
      break;
    case 0xFF23:
      noise.length_enabled = value & 0x40;
      if (value & 0x80) trigger_noise();
      break;
    default:
      if (address == 0xFF13) pulse1.frequency = pulse_frequency(0xFF10);
      if (address == 0xFF18) pulse2.frequency = pulse_frequency(0xFF15);
      if (address == 0xFF1D) wave.frequency = reg(0xFF1D) | ((reg(0xFF1E) & 0x07) << 8);
      break;
  }
}

size_t apu_available_samples(void) { return sample_count; }

size_t apu_read_samples(uint8_t *out, size_t max_samples) {
  if (!out || !max_samples) return 0;
  size_t count = 0;
  while (count < max_samples && sample_count) {
    out[count++] = sample_buffer[sample_read_index];
    sample_read_index = (sample_read_index + 1) % SAMPLE_BUFFER_SIZE;
    sample_count--;
  }
  return count;
}

void apu_clear_samples(void) {
  sample_read_index = 0;
  sample_write_index = 0;
  sample_count = 0;
}

uint16_t apu_sample_rate(void) { return SAMPLE_RATE; }
