#!/usr/bin/env python3
"""Embeds web/index.html into a C source file as web_ui_html[]."""
import sys

src, out = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
lines = []
for i in range(0, len(data), 16):
    lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
with open(out, "w") as f:
    f.write("/* Generated from %s by tools/embed_web.py. Do not edit. */\n" % src.split("/")[-1])
    f.write('#include "lc/web_ui.h"\n\n')
    f.write("const char web_ui_html[] = {\n%s\n};\n" % "\n".join(lines))
    f.write("const unsigned int web_ui_html_len = %d;\n" % len(data))
