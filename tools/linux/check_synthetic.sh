#!/bin/bash
# No game files. Run in tools/linux/Containerfile after build_deps.sh.
set -euo pipefail
export PYTHONDONTWRITEBYTECODE=1
cd "$(dirname "$0")/../.."
DEPS=${DEPS:-build/linux/x86_64/deps}
TEST_TMP=$(mktemp -d)
trap 'rm -rf -- "$TEST_TMP"' EXIT
export LD_LIBRARY_PATH="$PWD/$DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
python3 -m unittest discover -s tools/linux -p 'test_*.py' -v
python3 tools/test_quick_taps_opt5.py
python3 tools/test_fence_sequence.py
python3 tools/test_pad.py
clang -std=gnu11 -O1 -ffunction-sections -fdata-sections -Iruntime -Wl,--gc-sections \
    tools/test_settings_opt5.c runtime/settings.c -o "$TEST_TMP/settings"
"$TEST_TMP/settings"
make test-mods
make test-nfp
clang -std=gnu11 -O1 -Iruntime tools/test_romfs_hashes.c runtime/romfs_build.c runtime/sha256.c -o "$TEST_TMP/romfs_hashes"
python3 - "$TEST_TMP/hash-fixture" <<'PYTEST'
from pathlib import Path
import sys
p=Path(sys.argv[1]);(p/'patch4').mkdir(parents=True)
for name,size in {'DATA0.bin':997120,'DATA1.bin':6936719456,'patch4/INFO0.bin':348480,'patch4/INFO1.bin':1137360,'patch4/INFO2.bin':16}.items():
    with (p/name).open('wb') as f:f.truncate(size)
for i in range(3696-5):(p/str(i)).touch()
PYTEST
"$TEST_TMP/romfs_hashes" "$TEST_TMP/hash-fixture" warning
clang -Iruntime tools/test_text_utf8.c -o "$TEST_TMP/text_utf8"
"$TEST_TMP/text_utf8"
clang -D_GNU_SOURCE -O1 -ffunction-sections -fdata-sections -Iruntime \
    -Dromfs_dir_bytes=old_romfs_dir_bytes -Dromfs_copy_tree=old_romfs_copy_tree \
    -c tools/romfs_old.c -o "$TEST_TMP/romfs_old.o"
clang -D_GNU_SOURCE -O1 -ffunction-sections -fdata-sections -Iruntime -Wl,--gc-sections \
    tools/test_romfs_opt4.c runtime/romfs_build.c runtime/sha256.c runtime/mods.c "$TEST_TMP/romfs_old.o" -pthread -o "$TEST_TMP/romfs"
python3 - "$TEST_TMP/romfs-fixture" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);d=p/'romfs';(d/'sub').mkdir(parents=True)
for i in range(1004):
    f=(d/'sub' if i%3 else d)/str(i)
    f.write_bytes(bytes((j*17+i)%256 for j in range(i%193)))
(d/'sub/empty').mkdir()
PY
"$TEST_TMP/romfs" "$TEST_TMP/romfs-fixture"
for t in native_owners movie_buffer_owners am_storage_owners; do
    cc -std=gnu11 -O1 -g -fsanitize=address,undefined "tools/test_$t.c" -pthread -o "$TEST_TMP/$t"
    "$TEST_TMP/$t"
done
# Clang understands the lifted SIMD builtins. GCC supplies the sanitizer runtimes
# when the build image has no compiler-rt sanitizer package installed.
for t in boot_native_cleanup save_failures_opt5; do
    clang -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize=function \
        -ffunction-sections -fdata-sections -Wno-format -I"$DEPS/include" -Iruntime \
        -c "tools/test_$t.c" -o "$TEST_TMP/$t.o"
    cc -fsanitize=address,undefined -Wl,--gc-sections "$TEST_TMP/$t.o" -pthread -ldl -lm -o "$TEST_TMP/$t"
    "$TEST_TMP/$t"
done
# This test must own the signal stack; ASan installs its own stack before each
# worker starts, so its default instrumentation would test the borrowed path.
clang -std=gnu11 -O1 -ffunction-sections -fdata-sections -Wno-format -I"$DEPS/include" -Iruntime \
    tools/test_crash_stack_linux.c -Wl,--gc-sections -pthread -ldl -o "$TEST_TMP/crash_stack_linux"
"$TEST_TMP/crash_stack_linux"
for t in dom sim build barrier; do
    clang -std=c11 -O1 -Iruntime/nvn/maxwell "tools/test_scf_$t.c" runtime/nvn/maxwell/scf_*.c -o "$TEST_TMP/scf_$t"
    "$TEST_TMP/scf_$t" 5000
done
clang -std=gnu11 -O1 -ffunction-sections -fdata-sections -I"$DEPS/include" -Iruntime \
    tools/test_shader_compiler_lifetime.c -Wl,--gc-sections -L"$DEPS/lib" -lshaderc_shared -pthread -o "$TEST_TMP/shader_lifetime"
"$TEST_TMP/shader_lifetime"
clang++ -std=c++17 -pthread -Iruntime -Ithird_party/imgui -I"$DEPS/include" \
    tools/test_text_clipboard.cpp third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
    third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_tables.cpp \
    -L"$DEPS/lib" -lSDL3 -o "$TEST_TMP/text_clipboard"
SDL_VIDEODRIVER=dummy timeout 20 "$TEST_TMP/text_clipboard"
echo 'PASS synthetic Linux checks'
