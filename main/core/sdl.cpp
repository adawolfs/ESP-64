#include "sdl.h"

#include "board_config.h"
#include "display.h"
#include "touch_input.h"

static uint8_t frame_buf[board::GAMEBOY_WIDTH * board::GAMEBOY_HEIGHT];

unsigned int sdl_get_buttons(void) { return touch_input_buttons(); }
unsigned int sdl_get_directions(void) { return touch_input_directions(); }
uint8_t *sdl_get_framebuffer(void) { return frame_buf; }

void sdl_poll_input(void) { touch_input_update(); }

void sdl_frame(void) { display_present_indexed_frame(frame_buf); }

int sdl_update(void) {
  sdl_poll_input();
  sdl_frame();
  return 0;
}

void sdl_quit(void) {}

void sdl_init(void) {
  display_init();
  touch_input_init();
}
