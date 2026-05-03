#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

void display_init(void);
void display_present_indexed_frame(const uint8_t *framebuffer);
void display_show_status(const char *line1, const char *line2);

#endif
