/* ap.txt / wifi.txt parsing: friendly keys, quoting, comments, defaults
 * for what a file leaves out, and errors that point at the line. */
#include <stdio.h>
#include <string.h>

#include "lc/hal.h"
#include "lc/sd_config.h"

/* config.c and sd_config.c need these; the parser uses none of them. */
void hal_get_mac(uint8_t mac[6]) { memset(mac, 0x12, 6); }
int  hal_kv_get(const char *k, void *b, size_t l) { return -1; }
int  hal_kv_set(const char *k, const void *b, size_t l) { return 0; }
int  hal_kv_erase_all(void) { return 0; }
int  hal_storage_poll(void) { return 0; }
hal_file_t *hal_file_open(const char *p, char m) { return NULL; }
int  hal_file_read(hal_file_t *f, void *b, size_t l) { return -1; }
int  hal_file_write(hal_file_t *f, const void *b, size_t l) { return -1; }
int  hal_file_close(hal_file_t *f) { return 0; }
int  hal_file_remove(const char *p) { return 0; }

static int fails;

static void expect_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want)) {
        printf("%s: got \"%s\", want \"%s\"\n", what, got, want);
        fails++;
    }
}

static void expect_int(const char *what, int got, int want)
{
    if (got != want) {
        printf("%s: got %d, want %d\n", what, got, want);
        fails++;
    }
}

static int parse(lc_config_t *c, const char *text, int ap, const char **err, int *line)
{
    char buf[512];

    snprintf(buf, sizeof(buf), "%s", text);
    return sd_config_parse(c, buf, ap, err, line);
}

int main(void)
{
    lc_config_t c;
    const char *err;
    int line;

    /* Station with DHCP; Windows line ends, BOM, comments, quoted spaces. */
    config_defaults(&c);
    strcpy(c.ip_addr, "10.0.0.9");
    c.ip_static = 1;
    expect_int("wifi ok", parse(&c, "\xEF\xBB\xBF# home\r\nssid = \"My Home \"\r\n"
                                    "password=secret pass # comment\r\n", 0, &err, &line), 0);
    expect_str("sta_ssid", c.sta_ssid, "My Home ");
    expect_str("sta_pass", c.sta_pass, "secret pass");
    expect_int("dhcp when no ip", c.ip_static, 0);

    /* Static address; mask defaults; other settings by API name. */
    expect_int("static ok", parse(&c, "ssid=Lab\nip=192.168.1.50\ngateway=192.168.1.1\n"
                                      "device_name=Porch\n", 0, &err, &line), 0);
    expect_int("ip_static", c.ip_static, 1);
    expect_str("ip_addr", c.ip_addr, "192.168.1.50");
    expect_str("ip_mask", c.ip_mask, "255.255.255.0");
    expect_str("ip_gw", c.ip_gw, "192.168.1.1");
    expect_str("device_name", c.device_name, "Porch");
    expect_str("open network", c.sta_pass, "");

    /* AP file: forgets the home network so the camera stays an AP. */
    expect_int("ap ok", parse(&c, "ssid=CamAP\npassword=12345678\nchannel=11\n", 1, &err, &line), 0);
    expect_str("ap_ssid", c.ap_ssid, "CamAP");
    expect_str("ap_pass", c.ap_pass, "12345678");
    expect_int("ap_channel", c.ap_channel, 11);
    expect_str("sta cleared", c.sta_ssid, "");

    /* Errors leave the config alone and name the line. */
    config_defaults(&c);
    expect_int("short ap pass", parse(&c, "ssid=X\npassword=short\n", 1, &err, &line), -1);
    expect_int("line", line, 2);
    expect_str("unchanged", c.ap_pass, "");
    expect_int("bad ip", parse(&c, "ssid=X\n\nip=300.1.1.1\n", 0, &err, &line), -1);
    expect_int("bad ip line", line, 3);
    expect_int("no '='", parse(&c, "ssid X\n", 0, &err, &line), -1);
    expect_int("unknown key", parse(&c, "ssid=X\ncolour=red\n", 0, &err, &line), -1);
    expect_int("no ssid", parse(&c, "password=abcdefgh\n", 0, &err, &line), -1);
    expect_int("whole-file error", line, 0);
    expect_str("sta untouched", c.sta_ssid, "");

    if (fails)
        printf("%d failures\n", fails);
    else
        printf("sd_config: all passed\n");
    return fails != 0;
}
