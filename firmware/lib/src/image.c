#include "lc/image.h"
#include "lc/hal.h"

#include <stdio.h>
#include <string.h>

/* Form key of each setting and the HAL control it drives. "night" drives
 * two: the IR LEDs, and saturation (black and white while it is on). */
static const struct {
    const char *key;
    hal_ctrl_t  ctrl;
} g_keys[] = {
    { "brightness", HAL_CTRL_BRIGHTNESS },
    { "contrast",   HAL_CTRL_CONTRAST },
    { "saturation", HAL_CTRL_SATURATION },
    { "ae",         HAL_CTRL_AE },
    { "ae_level",   HAL_CTRL_AE_LEVEL },
    { "exposure",   HAL_CTRL_EXPOSURE },
    { "gain",       HAL_CTRL_GAIN },
    { "awb",        HAL_CTRL_AWB },
    { "flicker",    HAL_CTRL_FLICKER },
    { "flip",       HAL_CTRL_FLIP },
    { "night",      HAL_CTRL_IR },
};
#define NKEYS (sizeof(g_keys) / sizeof(g_keys[0]))

static int ctrl_value(const lc_config_t *c, hal_ctrl_t ctrl)
{
    const lc_image_t *im = &c->image;

    switch (ctrl) {
    case HAL_CTRL_BRIGHTNESS: return im->brightness;
    case HAL_CTRL_CONTRAST:   return im->contrast;
    case HAL_CTRL_SATURATION: return im->night ? -4 : im->saturation;
    case HAL_CTRL_AE:         return im->ae;
    case HAL_CTRL_AE_LEVEL:   return im->ae_level;
    case HAL_CTRL_EXPOSURE:   return im->exposure;
    case HAL_CTRL_GAIN:       return im->gain;
    case HAL_CTRL_AWB:        return im->awb;
    case HAL_CTRL_FLICKER:    return im->flicker;
    case HAL_CTRL_FLIP:       return c->camera.flip;
    case HAL_CTRL_IR:         return im->night;
    default:                  return 0;
    }
}

static void apply(const lc_config_t *c, hal_ctrl_t ctrl)
{
    if (hal_camera_ctrls() & (1u << ctrl))
        hal_camera_set_ctrl(ctrl, ctrl_value(c, ctrl));
}

void image_apply_all(const lc_config_t *c)
{
    int i;

    /* Manual exposure and gain only take effect with auto exposure off,
     * so the HAL writes them when AE changes; the order doesn't matter. */
    for (i = 0; i < HAL_CTRL_COUNT; i++)
        apply(c, (hal_ctrl_t)i);
}

int image_set(lc_config_t *c, const char *key, const char *value)
{
    size_t i;

    for (i = 0; i < NKEYS; i++) {
        if (strcmp(key, g_keys[i].key))
            continue;
        if (config_set(c, key, value) != 0)
            return -1;
        apply(c, g_keys[i].ctrl);
        if (g_keys[i].ctrl == HAL_CTRL_IR)
            apply(c, HAL_CTRL_SATURATION);
        return 0;
    }
    return -1;
}

int image_supported_json(char *out, size_t cap)
{
    uint32_t ctrls = hal_camera_ctrls();
    size_t i;
    int n = snprintf(out, cap, "[");

    for (i = 0; i < NKEYS && n > 0 && (size_t)n < cap; i++)
        if (ctrls & (1u << g_keys[i].ctrl))
            n += snprintf(out + n, cap - (size_t)n, "%s\"%s\"", n > 1 ? "," : "",
                          g_keys[i].key);
    if (n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, "]");
    return n;
}
