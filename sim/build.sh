#!/bin/sh
# Builds the simulator.
#   sim/build.sh native   - native core + smoke test (needs only a C compiler)
#   sim/build.sh web      - WebAssembly cores for sim/web (needs sim/emsdk), and
#                           sim/web/version.js, which names the commit it was built from
#   sim/build.sh          - both
# Neither output is kept in git: CI builds them for the public page.
set -e
cd "$(dirname "$0")/.."

# Every firmware source except main.c (compiled inside sim_main.c) and
# rgb_leds.c (PIO + DMA; sim/core/sim_rgb_leds.c stands in).
FIRMWARE=$(ls src/*.c | grep -v -e '^src/main\.c$' -e '^src/rgb_leds\.c$')
CORE="sim/core/hal.c sim/core/sim_main.c sim/core/sim_rgb_leds.c"
INCLUDES="-Iinclude -Itest/fakes -Isim/core"

# What the firmware being built can do, for the page to pick a fitting score:
# 1 new steps start at the last note and clips copy, 2 it reads its MIDI input,
# 4 the pads are a keyboard while a step is held, 8 MUTE waits for the launch
# point, 16 it follows an external clock.
FEATURES=0
grep -q 'last_note' src/ui.c && FEATURES=$((FEATURES | 1))
[ -f src/midi_in.c ] && FEATURES=$((FEATURES | 2))
grep -q 'pad_key_note' include/led_frame.h && FEATURES=$((FEATURES | 4))
grep -q 'transport_toggle_mutes' include/sequencer.h && FEATURES=$((FEATURES | 8))
[ -f src/clock_in.c ] && FEATURES=$((FEATURES | 16))
INCLUDES="$INCLUDES -DSIM_FEATURES=$FEATURES"
echo "firmware features: $FEATURES (1 last note and clip copy, 2 MIDI input, 4 pad keyboard, 8 MUTE waits for the launch point, 16 external clock)"
mkdir -p sim/build

# The Emscripten release the public page is built with (.github/workflows/ci.yml).
EMSDK_VERSION=7.0.0

native() {
    ${CC:-cc} -std=c11 -O1 -g -Wall -Wno-unused-function $INCLUDES \
        $FIRMWARE $CORE sim/test/smoke.c -o sim/build/smoke
    sim/build/smoke
}

web() {
    if [ ! -x sim/emsdk/upstream/emscripten/emcc ]; then
        echo "sim/emsdk is missing. Install it with:" >&2
        echo "  git clone --depth 1 https://github.com/emscripten-core/emsdk.git sim/emsdk" >&2
        echo "  sim/emsdk/emsdk install $EMSDK_VERSION && sim/emsdk/emsdk activate $EMSDK_VERSION" >&2
        exit 1
    fi
    # emsdk_env.sh cannot find its own folder when sourced by a plain POSIX sh
    # (dash, the /bin/sh of Linux), so take the environment from emsdk itself.
    eval "$(EMSDK_BASH=1 sim/emsdk/emsdk construct_env 2>/dev/null)"
    command -v emcc >/dev/null || { echo "emcc is not on PATH after activating sim/emsdk" >&2; exit 1; }
    EXPORTS=$(grep -o 'sim_[a-z0-9_]*(' sim/core/sim_api.h | sort -u | sed 's/(//; s/^/_/' | paste -sd, -)
    emcc -std=c11 -O2 $INCLUDES $FIRMWARE $CORE \
        -sEXPORTED_FUNCTIONS="$EXPORTS,_malloc,_free" \
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU16,HEAPU32,HEAPF64,UTF8ToString \
        -sMODULARIZE=1 -sEXPORT_NAME=RebirthCore -sSINGLE_FILE=1 \
        -sENVIRONMENT=web,node -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=33554432 \
        -o sim/web/core.js
    echo "built sim/web/core.js"
    SH101_EXPORTS=$(grep -o 'sh101_[a-z0-9_]*(' sim/sh101/board.h | sort -u | sed 's/(//; s/^/_/' | paste -sd, -)
    emcc -std=c11 -O2 sim/sh101/board.c \
        -sEXPORTED_FUNCTIONS="$SH101_EXPORTS" -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32 \
        -sMODULARIZE=1 -sEXPORT_NAME=SH101Core -sSINGLE_FILE=1 \
        -sENVIRONMENT=web,node -o sim/web/sh101_cpu.js
    echo "built sim/web/sh101_cpu.js"
    # What the page shows as its firmware: the commit, and whether the
    # firmware or the simulator had changes not yet committed.
    COMMIT=$(git rev-parse HEAD 2>/dev/null || echo unknown)
    DIRTY=false
    [ -n "$(git status --porcelain --untracked-files=no -- src include sim test/fakes 2>/dev/null)" ] && DIRTY=true
    SIM_COMMIT="$COMMIT" SIM_DIRTY="$DIRTY" SIM_FEATURES="$FEATURES" \
    SIM_DATE="$(git log -1 --format=%cI 2>/dev/null)" SIM_SUBJECT="$(git log -1 --format=%s 2>/dev/null)" \
    SIM_EMCC="$(emcc --version | head -1 | sed 's/.*) //; s/ .*//')" node -e '
      const e = process.env, b = { commit: e.SIM_COMMIT, short: e.SIM_COMMIT.slice(0, 7), date: e.SIM_DATE,
        subject: e.SIM_SUBJECT, dirty: e.SIM_DIRTY === "true", features: +e.SIM_FEATURES, emscripten: e.SIM_EMCC };
      require("fs").writeFileSync("sim/web/version.js", "window.SIM_BUILD = " + JSON.stringify(b) + ";\n");'
    echo "built sim/web/version.js ($(echo "$COMMIT" | cut -c1-7)$( [ "$DIRTY" = true ] && echo ', with uncommitted changes'))"
}

case "${1:-all}" in
    native) native ;;
    web) web ;;
    all) native; web ;;
    *) echo "usage: $0 [native|web]" >&2; exit 2 ;;
esac
