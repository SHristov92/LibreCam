/*
 * Status LED patterns. Each state has its own rhythm so the camera's state
 * can be read from across a room:
 *
 *   joining Wi-Fi / booting   fast blink        5 Hz
 *   setup access point        slow blink        1 Hz
 *   connected                 solid on
 *   recording a clip          double blink      every second
 *   low battery               short blip        every 2 s
 *   photo saved               three quick flashes (once)
 *   power button held 3 s     dark              release = deep sleep
 *   power button held 10 s    very fast flicker release = factory reset
 *
 * With the LED disabled in settings only the two button gestures show.
 */
#ifndef LC_LED_H
#define LC_LED_H

#include <stdint.h>

typedef enum {
    LED_OFF = 0,
    LED_CONNECTING,
    LED_AP,
    LED_CONNECTED,
    LED_RECORDING,
    LED_LOW_BATTERY,
    LED_PHOTO,          /* one-shot; t is the time since the photo */
    LED_SLEEP_ARMED,
    LED_RESET_ARMED,
    LED_STATES
} led_state_t;

/* LED level (0/1) for `state` at time t_ms. Steady patterns repeat; LED_PHOTO
 * plays once and then reports 0. */
int led_level(led_state_t state, uint32_t t_ms);
/* Length of a one-shot pattern in ms (0 for repeating ones). */
uint32_t led_oneshot_ms(led_state_t state);

#endif
