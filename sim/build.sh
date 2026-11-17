#!/bin/sh
# Builds the simulator.
#   sim/build.sh native   - native core + smoke test (needs only a C compiler)
#   sim/build.sh web      - WebAssembly core for sim/web (needs sim/emsdk)
#   sim/build.sh          - both
set -e
cd "$(dirname "$0")/.."

# Every firmware source except main.c (compiled inside sim_main.c) and
# rgb_leds.c (PIO + DMA; sim/core/sim_rgb_leds.c stands in).
FIRMWARE=$(ls src/*.c | grep -v -e '^src/main\.c$' -e '^src/rgb_leds\.c$')
CORE="sim/core/hal.c sim/core/sim_main.c sim/core/sim_rgb_leds.c"
INCLUDES="-Iinclude -Itest/fakes -Isim/core"
mkdir -p sim/build

native() {
    ${CC:-cc} -std=c11 -O1 -g -Wall -Wno-unused-function $INCLUDES \
        $FIRMWARE $CORE sim/test/smoke.c -o sim/build/smoke
    sim/build/smoke
}

web() {
    if [ ! -x sim/emsdk/upstream/emscripten/emcc ]; then
        echo "sim/emsdk is missing. Install it with:" >&2
        echo "  git clone --depth 1 https://github.com/emscripten-core/emsdk.git sim/emsdk" >&2
        echo "  sim/emsdk/emsdk install latest && sim/emsdk/emsdk activate latest" >&2
        exit 1
    fi
    . sim/emsdk/emsdk_env.sh >/dev/null 2>&1
    EXPORTS=$(grep -o 'sim_[a-z0-9_]*(' sim/core/sim_api.h | sort -u | sed 's/(//; s/^/_/' | paste -sd, -)
    emcc -std=c11 -O2 $INCLUDES $FIRMWARE $CORE \
        -sEXPORTED_FUNCTIONS="$EXPORTS,_malloc,_free" \
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU16,HEAPU32,HEAPF64,UTF8ToString \
        -sMODULARIZE=1 -sEXPORT_NAME=RebirthCore -sSINGLE_FILE=1 \
        -sENVIRONMENT=web,node -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=33554432 \
        -o sim/web/core.js
    echo "built sim/web/core.js"
}

case "${1:-all}" in
    native) native ;;
    web) web ;;
    all) native; web ;;
    *) echo "usage: $0 [native|web]" >&2; exit 2 ;;
esac
