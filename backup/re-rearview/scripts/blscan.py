"""blscan.py TARGET.. : list Thumb BL call sites to each target with the r0/r1/r2 immediates set just before."""
import struct, sys
b = open(__file__.rsplit('/', 2)[0] + '/app_0x10000.bin', 'rb').read(); BASE = 0x10000
tg = {int(a, 16) for a in sys.argv[1:]}
def bl_at(i):
    h1, h2 = struct.unpack_from('<HH', b, i)
    if h1 & 0xF800 == 0xF000 and h2 & 0xF800 == 0xF800:
        off = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
        if off & 0x400000: off -= 0x800000
        return BASE + i + 4 + off
for i in range(0, len(b) - 4, 2):
    t = bl_at(i)
    if t in tg:
        regs = {}
        for j in range(i - 2, max(i - 24, 0), -2):
            h = struct.unpack_from('<H', b, j)[0]
            if bl_at(j - 2) is not None: break
            if h & 0xF800 == 0x2000:  # movs rd,#imm
                rd = (h >> 8) & 7
                regs.setdefault(rd, h & 0xFF)
        print(f'{t:x} <- {BASE+i:x}  ' + ' '.join(f'r{r}={v}' for r, v in sorted(regs.items()) if r < 3))
