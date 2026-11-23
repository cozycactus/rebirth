---
name: rebirth-verify
description: How to check a change in this firmware repository before calling it done, the way CI does it - host tests (Debug preset, clang with -Werror and ASan+UBSan, with and without -fshort-enums), the firmware builds per board preset, and the browser simulator build and checks (sim/build.sh, sim/check.sh, with Emscripten linked into a worktree) - and how to read a red CI run (account or runner failures, cancelled runs, the playback_tests timeout under load). Use it at the end of any task that changes code, tests, CMake files, the simulator or the workflows, and whenever asked to "verify", "run the tests", "check it builds", "is it ready to merge", "why is CI red", "did the checks pass". Also in Russian - "проверь", "прогони тесты", "собери и проверь", "почему CI красный", "упали проверки", "можно мержить", "готово ли к PR". Not for writing a design, spec or plan with no code change.
---

# Verify a change

CI (`.github/workflows/ci.yml`) is the reference; the commands below come from
it and the two `CMakePresets.json` files, and if they disagree, the workflow
wins (the README's `make` steps are older). Run from the repository root. On an
idle machine the whole matrix takes a few minutes.

## What to run

| Change | Run |
| --- | --- |
| Docs, specs, plans only | nothing |
| `src/`, `include/`, `test/` | 1; 2 with host enums; `sim/build.sh native`; 3 (only it compiles `main.c` and `rgb_leds.c` against the SDK). Saved records, structs or enums: also 2 with short enums |
| TX queue in `src/midi_out.c`, step timing in `src/sequencer.c`, pass-through in `src/ui.c` | as `src/`, plus the `midi-wire-budget` skill |
| `sim/` | 4 (build, then full check) |
| CMake, board files, linker script, `hardware/rev-b/boards` | 3 |
| Before a PR or "done" on a code change | all of 1-4 |

Run the suites one after another, never two sanitizer runs side by side: the
timed tests fail under load (see "Reading a red result").

## 1. Host tests (CI job "Host tests", on Linux, Windows and macOS)

    (cd test && cmake --preset host-tests-debug && cmake --build --preset host-tests-debug && ctest --preset host-tests-debug)

## 2. Sanitizers (CI job "Host address and undefined behavior checks")

Two variants. The second sizes enums as the RP2040 toolchain does, which
changes the saved records. Use a build folder per variant:

    cmake -S test -B test/build/sanitizers -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
      '-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
      '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
    cmake --build test/build/sanitizers --parallel
    ctest --test-dir test/build/sanitizers --output-on-failure

For short enums, the same three commands with `test/build/sanitizers-short-enums`
as the folder and `-fshort-enums` added to `CMAKE_C_FLAGS` after `-Werror`.

## 3. Firmware builds (CI job "Firmware build")

Needs `arm-none-eabi-gcc`, Ninja and Pico SDK 2.2.0 in `$HOME/.pico-sdk/sdk/2.2.0`
(the path the presets use). CI builds three of the five presets:

    for p in firmware-debug pico2-release revb-rp2354b; do
      cmake --preset $p && cmake --build --preset $p --parallel || break
    done

`firmware-release` and `revb-rp2350b` are not built by CI; build them when the
change is specific to them. The link fails if RAM or flash overflows. CI
records no size; to report it, `arm-none-eabi-size build/<preset>/*.elf`
(text + data is flash, data + bss is RAM); the map is `build/<preset>/*.elf.map`. Without
the toolchain, say the firmware build is left to CI.

## 4. Simulator (CI job "Simulator")

`sim/build.sh` compiles every firmware source except `main.c` and
`rgb_leds.c`, so it catches firmware changes too. `native` needs only a C
compiler; `web` needs Emscripten in `sim/emsdk`, which is installed in the
main checkout only. In a worktree, link it in, and remove the link when done
(`sim/.gitignore` ignores the folder, not a link, so the link shows as
untracked and must never be committed):

    [ -e sim/emsdk ] || ln -s "$(cd "$(git rev-parse --git-common-dir)/.." && pwd)/sim/emsdk" sim/emsdk
    sim/build.sh            # native core + smoke test, then the WebAssembly cores
    sim/check.sh            # every check for every score this firmware takes
    [ -L sim/emsdk ] && rm sim/emsdk
    git status --short      # no sim/emsdk here

`sim/build.sh native` alone and `sim/check.sh quick` (skips the artist check)
are the fast forms. Always build before checking: `sim/check.sh` tests whatever
`sim/web/core.js` is on disk. A failed artist check leaves its log in
`sim/build/artist-<score>.log`. On a committed tree only,
`sim/publish.sh package <new temp dir>` repeats CI's packaging step (it
empties that folder first, and refuses a build with uncommitted changes). After a merge to master, CI
publishes the page (`.github/workflows/publish-simulator.yml`), so a sim
change is public from then on; never publish by hand.

## Reading a red result

- **Look at the run first:** `gh run list --limit 10`, then `gh run view <id>`
  (jobs, durations, annotations) and `gh run view <id> --log-failed`.
- **Not the code:** every job red within seconds, with an annotation that the
  job was not started because of payments or a spending limit, is an account
  problem for the owner; a job red after about 15 minutes with no steps, no log
  and "not acquired by Runner" is runner capacity (it has hit the macOS host
  tests). Say so, rely on the local runs, and re-run when it is cleared.
- **Cancelled** runs were superseded by a newer push to the same ref
  (`cancel-in-progress` in `ci.yml`); look at the newest run.
- **`playback_tests` (30 s limit) or `clock_tests` (60 s)** red locally:
  look for `Timeout` in the ctest output and check `uptime`. Under load from
  other builds they run past the limit. Re-run the one test alone before
  blaming the code, e.g.
  `ctest --test-dir test/build/sanitizers -R playback_tests --output-on-failure`.

## Limits

No board has been assembled. Host tests, sanitizers, the firmware build and
the simulator are the whole verification: never report a bench or hardware
result, and do not list bench checks as pending work.

## Report

Report each command with pass or fail and its time, anything skipped and why,
and whether CI has run on the pushed commit.
