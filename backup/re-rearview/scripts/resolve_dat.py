"""Annotate Ghidra C: DAT_xxxxxxxx -> literal value (and string, if it points at one)."""
import re, struct, sys
img = open(sys.argv[1], 'rb').read(); BASE = 0x10000; END = BASE + len(img)

def cstr(a):
    o = a - BASE
    if not 0 <= o < len(img): return None
    e = img.find(b'\0', o, o + 200)
    s = img[o:e] if e > o else b''
    if len(s) >= 3 and all(32 <= c < 127 or c in (9, 10, 13) for c in s):
        return s.decode().replace('\n', '\\n').replace('\r', '\\r')

def sub(m):
    a = int(m.group(1), 16)
    if not BASE <= a < END - 4: return m.group(0)
    v = struct.unpack_from('<I', img, a - BASE)[0]
    s = cstr(v)
    return f'{m.group(0)}/*0x{v:x}' + (f' "{s}"' if s else '') + '*/'

src = open(sys.argv[2]).read()
open(sys.argv[3], 'w').write(re.sub(r'DAT_([0-9a-f]{8})', sub, src))
