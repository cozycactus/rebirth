#!/bin/sh
# Checks the built simulator (sim/web/core.js) the way the page will use it:
# for every score the page offers on this firmware, the device check and,
# unless `quick`, the artist check (every song in every arrangement).
#   sim/check.sh          - all of it, seconds per score
#   sim/check.sh quick    - the device check only
# Which scores fit is read from the core, as the page does (app.js, initKeyboard):
# the base score before the last-note rule, then fw2, keys with MIDI input,
# pads with the pad keyboard.
set -e
cd "$(dirname "$0")/.."
MODE=${1:-full}

node sim/test/sh101_check.js
node sim/test/sh101_rom_check.js

SCORES=$(node -e '
  require("./sim/web/core.js")().then(core => {
    const f = core._sim_features ? core._sim_features() : 0;
    const last = !!(f & 1), out = [];
    if (!last) out.push("base");
    if (last) out.push("fw2");
    if (last && (f & 2)) out.push("keys");
    if (last && (f & 4)) out.push("pads");
    console.log(out.join(" "));
  });')
echo "scores this firmware takes: $SCORES"

for score in $SCORES; do
    case $score in
        base) VARS="" ;;
        fw2) VARS="FW2=1" ;;
        keys) VARS="KEYS=1" ;;
        pads) VARS="PADS=1" ;;
    esac
    echo "── $score: device check"
    env $VARS node sim/test/device_check.js
    [ "$MODE" = quick ] && continue
    echo "── $score: artist check"
    LOG=sim/build/artist-$score.log
    mkdir -p sim/build
    if env $VARS node sim/test/artist_check.js > "$LOG" 2>&1; then
        tail -1 "$LOG"
    else
        grep -E '^FAIL|FAILED|Error' "$LOG" | head -40
        echo "artist check failed for score $score; the whole run is in $LOG" >&2
        exit 1
    fi
done
