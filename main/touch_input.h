#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void touch_input_init(void);
void touch_input_update(void);
void touch_input_maintain(uint32_t now_ms);
void touch_input_set_web_control(const char *control, bool pressed,
                                 uint32_t now_ms);
void touch_input_clear_web_controls(void);
unsigned int touch_input_buttons(void);
unsigned int touch_input_directions(void);
unsigned int touch_input_web_buttons(void);
unsigned int touch_input_web_directions(void);

#ifdef __cplusplus
}
#endif

#endif
