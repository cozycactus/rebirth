# Jitter Measurement

The firmware is DIN MIDI only. USB MIDI support was removed so timing is not exposed to USB host scheduling.

The current timing path also removes two major avoidable ISR costs:

- MIDI bytes are queued into a UART TX ring and drained by UART0's TX interrupt.
- Pattern and Song playback use prepared events. Core 0 builds the cache on initialization and after edits.

The playback lock serializes timer processing and transport controls. This prevents
Stop/Panic races, but control operations and shared-lock contention can still add
latency. No physical before/after jitter reduction has been established.

## Firmware Metrics

The sequencer timer ISR records these values in microseconds:

- `J`: max IRQ entry latency, measured as actual ISR entry time minus the scheduled timer alarm time.
- `P`: max absolute pulse-period error, measured between consecutive ISR entries.
- `I`: max ISR duration.
- `samples`: number of timer ISR samples.
- `overruns`: count of interrupts at least one pulse late or handlers that miss their next deadline.

A missed alarm is re-armed in the future without a catch-up burst. This prevents
a timer stall; it cannot recover timing already missed. Tempo changes are
accounted for when calculating pulse-period error.

In pattern select, the OLED footer shows:

```text
J:<latency> P:<period> I:<isr>
```

Press step button 13 in pattern select to reset the counters.

## GPIO Probe

GPIO 7 is the timing probe. It is driven high at sequencer timer ISR entry and low at ISR exit.

Use a logic analyzer or oscilloscope:

1. Connect probe channel 0 to GPIO 7 and ground to Pico GND.
2. Optional: connect channels to MIDI TX GPIO 0 and gate outputs GPIO 14-17.
3. Start playback at 120 BPM, then repeat at 200 BPM and 300 BPM.
4. Use a dense stress pattern: all 8 tracks active on all 16 steps, with MIDI and CV/Gate enabled where available.
5. Capture at least 10,000 timer pulses.

## What To Measure

- GPIO 7 rising-edge interval should match the pulse period:
  - 120 BPM: 5208 us
  - 200 BPM: 3125 us
  - 300 BPM: 2083 us
- GPIO 7 pulse width is the ISR duration.
- Gate edge alignment can be checked across GPIO 14-17.
- MIDI TX bursts on GPIO 0 can be compared against gate edges to quantify output skew.
- Decode MIDI Clock separately: 24 messages per quarter, approximately 20.833 ms
  apart at 120 BPM. GPIO 7 measures the 96 PPQN timer, not wire-level MIDI jitter.
- DIN messages share a serial link at 320 us per byte. Note bursts and the UART
  FIFO can delay Clock bytes; gate alignment does not prove MIDI alignment.
- Session panel load case: repeat the measurement while scenes launch every bar,
  the 82 LEDs animate, the TCA8418 is polled and the OLED refreshes. GPIO 7 jitter
  and wire-level MIDI Clock must match the idle capture; Core 1 never touches
  I2C or the LEDs, so any change points to shared-bus or lock contention.
- Exercise mutes, live edits, different loop lengths, Song transitions,
  tempo changes, Stop and Panic. Check that gates stay low after Stop/Panic.
- Save/load are rejected while running. After stopping, test save/load and restart
  to check alarm recovery after flash lockout.

## Current Bench Status

No physical before/after capture is recorded for these changes. Host tests verify
timing logic and output ordering, not physical jitter. Use the GPIO probe and
on-device counters above for repeatable measurements on the assembled hardware.
