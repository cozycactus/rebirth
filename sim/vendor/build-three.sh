#!/bin/bash
# Builds sim/web/three.js: the parts of three.js listed in three-entry.js, as
# one minified classic script that sets THREE on the global object.
#
#   sim/vendor/build-three.sh
#
# Needs Node and npm and a network: three and esbuild are installed, at the
# pinned versions, into a temporary folder that is removed afterwards. The
# MIT licence comment of three.js stays in the output (--legal-comments=inline).
set -euo pipefail

THREE_VERSION=0.187.0
ESBUILD_VERSION=0.25.0

fail() { echo "build-three: $*" >&2; exit 1; }

cd "$(dirname "$0")/../.."
out=sim/web/three.js
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A package.json of its own, so npm installs here and not in a folder above.
echo '{ "private": true }' > "$tmp/package.json"
cp sim/vendor/three-entry.js "$tmp/entry.js"
(cd "$tmp" && npm install --ignore-scripts --no-audit --no-fund --save-exact \
    "three@$THREE_VERSION" "esbuild@$ESBUILD_VERSION" > /dev/null)

"$tmp/node_modules/.bin/esbuild" "$tmp/entry.js" --bundle --minify --format=iife \
    --legal-comments=inline --target=es2020 --log-level=warning \
    --banner:js="// three.js $THREE_VERSION (MIT, see notices.txt): the parts the hands use, setting THREE. Built by sim/vendor/build-three.sh from sim/vendor/three-entry.js; do not edit." \
    --outfile="$out"

grep -q '@license' "$out" || fail "the @license comment of three.js is missing from $out"
grep -q 'SPDX-License-Identifier: MIT' "$out" || fail "the MIT licence identifier is missing from $out"
echo "$out: $(wc -c < "$out" | tr -d ' ') bytes, sha256 $(shasum -a 256 "$out" | cut -d' ' -f1)"
