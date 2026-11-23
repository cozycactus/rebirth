#!/usr/bin/env python3
"""Host density probe: today's firmware on one fake DIN wire (README.md).

    python3 tools/midi_density_probe/run_probe.py cell "A N=25" 25 0 0 0 50 300
    python3 tools/midi_density_probe/run_probe.py              # the full table, CSV
    python3 tools/midi_density_probe/run_probe.py long         # the 64-bar cells

A cell is: name, SYNTH tracks, kits on T1-T2 (0/1), every kit voice flammed
(0/1), every SYNTH track on channel 1 (0/1), gate %, BPM; 8 bars. The script
copies src/, include/ and test/ to a temporary folder, adds the probe's
counters to midi_out.c (midi_out.patch), enlarges the fake's byte capture,
builds density_probe beside the host tests with CMake and runs it. The tree
itself is not touched. Set KEEP=1 to keep the build folder and print its path.

Copyright (C) 2026 ReBirth contributors. GPL-3.0-or-later.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

CMAKE_TARGET = """
# Density probe (tools/midi_density_probe), added to the temporary copy only.
add_executable(density_probe
    density_probe.c
    fakes/fake_hardware.c
    ${REBIRTH_SRC}/song.c
    ${REBIRTH_SRC}/pattern.c
    ${REBIRTH_SRC}/kit.c
    ${REBIRTH_SRC}/shared_state.c
    ${REBIRTH_SRC}/midi_out.c
    ${REBIRTH_SRC}/clock_in.c
    ${REBIRTH_SRC}/midi_song.c
)
target_include_directories(density_probe PRIVATE ${REBIRTH_INC} ${CMAKE_SOURCE_DIR}/fakes)
target_compile_definitions(density_probe PRIVATE ${REBIRTH_HOST_DEFS} REBIRTH_THREADED_TEST=1)
target_link_libraries(density_probe PRIVATE Threads::Threads)
"""


def build(work):
    for d in ('src', 'include', 'test'):
        shutil.copytree(os.path.join(ROOT, d), os.path.join(work, d),
                        ignore=shutil.ignore_patterns('build*'))
    subprocess.run(['patch', '-s', '-p1', '-i', os.path.join(HERE, 'midi_out.patch')],
                   cwd=work, check=True)
    fake = os.path.join(work, 'test', 'fakes', 'fake_hardware.h')
    text = open(fake).read()
    old = '#define FAKE_MIDI_CAPACITY 65536'
    if old not in text:
        sys.exit('fake_hardware.h: FAKE_MIDI_CAPACITY changed; update run_probe.py')
    open(fake, 'w').write(text.replace(old, '#define FAKE_MIDI_CAPACITY (1u << 20)'))
    shutil.copy(os.path.join(HERE, 'density_probe.c'), os.path.join(work, 'test'))
    with open(os.path.join(work, 'test', 'CMakeLists.txt'), 'a') as f:
        f.write(CMAKE_TARGET)
    b = os.path.join(work, 'build')
    subprocess.run(['cmake', '-S', os.path.join(work, 'test'), '-B', b, '-DCMAKE_BUILD_TYPE=Debug'],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', b, '--target', 'density_probe'], check=True,
                   stdout=subprocess.DEVNULL)
    return os.path.join(b, 'density_probe')


def main():
    work = tempfile.mkdtemp(prefix='midi_density_probe_')
    try:
        exe = build(work)
        return subprocess.run([exe] + sys.argv[1:]).returncode
    finally:
        if os.environ.get('KEEP'):
            print('build kept in', work, file=sys.stderr)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
