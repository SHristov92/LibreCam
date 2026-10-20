#include "lc/led.h"

#include <string.h>

#define SLOT_MS 50

/* One character per 50 ms slot: '#' on, '.' off. Repeating unless noted. */
static const char *const g_patterns[LED_STATES] = {
    [LED_OFF]         = ".",
    [LED_CONNECTING]  = "##..",                                       /* 5 Hz */
    [LED_AP]          = "##########..........",                       /* 1 Hz */
    [LED_CONNECTED]   = "#",
    [LED_RECORDING]   = "##..##..............",                       /* double, 1 s */
    [LED_LOW_BATTERY] = "##......................................",   /* blip, 2 s */
    [LED_PHOTO]       = "..##..##..##..",                             /* once */
    [LED_SLEEP_ARMED] = ".",
    [LED_RESET_ARMED] = "#.",                                         /* 10 Hz */
};

uint32_t led_oneshot_ms(led_state_t state)
{
    return state == LED_PHOTO ? (uint32_t)strlen(g_patterns[LED_PHOTO]) * SLOT_MS : 0;
}

int led_level(led_state_t state, uint32_t t_ms)
{
    const char *p;
    uint32_t n, slot;

    if ((unsigned)state >= LED_STATES)
        return 0;
    p = g_patterns[state];
    n = (uint32_t)strlen(p);
    slot = t_ms / SLOT_MS;
    if (led_oneshot_ms(state)) {
        if (slot >= n)
            return 0;
    } else {
        slot %= n;
    }
    return p[slot] == '#';
}
