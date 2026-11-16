"""fnsrc.py <generated dir> <address> [<address> ...]

Prints the recompiled C of the guest function that contains each address (the f_<start> with the
greatest start <= the address), e.g.  python tools/fnsrc.py generated 7000aa5794 70009b69c0 > fn.txt"""
import os
import re
import sys

gdir = sys.argv[1]
args = sys.argv[2:]
grep = None  # --grep REGEX LO HI: also every function in [LO, HI) whose code matches REGEX
if '--grep' in args:
    i = args.index('--grep')
    grep = (re.compile(args[i + 1]), int(args[i + 2], 16), int(args[i + 3], 16))
    del args[i:i + 4]
want = [int(a, 16) for a in args]
defn = re.compile(r'^[^\n;]*?\bf_([0-9a-f]{6,16})\s*\(\s*Cpu\s*\*[^)]*\)\s*\{', re.M)
starts = []  # (start, file, offset)
texts = {}


def read(path):
    if path not in texts:
        with open(path, encoding='utf-8', errors='replace') as f:
            texts[path] = f.read()
    return texts[path]

nfiles = 0
for root, _, names in os.walk(gdir):
    for name in sorted(names):
        if not name.endswith('.c'):
            continue
        nfiles += 1
        path = os.path.join(root, name)
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()  # (not cached: all of it would be about a gigabyte)
        for m in defn.finditer(text):
            starts.append((int(m.group(1), 16), path, m.start()))
starts.sort()
print(f'// {nfiles} .c files, {len(starts)} function definitions')
if not starts:
    for root, _, names in os.walk(gdir):
        for name in names:
            if name.endswith('.c'):
                with open(os.path.join(root, name), encoding='utf-8', errors='replace') as f:
                    print('// sample of', name, ':')
                    print(''.join(f.readlines()[:40]))
                break
        break
if grep:
    rx, lo, hi = grep
    for start, path, off in starts:
        if not lo <= start < hi:
            continue
        text = read(path)
        nxt = defn.search(text, off + 1)
        body = text[off:nxt.start() if nxt else len(text)]
        if rx.search(body) and start not in want:
            want.append(start)
for a in want:
    best = None
    for s in starts:
        if s[0] <= a:
            best = s
        else:
            break
    if not best:
        print(f'// {a:x}: no function found\n')
        continue
    start, path, off = best
    text = read(path)
    nxt = defn.search(text, off + 1)
    end = nxt.start() if nxt else -1
    body = text[off:end if end > 0 else len(text)]
    print(f'// ===== {a:x} is in f_{start:x} ({os.path.basename(path)}) =====')
    print(body[:200000])
    print()
