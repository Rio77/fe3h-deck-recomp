"""The code patches in an Atmosphere-format cheat file (cheats.txt in the project folder, never committed).

A Switch cheat that writes an instruction into the game's code (opcode 0, 4 bytes, the main module) can't patch a
recompiled game at run time: the instruction is C by then. gen.py compiles each such instruction in as a variant
instead, beside the original, and the runtime's cheat VM (runtime/hle/cheatvm.c) picks it while the cheat is on.

code_variants(path, in_text) -> {offset from main's base: [instruction words]}
"""
import os
import re


def opcodes(words):
    """(type, words) per opcode of an Atmosphere cheat VM program; stops at a type it doesn't know the length of."""
    i = 0
    while i < len(words):
        w = words[i]
        t = w >> 28
        if t in (0, 1):
            n = 4 if ((w >> 24) & 0xF) == 8 else 3
        elif t == 2:
            n = 1
        elif t == 3:
            n = 2 if ((w >> 24) & 0xF) == 0 else 1  # (300R0000 VVVVVVVV begins a loop, 310R0000 ends it)
        elif t == 4:
            n = 3
        elif t == 5:
            n = 2
        elif t == 6:
            n = 3
        elif t == 7:
            n = 2
        elif t == 8:
            n = 1
        else:
            return
        yield t, words[i:i + n]
        i += n


def parse(path):
    """[(name, [words])] for each cheat in the file (master codes {name} included)."""
    cheats, cur = [], None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            m = re.match(r'^[\[{](.*)[\]}]$', line)
            if m:
                cur = (m.group(1).strip(), [])
                cheats.append(cur)
                continue
            if cur is None or line.startswith('#'):  # (# lines: the cheat's description)
                continue
            for tok in line.split():
                if re.fullmatch(r'[0-9A-Fa-f]{8}', tok):
                    cur[1].append(int(tok, 16))
    return cheats


def code_variants(path, in_text):
    out = {}
    if not path or not os.path.exists(path):
        return out
    for name, words in parse(path):
        for t, op in opcodes(words):
            w = op[0]
            width, mem, reg = (w >> 24) & 0xF, (w >> 20) & 0xF, (w >> 16) & 0xF
            if t != 0 or width != 4 or mem != 0 or reg != 0:  # (a register offset: not known before it runs)
                continue
            off = ((w & 0xFF) << 32) | op[1]
            if not in_text(off):
                continue
            vs = out.setdefault(off, [])
            if op[2] not in vs:
                vs.append(op[2])
    return out
