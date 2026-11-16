"""whereis.py <guest address> [count]: which module and function an address is in, and its first instructions."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone
from nso import Nso

BASES = {'main': 0x7000000000, 'sdk': 0x7004000000, 'subsdk0': 0x7005000000}
addr = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 24
for name, base in BASES.items():
    m = Nso(os.path.join('extracted/exefs', name))
    if not base <= addr < base + m.size:
        continue
    best = None
    for s in m.syms:
        if s[2] and (s[1] & 0xF) == 2 and base + s[3] <= addr and (best is None or s[3] > best[3]):
            best = s
    print(f'{addr:#x} is in {name} +{addr - base:#x}')
    if best:
        print(f'  in function {best[0]} (starts {base + best[3]:#x}, +{addr - base - best[3]:#x})')
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    off = addr - base
    for i in md.disasm(bytes(m.image[off:off + count * 4]), addr):
        print(f'  {i.address:#x}: {i.mnemonic} {i.op_str}')
    break
else:
    print('not in main, sdk or subsdk0')
