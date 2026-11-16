"""Which module provides what: sizes, linked libraries, and where main's imports come from."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import Nso

exefs = sys.argv[1] if len(sys.argv) > 1 else 'extracted/exefs'
names = [n for n in ('main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3') if os.path.exists(os.path.join(exefs, n))]
mods = {n: Nso(os.path.join(exefs, n)) for n in names}
defined = {}
for n, m in mods.items():
    d = {s[0] for s in m.syms if s[2] and s[0]}
    u = {s[0] for s in m.syms if not s[2] and s[0]}
    defined[n] = d
    print(f'{n:8} size {m.size:#x}  defines {len(d):6}  imports {len(u):5}  needs {m.needed}')
print()
for n in ('subsdk0', 'subsdk1'):
    if n in defined:
        sample = sorted(s for s in defined[n] if not s.startswith('_Z'))[:12] or sorted(defined[n])[:12]
        print(f'{n} sample symbols: {", ".join(sample)}')
print()
for n, m in mods.items():
    if n == 'sdk':
        continue
    imports = {s[0] for s in m.syms if not s[2] and s[0]}
    by = {}
    for s in sorted(imports):
        src = next((k for k in names if k != n and s in defined[k]), None)
        by.setdefault(src, []).append(s)
    print(f'{n} imports from: ' + ', '.join(f'{k or "NOBODY"} {len(v)}' for k, v in by.items()))
    for k, v in by.items():
        if k not in ('sdk', 'main'):
            print(f'   from {k or "NOBODY"}: {", ".join(v[:10])}{" ..." if len(v) > 10 else ""}')
