/* LED patterns: each state must be distinguishable and timed as documented. */
#include <stdio.h>

#include "lc/led.h"

static int fails;

static void expect(const char *what, int got, int want)
{
    if (got != want) {
        printf("%s: got %d, want %d\n", what, got, want);
        fails++;
    }
}

/* Fraction of time on over one 2 s window, in percent. */
static int duty(led_state_t s)
{
    int on = 0, t;
    for (t = 0; t < 2000; t += 10)
        on += led_level(s, (uint32_t)t);
    return on / 2;
}

/* Number of off->on edges over 2 s. */
static int pulses(led_state_t s)
{
    int n = 0, prev = led_level(s, 1990), t;
    for (t = 0; t < 2000; t += 10) {
        int v = led_level(s, (uint32_t)t);
        n += v && !prev;
        prev = v;
    }
    return n;
}

int main(void)
{
    expect("connected duty", duty(LED_CONNECTED), 100);
    expect("off duty", duty(LED_OFF), 0);
    expect("sleep-armed duty", duty(LED_SLEEP_ARMED), 0);
    expect("connecting pulses/2s", pulses(LED_CONNECTING), 10);   /* 5 Hz */
    expect("ap pulses/2s", pulses(LED_AP), 2);                    /* 1 Hz */
    expect("recording pulses/2s", pulses(LED_RECORDING), 4);      /* double per second */
    expect("low battery pulses/2s", pulses(LED_LOW_BATTERY), 1);
    expect("reset-armed pulses/2s", pulses(LED_RESET_ARMED), 20); /* 10 Hz */
    expect("ap half on", duty(LED_AP), 50);
    /* Photo flash: three flashes, then dark for good. */
    {
        int n = 0, prev = 0, t;
        for (t = 0; t < 3000; t += 10) {
            int v = led_level(LED_PHOTO, (uint32_t)t);
            n += v && !prev;
            prev = v;
        }
        expect("photo flashes", n, 3);
        expect("photo after end", led_level(LED_PHOTO, led_oneshot_ms(LED_PHOTO) + 5), 0);
    }
    printf("%s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
