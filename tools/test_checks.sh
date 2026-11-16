#!/bin/bash
set -euo pipefail
for t in dom sim build barrier; do
 clang -std=c11 -O1 -Iruntime/nvn/maxwell tools/test_scf_$t.c runtime/nvn/maxwell/scf_*.c -o /tmp/test_scf_$t
 /tmp/test_scf_$t 5000
done
clang -D_GNU_SOURCE -O1 -ffunction-sections -fdata-sections -Iruntime -Dromfs_dir_bytes=old_romfs_dir_bytes -Dromfs_copy_tree=old_romfs_copy_tree -c tools/romfs_old.c -o /tmp/romfs_old.o
clang -D_GNU_SOURCE -O1 -ffunction-sections -fdata-sections -Iruntime -Wl,--gc-sections tools/test_romfs_opt4.c runtime/romfs_build.c runtime/mods.c /tmp/romfs_old.o -pthread -o /tmp/test_romfs_opt4
python3 - <<'PY'
from pathlib import Path
import tempfile
p=Path(tempfile.mkdtemp(prefix='romfs-opt4-'))
d=p/'romfs';d.mkdir();(d/'sub').mkdir()
for i in range(1004):
 f=(d/'sub' if i%3 else d)/str(i)
 f.write_bytes(bytes((j*17+i)%256 for j in range(i%193)))
(d/'sub/empty').mkdir()
(Path('/tmp/romfs-opt4-fixture')).write_text(str(p))
PY
/tmp/test_romfs_opt4 "$(cat /tmp/romfs-opt4-fixture)"
