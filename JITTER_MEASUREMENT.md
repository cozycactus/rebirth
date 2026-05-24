# Jitter Measurement

The firmware is DIN MIDI only. USB MIDI support was removed so timing is not exposed to USB host scheduling.

The current timing path also removes two major avoidable ISR costs:

- MIDI bytes are queued into a UART TX ring and drained by UART0's TX interrupt.
- Pattern-mode playback uses prepared step events instead of copying full pattern data in the timer ISR.

## Firmware Metrics

The sequencer timer ISR records these values in microseconds:

- `J`: max IRQ entry latency, measured as actual ISR entry time minus the scheduled timer alarm time.
- `P`: max absolute pulse-period error, measured between consecutive ISR entries.
- `I`: max ISR duration.
- `samples`: number of timer ISR samples.
- `overruns`: count of ISR entries later than one full pulse interval.

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

## Current Bench Status

No physical bench capture was made in this session because no RP2040 hardware or logic analyzer is attached to the workspace. The firmware now contains the probe and on-device counters needed to make repeatable measurements on hardware.
