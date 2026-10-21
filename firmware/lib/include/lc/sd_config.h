/*
 * Wi-Fi setup from the SD card. At boot, before Wi-Fi starts, the camera
 * looks in the card's root for:
 *
 *   ap.txt    the setup access point: ssid, password (empty = open,
 *             otherwise 8..63 characters), channel. The camera then runs as
 *             an access point: the home network is forgotten.
 *   wifi.txt  the home network: ssid, password (empty = open), and for a
 *             static address ip, mask, gateway, dns (no ip = DHCP).
 *
 * One "key = value" per line; '#' starts a comment; a value in double quotes
 * keeps its spaces. Any other setting the web UI saves can be given by its
 * API name too (device_name, admin_pass, ...). Both files may be present:
 * ap.txt is applied first, so wifi.txt wins the mode. A file that applies is
 * deleted, since it holds a password; one that doesn't is left in place
 * next to <name>-error.txt saying why.
 */
#ifndef LC_SD_CONFIG_H
#define LC_SD_CONFIG_H

#include "lc/config.h"

#define SD_CONFIG_AP   "/ap.txt"
#define SD_CONFIG_WIFI "/wifi.txt"

/* Applies the text of ap.txt (ap = 1) or wifi.txt (ap = 0) to *c.
 * Returns 0, or -1 with *err (and *line, 1-based, 0 = whole file) set. */
int  sd_config_parse(lc_config_t *c, char *text, int ap, const char **err, int *line);

/* Mounts the card if there is one and applies the files to g_config,
 * saving it. Returns the number of files applied. */
int  sd_config_apply(void);

#endif
