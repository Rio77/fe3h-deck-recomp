from pathlib import Path
import subprocess,tempfile
s=Path('runtime/hle/input.c').read_text()
note=s[s.index('static void note_press('):s.index('// Keep a quick tap')]
read=s[s.index('static uint64_t read_taps('):s.index('// Forget held keys')]
prefix="#include <stdint.h>\n#include <stdatomic.h>\n#include <assert.h>\n#include <stdio.h>\nstatic _Atomic uint64_t press_unseen,tap_buttons,reset_serial;\n"
main=r'''
int main(void){
 note_press(1,0);note_press(0,1);assert(read_taps(20)==1);assert(read_taps(20)==1);assert(!read_taps(21));
 note_press(2,0);atomic_fetch_and(&press_unseen,~2ull);note_press(0,2);assert(!read_taps(22));
 note_press(4,0);note_press(0,4);assert(read_taps(23)==4);
 note_press(8,0);note_press(0,8);assert(read_taps(23)==12);assert(!read_taps(24));
 note_press(16,0);note_press(0,16);assert(read_taps(9999)==16);
 atomic_fetch_add(&reset_serial,1);tap_buttons=press_unseen=0;assert(!read_taps(0));
 note_press(32,0);note_press(0,32);assert(read_taps(1)==32);
 puts("PASS quick taps: batched down/up, held button already polled, simultaneous presses, frame hold, reset/frame rewind");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(prefix+note+read+main);exe=Path(d)/'test'
 subprocess.run(['clang','-std=c11',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
