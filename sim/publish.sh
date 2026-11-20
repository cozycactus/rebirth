#!/bin/bash
# Puts the simulator on the public page, cozycactus.github.io/rebirth/.
#
#   sim/publish.sh package OUT        after sim/build.sh web: OUT/page holds the
#                                     page as served, OUT/source the GPL source
#                                     of its WebAssembly, from this checkout
#   sim/publish.sh apply OUT SITE     lays OUT over a checkout of the site repo
#                                     and stages the change; touches only the
#                                     page's own files and rebirth/source/
#
# CI runs `package` on master and the publish workflow runs `apply` (both in
# .github/workflows). The page's other files (the manual PDF, the site's own
# index.html) are never written or removed here.
set -euo pipefail

# The files of the page this script owns in the site's rebirth/ folder.
PAGE_FILES="index.html app.js artist.js device.js rig.js acid.js dsp.js sh101.js sh101_cpu.js sh101_rom.js midi_songs.js midi_clips.js core.js version.js"
SONG_LIBRARY="songs/twisted"

fail() { echo "publish: $*" >&2; exit 1; }

package() {
    local out
    mkdir -p "$1"
    out=$(cd "$1" && pwd)
    cd "$(dirname "$0")/.."
    # OUT is emptied first: never the checkout or anything holding a repository.
    case "$(pwd)/" in "$out"/*) fail "OUT may not be the checkout or above it" ;; esac
    [ ! -e "$out/.git" ] || fail "OUT holds a git repository"
    [ -f sim/web/core.js ] && [ -f sim/web/sh101_cpu.js ] && [ -f sim/web/version.js ] || fail "build first: sim/build.sh web"
    local commit short
    commit=$(git rev-parse HEAD)
    short=$(git rev-parse --short=7 HEAD)
    grep -q "\"commit\":\"$commit\"" sim/web/version.js || fail "sim/web/version.js is not from $commit: build again"
    grep -q '"dirty":false' sim/web/version.js || fail "the build has uncommitted changes in it"
    rm -rf "$out"
    mkdir -p "$out/page" "$out/source/sim" "$out/source/test"

    # The page, its scripts asked for with the commit, so a browser holding
    # files of the last build in its cache does not mix them with these.
    for f in $PAGE_FILES; do cp "sim/web/$f" "$out/page/$f"; done
    mkdir -p "$out/page/songs"
    cp -R "sim/web/$SONG_LIBRARY" "$out/page/songs/"
    sed -i.bak -E "s#<script src=\"([a-z0-9_]+\\.js)\"></script>#<script src=\"\\1?v=$short\"></script>#" "$out/page/index.html"
    rm "$out/page/index.html.bak"
    local scripts=0
    for f in $PAGE_FILES; do
        case "$f" in *.js) scripts=$((scripts + 1));; esac
    done
    [ "$(grep -c "?v=$short\"" "$out/page/index.html")" -eq "$scripts" ] || fail "index.html does not load all $scripts versioned scripts"

    # The corresponding source of the WebAssembly: what sim/build.sh compiles, and
    # how. dsp.js stays beside the page: it is not part of the core (CC0).
    cp LICENSE "$out/source/"
    cp -R src include "$out/source/"
    cp -R test/fakes "$out/source/test/"
    cp -R sim/core sim/sh101 sim/test sim/build.sh sim/check.sh sim/README.md "$out/source/sim/"
    local subject date emcc
    subject=$(git log -1 --format=%s)
    date=$(git log -1 --format=%cd --date=short)
    emcc=$(sed -E 's/.*"emscripten":"([^"]*)".*/\1/' sim/web/version.js)
    cat > "$out/source/README.md" <<EOF
# Source of the ReBirth simulator page

The page one level up runs the firmware of the ReBirth RP2040 pattern sequencer,
compiled to WebAssembly (\`../core.js\`). This folder is the complete
corresponding source of that build, under the GNU General Public License v3.0
(see \`LICENSE\`).

- Built from commit $commit of the firmware repository
  (branch \`master\`, $date: "$subject"), with Emscripten $emcc,
  by \`sim/build.sh web\` and checked by \`sim/check.sh\`.
- \`src/\`, \`include/\`: the firmware, unmodified.
- \`test/fakes/\`: host-side stand-ins for the Pico SDK headers.
- \`sim/core/\`: the virtual board around the firmware.
- \`sim/sh101/\`: the SH-101 MCS-48 decoder and virtual board (\`../sh101_cpu.js\`).
  The page fetches the Roland ROM from a pinned upstream GitHub revision and
  checks its SHA-256. The file picker is available if the download fails.
- \`sim/build.sh\`: builds the native core and the two WebAssembly modules.
- \`sim/test/\`, \`sim/check.sh\`: checks. The Node checks load the page scripts
  from \`sim/web/\`: copy \`index.html\` and the \`.js\` files from one level up
  into \`sim/web/\` first (without the \`?v=\` the page adds to their names).
- \`sim/README.md\`: what is real, what is modelled, and what the simulator does not show.

The page scripts (\`app.js\`, \`artist.js\`, \`device.js\`, \`rig.js\`, \`acid.js\`,
\`sh101.js\`, \`sh101_rom.js\`) are served as written, one level up. \`dsp.js\` there is the x0xb0x filter model from
[x0xb0x VCF Bench](https://github.com/cozycactus/x0xb0x-vcf-bench), copied
unchanged from its commit 8384696 (CC0). \`manual-ru.pdf\` is the user manual.

Behaviour of the original ReBirth RB-338 is taken from
[OpenRB-338](https://github.com/pingw33n/openrb-338) (also GPL-3.0).
EOF
    [ -z "$(find "$out" -type l)" ] || fail "symbolic links in the package"
    echo "packaged $short into $out"
}

apply() {
    local out site
    out=$(cd "$1" && pwd)
    site=$(cd "$2" && pwd)
    [ -f "$site/.nojekyll" ] || fail "$site/.nojekyll is missing: is this the site checkout?"
    [ -d "$site/rebirth" ] || fail "$site/rebirth is missing"
    [ -z "$(find "$out" -type l)" ] || fail "symbolic links in the package"
    # The package holds exactly the page's files and source/, nothing else.
    local have want
    have=$(cd "$out/page" && ls | sort | tr '\n' ' ')
    want=$(echo $PAGE_FILES songs | tr ' ' '\n' | sort | tr '\n' ' ')
    [ "$have" = "$want" ] || fail "the package's page files are [$have], want [$want]"
    [ "$(cd "$out" && ls | sort | tr '\n' ' ')" = "page source " ] || fail "the package holds more than page/ and source/"
    [ "$(cd "$out/page/songs" && ls)" = "twisted" ] && [ -f "$out/page/$SONG_LIBRARY/catalog.json" ] \
        || fail "the package must contain the Twisted library"
    grep -q "^- Built from commit [0-9a-f]\{40\} " "$out/source/README.md" || fail "source/README.md names no commit"

    for f in $PAGE_FILES; do cp "$out/page/$f" "$site/rebirth/$f"; done
    mkdir -p "$site/rebirth/songs"
    rm -rf "$site/rebirth/$SONG_LIBRARY"
    cp -R "$out/page/$SONG_LIBRARY" "$site/rebirth/songs/"
    rm -rf "$site/rebirth/source"
    cp -R "$out/source" "$site/rebirth/source"

    cd "$site"
    git add -A -- $(for f in $PAGE_FILES; do echo "rebirth/$f"; done) rebirth/source "rebirth/$SONG_LIBRARY"
    # Nothing else in the site may have changed.
    local other
    other=$(git status --porcelain | grep -vE '^[MADR]  ("?rebirth/(source/|songs/twisted/)|rebirth/('"${PAGE_FILES// /|}"')$)' || true)
    [ -z "$other" ] || fail "other files changed in the site: $other"
    if git diff --cached --quiet; then echo "the site already has this build"; else git diff --cached --stat | tail -1; fi
}

case "${1:-}" in
    package) [ $# -eq 2 ] || fail "usage: $0 package OUT"; package "$2" ;;
    apply) [ $# -eq 3 ] || fail "usage: $0 apply OUT SITE"; apply "$2" "$3" ;;
    *) echo "usage: $0 package OUT | apply OUT SITE" >&2; exit 2 ;;
esac
