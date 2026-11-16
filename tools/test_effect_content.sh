#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
B=${B:-build/linux/x86_64}
DEPS=${DEPS:-build/linux/x86_64/deps}
CC=${CC:-clang}
CXX=${CXX:-clang++}
mkdir -p build
"$CC" -O1 -g -ffunction-sections -fdata-sections -Iruntime -I"$DEPS/include" -Ithird_party \
    tools/test_effect_content.c -Wl,--gc-sections -lm -o build/test_effect_content
build/test_effect_content
build/test_effect_content legacy
"$CC" -O1 -g -ffunction-sections -fdata-sections -Iruntime -I"$DEPS/include" -Ithird_party \
    -c tools/test_effect_content_gpu.c -o build/test_effect_content_gpu.o
"$CXX" build/test_effect_content_gpu.o "$B/rt/nvn/vulkan/vma.o" -Wl,--gc-sections \
    -lvulkan -lm -ldl -lpthread -o build/test_effect_content_gpu
# Compile in the build container with GPU_RUN=0; execute the resulting GPU test
# on the Deck host (without enabling implicit frame generation).
if [ "${GPU_RUN:-1}" = 1 ]; then
    DISABLE_LSFGVK=1 build/test_effect_content_gpu
fi
