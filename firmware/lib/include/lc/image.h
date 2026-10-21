/*
 * Image settings (brightness, exposure, white balance, night mode, ...):
 * maps the stored lc_image_t onto the HAL's camera controls. Unlike the
 * video settings they apply while streaming, without a reboot.
 */
#ifndef LC_IMAGE_H
#define LC_IMAGE_H

#include <stddef.h>
#include "lc/config.h"

/* Applies every image setting of c; called after the camera starts. */
void image_apply_all(const lc_config_t *c);

/* Sets one image setting (a form key such as "contrast" or "flip") in c
 * and applies it. Returns 0, or -1 for an unknown key or invalid value. */
int  image_set(lc_config_t *c, const char *key, const char *value);

/* Writes the keys the camera supports as a JSON array of strings, e.g.
 * ["brightness","contrast"]. Returns the length written. */
int  image_supported_json(char *out, size_t cap);

#endif
