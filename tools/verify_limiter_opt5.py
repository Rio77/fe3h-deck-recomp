from pathlib import Path
import sys,hashlib
try:
    import lz4.block
except ModuleNotFoundError:
    # The build image has liblz4 but intentionally no Python package manager.
    import ctypes, ctypes.util, types
    lib=ctypes.CDLL(ctypes.util.find_library('lz4'))
    lib.LZ4_decompress_safe.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_int,ctypes.c_int]
    lib.LZ4_decompress_safe.restype=ctypes.c_int
    def decompress(data,uncompressed_size):
        out=ctypes.create_string_buffer(uncompressed_size)
        n=lib.LZ4_decompress_safe(data,out,len(data),uncompressed_size)
        assert n==uncompressed_size,(n,uncompressed_size)
        return out.raw
    package=types.ModuleType('lz4');block=types.ModuleType('lz4.block')
    block.decompress=decompress;package.block=block
    sys.modules['lz4']=package;sys.modules['lz4.block']=block
from nso import Nso
root=Path(sys.argv[1]);symbol='_ZN2nn2os11YieldThreadEv'
assert hashlib.sha256((root/'main').read_bytes()).hexdigest()=='d9a2c29096eea99b536217a1116f937c17787e44a8236205cabbf0466f593600'
main=Nso(str(root/'main'));sdk=Nso(str(root/'sdk'))
entry=next(s for s in sdk.exports() if s[0]==symbol)
assert entry[3]+0x7004000000==0x700431bb30,hex(entry[3])
pc=0x5be4c0;branch=main.u32(pc);assert branch&0xfc000000==0x94000000,hex(branch)
imm=branch&0x3ffffff;imm=imm-(1<<26) if imm&(1<<25) else imm;target=pc+imm*4
if target+0x7000000000!=0x700431bb30:
    adrp=main.u32(target);ldr=main.u32(target+4)
    assert adrp&0x9f000000==0x90000000,hex(adrp)
    page_imm=((adrp>>29)&3)|(((adrp>>5)&0x7ffff)<<2)
    if page_imm&(1<<20):page_imm-=1<<21
    got=(target&~4095)+(page_imm<<12)+(((ldr>>10)&0xfff)<<3)
    assert any(off==got and main.syms[index][0]==symbol for off,kind,index,add in main.relocs),hex(got)
print('PASS limiter: original 1.2.0 NSO hash, return address 0x70005be4c4, YieldThread import and SDK 0x700431bb30')
