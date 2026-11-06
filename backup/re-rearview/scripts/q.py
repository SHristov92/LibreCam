"""Query app_r.c: `fn ADDR..` prints functions, `callers ADDR..` lists call sites, `str REGEX` lists functions using matching strings."""
import re, sys
src = open(__file__.rsplit('/', 2)[0] + '/app_r.c').read()
funcs = {}
for m in re.finditer(r'^// ==== ([0-9a-f]{8}) (\S+)\n(.*?)(?=^// ==== |\Z)', src, re.M | re.S):
    funcs[m.group(1)] = (m.group(2), m.group(3))
cmd, args = sys.argv[1], sys.argv[2:]
if cmd == 'fn':
    for a in args:
        n, body = funcs[a.lower().zfill(8)]
        print(f'// ==== {a} {n}\n{body}')
elif cmd == 'callers':
    for a in args:
        pat = re.compile(r'FUN_%s\(([^;]*)\)' % a.lower().zfill(8))
        for f, (n, body) in funcs.items():
            for m in pat.finditer(body):
                print(f'{a}<-{f}\t{m.group(1)[:160]}')
elif cmd == 'str':
    pat = re.compile(args[0], re.I)
    for f, (n, body) in funcs.items():
        hits = sorted(set(s for s in re.findall(r'/\*0x[0-9a-f]+ "([^"]*)"\*/', body) if pat.search(s)))
        if hits: print(f, '|', ' || '.join(h[:70] for h in hits[:6]))
