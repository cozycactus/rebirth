// The page: draws the panel from what the device drives (LED frames, the OLED
// bytes, the step LED latches), turns mouse input into pin and key levels,
// keeps device time in step with the audio clock, and feeds the MIDI wire to
// the synths.
(function () {
  'use strict';

  const LOOKAHEAD_US = 60000;     // the device runs this far ahead of what is heard
  const MAX_STEP_S = 0.05;        // most device time one tick may add; under the look-ahead
  const $ = id => document.getElementById(id);
  const deviceEl = $('device');

  let device, hands, rig = null, ctx = null;
  let playhead = -LOOKAHEAD_US;   // device time that is audible now
  let speed = 1;
  let lastWall = 0, wallIsAudio = false;
  let show = null, phase = 'idle';  // idle | prep | live
  const knobAngle = [0, 0];
  const anims = {};               // rig knob moves in progress, for the sliders

  // ─── Panel geometry, in the 1400-unit square of the panel drawing ──
  const U = v => (v / 14) + '%';
  function box(cls, x, y, w, h, parent) {
    const el = document.createElement('div');
    el.className = 'at ' + cls;
    Object.assign(el.style, { left: U(x), top: U(y), width: U(w), height: U(h) });
    (parent || deviceEl).appendChild(el);
    return el;
  }
  function label(text, x, y) {
    const el = document.createElement('div');
    el.className = 'label';
    el.textContent = text;
    Object.assign(el.style, { left: U(x), top: U(y) });
    deviceEl.appendChild(el);
    return el;
  }
  const PAD_X = t => 288 + 91 * t;
  const PAD_Y = r => 368 + 91 * r;
  const STEP_X = i => [180, 446, 712, 978][i >> 2] + 63 * (i & 3);

  const els = { pad: [], scene: [], mute: [], step: [], stepLed: [], enc: [], play: null, beat: null };
  const held = new Map();         // element -> release function, for everything down right now

  function buildPanel() {
    const cheekL = document.createElement('div'); cheekL.className = 'cheek'; cheekL.style.left = '3.5%';
    const cheekR = document.createElement('div'); cheekR.className = 'cheek'; cheekR.style.right = '3.5%';
    const face = document.createElement('div'); face.className = 'face';
    deviceEl.append(cheekL, cheekR, face);

    const frame = box('', 165, 155, 287, 155);
    frame.id = 'oled-frame';
    const canvas = document.createElement('canvas');
    canvas.id = 'oled'; canvas.width = 128; canvas.height = 64;
    frame.appendChild(canvas);

    [[583, 'TRACK'], [770, 'VALUE']].forEach(([cx, name], enc) => {
      const knob = box('knob', cx - 46, 187, 92, 92);
      knob.appendChild(document.createElement('i'));
      els.enc[enc] = knob;
      label(name, cx, 305);
      bindHold(knob, down => device.encoderSwitch(enc, down));
      knob.addEventListener('wheel', e => { e.preventDefault(); wheel(enc, e.deltaY); }, { passive: false });
      [[-1, '◀', cx - 70], [1, '▶', cx + 70]].forEach(([dir, glyph, x]) => {
        const b = document.createElement('button');
        b.className = 'encbtn'; b.textContent = glyph;
        Object.assign(b.style, { left: U(x), top: U(233) });
        b.addEventListener('click', () => userTurn(enc, dir));
        deviceEl.appendChild(b);
      });
    });

    els.play = box('', 955, 200, 67, 67); els.play.id = 'play';
    bindHold(els.play, down => device.play(down));
    label('PLAY', 989, 305);
    els.beat = box('', 1112, 206, 16, 16); els.beat.id = 'beat';
    label('BEAT', 1120, 267);
    label('SCENE', 1072, 337);

    const well = box('', 270, 350, 860, 752); well.id = 'pad-well';
    for (let row = 0; row < 8; row++) {
      label(String(row + 1), 236, PAD_Y(row) + 40);
      for (let track = 0; track < 8; track++) {
        const pad = box('pad', PAD_X(track), PAD_Y(row), 80, 80);
        (els.pad[track] = els.pad[track] || [])[row] = pad;
        bindHold(pad, down => device.pad(track, row, down));
      }
      const scene = box('scene', 1040, PAD_Y(row) + 7, 65, 65);
      els.scene[row] = scene;
      bindHold(scene, down => device.scene(row, down));
    }
    label('MUTE', 236, 1131);
    for (let track = 0; track < 8; track++) {
      const key = box('mutekey', PAD_X(track), 1113, 80, 37);
      els.mute[track] = key;
      bindHold(key, down => device.mute(track, down));
    }
    for (let i = 0; i < 16; i++) {
      els.stepLed[i] = box('stepled', STEP_X(i) + 19, 1173, 14, 14);
      const key = box('stepkey', STEP_X(i), 1212, 52, 58);
      els.step[i] = key;
      bindHold(key, down => device.step(i, down), true);
      label(String(i + 1), STEP_X(i) + 26, 1290);
    }
  }

  // Press and hold with the pointer; Shift+click latches a control down.
  function bindHold(el, set, latchable) {
    const release = () => {
      if (!held.has(el)) return;
      held.delete(el);
      el.classList.remove('down', 'latched');
      set(false);
    };
    el.addEventListener('pointerdown', e => {
      e.preventDefault();
      takeOver();
      if (el.classList.contains('latched')) { release(); return; }
      el.setPointerCapture(e.pointerId);
      held.set(el, release);
      el.classList.add('down');
      if (latchable && e.shiftKey) el.classList.add('latched');
      set(true);
    });
    const up = () => { if (!el.classList.contains('latched')) release(); };
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
  }
  function releaseAll() {
    for (const release of [...held.values()]) release();
    for (let t = 0; t < 8; t++) {
      device.mute(t, false);
      for (let r = 0; r < 8; r++) device.pad(t, r, false);
      device.scene(t, false);
    }
    for (let i = 0; i < 16; i++) device.step(i, false);
    device.play(false);
    device.encoderSwitch(0, false);
    device.encoderSwitch(1, false);
    hands.settleEncoders();
    deviceEl.querySelectorAll('.finger, .down').forEach(el => el.classList.remove('finger', 'down'));
  }

  let wheelAcc = [0, 0];
  function wheel(enc, deltaY) {
    wheelAcc[enc] += deltaY;
    while (Math.abs(wheelAcc[enc]) >= 30) {
      const dir = wheelAcc[enc] > 0 ? -1 : 1;     // wheel up = clockwise
      wheelAcc[enc] += dir * 30;
      userTurn(enc, dir);
    }
  }
  function userTurn(enc, detents) {
    takeOver();
    hands.turn(enc, detents, device.now());
    knobAngle[enc] += detents * 15;
  }

  // ─── Lights ───────────────────────────────────────────────────
  const LED_FULL = 32;            // the firmware's brightest level (include/led_frame.h)
  const lastLed = new Array(82).fill('');
  function light(el, r, g, b, off, index) {
    const key = r + ',' + g + ',' + b;
    if (lastLed[index] === key) return;
    lastLed[index] = key;
    const top = Math.max(r, g, b);
    if (!top) {
      el.style.background = off;
      el.style.boxShadow = '';
      return;
    }
    const k = Math.min(1, Math.sqrt(top / LED_FULL));
    const hue = [r, g, b].map(c => c / top);
    const offRgb = [0x77, 0x78, 0x7c];
    const lit = hue.map(c => 255 * (c + (1 - c) * (0.25 + 0.45 * k)));     // LEDs behind white silicone
    const mix = lit.map((c, i) => Math.round(offRgb[i] + (c - offRgb[i]) * (0.35 + 0.65 * k)));
    const glow = hue.map(c => Math.round(255 * c));
    el.style.background = 'rgb(' + mix + ')';
    el.style.boxShadow = '0 0 ' + (0.6 + 2.2 * k) + 'cqw rgba(' + glow + ',' + (0.25 + 0.5 * k) + ')';
  }

  let oledSeq = -1, oledCtx, oledImage;
  function drawOled() {
    if (device.oledSeq() === oledSeq) return;
    oledSeq = device.oledSeq();
    const fb = device.oled();
    const px = oledImage.data;
    for (let y = 0; y < 64; y++) {
      for (let x = 0; x < 128; x++) {
        const on = (fb[(y >> 3) * 128 + x] >> (y & 7)) & 1;
        const o = (y * 128 + x) * 4;
        px[o] = on ? 225 : 4; px[o + 1] = on ? 240 : 6; px[o + 2] = on ? 255 : 9; px[o + 3] = 255;
      }
    }
    oledCtx.putImageData(oledImage, 0, 0);
  }

  let lastStepLeds = -1;
  function drawLights() {
    const leds = device.leds();
    for (let row = 0; row < 8; row++) {
      for (let track = 0; track < 8; track++) {
        const i = row * 8 + track;
        light(els.pad[track][row], leds[i * 3], leds[i * 3 + 1], leds[i * 3 + 2], 'var(--pad-off)', i);
      }
      const s = 64 + row;
      light(els.scene[row], leds[s * 3], leds[s * 3 + 1], leds[s * 3 + 2], 'var(--pad-off)', s);
    }
    for (let track = 0; track < 8; track++) {
      const m = 72 + track;
      light(els.mute[track], leds[m * 3], leds[m * 3 + 1], leds[m * 3 + 2], 'var(--pad-off)', m);
    }
    light(els.play, leds[240], leds[241], leds[242], 'var(--key-off)', 80);
    light(els.beat, leds[243], leds[244], leds[245], '#2a2a2c', 81);
    const mask = device.stepLeds();
    if (mask !== lastStepLeds) {
      lastStepLeds = mask;
      for (let i = 0; i < 16; i++) {
        const on = (mask >> i) & 1;
        els.stepLed[i].style.background = on ? '#ff5a4d' : '#3b1414';
        els.stepLed[i].style.boxShadow = on ? '0 0 1.1cqw rgba(255,70,50,.9)' : '';
      }
    }
    els.enc.forEach((knob, enc) => { knob.style.transform = 'rotate(' + knobAngle[enc] + 'deg)'; });
  }

  // ─── Artist output ────────────────────────────────────────────
  function elementFor(a) {
    switch (a.op) {
      case 'pad': return els.pad[a.track][a.row];
      case 'scene': return els.scene[a.row];
      case 'mute': return els.mute[a.track];
      case 'step': return els.step[a.index];
      case 'play': return els.play;
      case 'encsw': return els.enc[a.enc];
    }
    return null;
  }
  const BADGE = { device: 'Прибор', rig: 'Синтезатор', warn: 'Внимание' };
  function when() {
    const t = device.now();
    if (show && phase === 'live' && t >= show.t0) {
      const steps = Math.floor((t - show.t0) / (show.barUs / 16));
      return 'т. ' + (Math.floor(steps / 16) + 1) + '.' + (Math.floor((steps % 16) / 4) + 1);
    }
    return (t / 1e6).toFixed(1) + ' с';
  }
  function say(kind, text) {
    const log = $('log');
    const p = document.createElement('p');
    p.className = kind;
    const w = document.createElement('span'); w.className = 'when'; w.textContent = when();
    const body = document.createElement('span');
    if (BADGE[kind]) { const b = document.createElement('b'); b.textContent = BADGE[kind] + ': '; body.appendChild(b); }
    body.appendChild(document.createTextNode(text));
    p.append(w, document.createTextNode(' '), body);
    log.appendChild(p);
    const caption = $('caption');
    caption.className = kind;
    caption.replaceChildren(body.cloneNode(true));
    while (log.childElementCount > 500) log.firstElementChild.remove();
    log.scrollTop = log.scrollHeight;
  }
  const io = {
    say,
    touch(a) {
      if (a.op === 'edge') { knobAngle[a.enc] += a.dir * 3.75; return; }
      const el = elementFor(a);
      if (!el) return;
      el.classList.toggle('finger', a.down);
      el.classList.toggle('down', a.down);
    },
    rig(id, value, ramp) { moveKnob(id, value, ramp, device.now()); },
  };

  // ─── Synths ───────────────────────────────────────────────────
  const INSTRUMENTS = ['KICK+909', 'BASS', 'ACID 1', 'ACID 2', 'LEAD', 'ARP', 'FX'];
  const CHANNEL_TO_INSTRUMENT = { 9: 0, 2: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6 };
  const KNOBS = ['master.volume', 'acid1.cutoff', 'acid1.resonance', 'acid1.envmod', 'acid1.decay',
                 'acid2.cutoff', 'acid2.resonance', 'bass.cutoff', 'lead.cutoff', 'delay.feedback', 'reverb.level'];
  const lampHot = new Array(INSTRUMENTS.length).fill(0);
  const sliders = {};

  function audioTime(deviceUs) {
    return Math.max(ctx.currentTime, ctx.currentTime + (deviceUs - playhead) / 1e6);
  }
  const sounding = () => rig && ctx && ctx.state === 'running' && speed === 1;

  function moveKnob(id, value, ramp, deviceUs) {
    const slider = sliders[id];
    const from = slider ? +slider.value : value;
    if (sounding() && ramp > 0) {
      rig.setParam(id, value, audioTime(deviceUs), ramp);
      anims[id] = { from, to: value, start: performance.now() + (deviceUs - playhead) / 1000, dur: ramp * 1000 };
    } else {
      if (rig) rig.setParam(id, value);
      delete anims[id];
      if (slider) slider.value = value;
    }
  }
  function buildRigPanel() {
    $('lamps').innerHTML = '';
    INSTRUMENTS.forEach((name, i) => {
      const s = document.createElement('span');
      s.className = 'lamp'; s.textContent = name; s.dataset.i = i;
      $('lamps').appendChild(s);
    });
    if (!rig) {
      $('knobs').textContent = 'Синтезаторы не загрузились (rig.js).';
      return;
    }
    const byId = {};
    for (const p of rig.params) byId[p.id] = p;
    for (const id of KNOBS) {
      const p = byId[id];
      if (!p) continue;
      const wrap = document.createElement('label');
      wrap.textContent = (p.group ? p.group + ' · ' : '') + (p.label || id);
      const input = document.createElement('input');
      input.type = 'range'; input.min = p.min; input.max = p.max; input.step = (p.max - p.min) / 200; input.value = p.value;
      input.addEventListener('input', () => { delete anims[id]; rig.setParam(id, +input.value); });
      wrap.appendChild(input);
      $('knobs').appendChild(wrap);
      sliders[id] = input;
    }
  }
  function startAudio() {
    if (!ctx) return;
    ctx.resume();
  }

  // ─── MIDI wire → synths, monitor ──────────────────────────────
  const midiLines = [];
  let lastClock = 0, clockPeriod = 0, tempoSent = 0;
  const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  function onMidi(m) {
    if (midiLines.length > 3) midiLines.splice(0, midiLines.length - 3);
    if (m.status === 0xF8) {
      // The synths know the tempo the way real gear would: from MIDI Clock.
      if (lastClock) clockPeriod = clockPeriod ? clockPeriod * 0.8 + (m.t - lastClock) * 0.2 : m.t - lastClock;
      lastClock = m.t;
      const bpm = clockPeriod ? 60e6 / (clockPeriod * 24) : 0;
      if (rig && bpm && Math.abs(bpm - tempoSent) > 0.4) { tempoSent = bpm; rig.setTempo(bpm, sounding() ? audioTime(m.t) : undefined); }
      return;
    }
    if (m.status === 0xFA) { lastClock = 0; clockPeriod = 0; }
    if (m.status === 0xFC && rig && sounding()) rig.allOff(audioTime(m.t));
    if (m.status >= 0xF0) {
      midiLines.push((m.t / 1e6).toFixed(3) + '  ' + ({ 0xFA: 'Start', 0xFC: 'Stop', 0xFB: 'Continue' }[m.status] || m.status.toString(16)));
      return;
    }
    const channel = m.status & 15, kind = m.status & 0xF0;
    if (sounding()) rig.midi(m.status, m.d1, m.d2, audioTime(m.t));
    if (kind === 0x90 && m.d2 > 0) {
      const inst = CHANNEL_TO_INSTRUMENT[channel];
      if (inst !== undefined) lampHot[inst] = performance.now() + (m.t - playhead) / 1000 + 90;
      midiLines.push((m.t / 1e6).toFixed(3) + '  ch' + String(channel + 1).padStart(2) + '  ' +
        (NOTE[m.d1 % 12] + (Math.floor(m.d1 / 12) - 1)).padEnd(4) + ' vel ' + m.d2);
    }
  }

  // ─── Clock and main loop ──────────────────────────────────────
  function wall() {
    const audio = !!ctx && ctx.state === 'running';
    if (audio !== wallIsAudio) { wallIsAudio = audio; lastWall = audio ? ctx.currentTime : performance.now() / 1000; }
    return audio ? ctx.currentTime : performance.now() / 1000;
  }
  function runTo(target) {
    target = Math.floor(target);
    // In slices, so the device's event buffer is emptied as it fills.
    while (device.now() < target) {
      hands.advance(Math.min(target, device.now() + 250000));
      for (const e of device.drain()) if (e.type === 'midi') onMidi(e);
    }
  }
  function tick() {
    const w = wall();
    // A tick that comes late (a busy page, a hidden tab) advances the device by
    // less than the look-ahead, so nothing it produces is already in the past:
    // the music slows for a moment instead of playing a bunch of notes at once.
    const dt = Math.min(Math.max(w - lastWall, 0), MAX_STEP_S);
    lastWall = w;
    if (phase === 'prep') {
      const target = speed === Infinity ? show.prepEnd : Math.min(show.prepEnd, device.now() + dt * 1e6 * speed);
      runTo(target);
      playhead = device.now();
      if (device.now() >= show.prepEnd) beginLive();
      return;
    }
    playhead += dt * 1e6;
    runTo(playhead + LOOKAHEAD_US);
    if (phase === 'live' && playhead > show.end) endShow('Сет сыгран. Прибор остался в том состоянии, в котором его оставил артист.');
  }

  function beginLive() {
    phase = 'live';
    speed = 1;
    playhead = device.now() - LOOKAHEAD_US;
    lastWall = wall();
  }
  function silence() {
    if (rig && ctx && ctx.state === 'running') rig.allOff(ctx.currentTime);
  }
  function startShow() {
    startAudio();
    hands.clear();
    releaseAll();
    device.boot(false);
    silence();      // a note the old device state left sounding gets no Note Off any more
    $('log').innerHTML = '';
    lastLed.fill('');
    show = Artist.buildShow();
    Artist.schedule(show, device, hands, io);
    phase = 'prep';
    speed = $('show-prep').checked ? +$('prep-speed').value : Infinity;
    playhead = 0;
    lastWall = wall();
    $('show-start').disabled = true;
    $('show-stop').disabled = false;
  }
  function endShow(message) {
    hands.clear();
    releaseAll();
    phase = 'idle';
    speed = 1;
    playhead = device.now() - LOOKAHEAD_US;
    $('show-start').disabled = false;
    $('show-stop').disabled = true;
    if (message) say('info', message);
  }
  // A hand on the panel during the show takes it away from the artist.
  function takeOver() {
    if (phase !== 'idle') endShow('Артист остановлен: управление у вас.');
  }
  function reboot() {
    if (phase !== 'idle') endShow();
    hands.clear();
    releaseAll();
    device.boot(false);
    lastLed.fill('');
    playhead = device.now() - LOOKAHEAD_US;
    silence();
    say('info', 'Прибор выключен и включён: память как после включения питания.');
  }

  // ─── Status ───────────────────────────────────────────────────
  function drawStatus() {
    const info = device.inspect();
    $('m-time').textContent = (device.now() / 1e6).toFixed(1) + ' с';
    $('m-state').textContent = ['стоп', 'играет', 'пауза'][info.state] + ', ' + info.bpm.toFixed(1) + ' BPM';
    $('m-speed').textContent = phase === 'prep' ? (speed === Infinity ? 'мгновенно' : '×' + speed) : '×1';
    $('m-audio').textContent = !ctx ? 'недоступен' : ctx.state === 'running' ? 'включён' : 'выключен';
    $('audio-toggle').textContent = ctx && ctx.state === 'running' ? '🔇 Выключить звук' : '🔊 Включить звук';
    $('d-overruns').textContent = info.overruns;
    $('d-overflow').textContent = info.midiOverflow ? 'ДА' : 'нет';
    $('d-leds').textContent = info.ledsSkipped;
    $('d-locks').textContent = info.lockErrors;
    const gates = device.gates();
    $('d-gates').textContent = [0, 1, 2, 3].map(i => (gates >> i) & 1 ? '█' : '·').join(' ');
    $('d-cv').textContent = [0, 1, 2, 3].map(i => (device.dac(i) * 2.048 / 4096).toFixed(2)).join(' ');
    $('midi').textContent = midiLines.join('\n');
    if (info.fatal) $('phase').textContent = 'Ошибка симулятора: ' + info.fatal;
    else if (phase === 'prep') {
      $('phase').textContent = 'Подготовка: артист набирает клипы на остановленном приборе (' +
        Math.round(device.now() / 1e6) + ' из ' + Math.round(show.prepEnd / 1e6) + ' с времени прибора)';
      $('progress').firstElementChild.style.width = (100 * device.now() / show.prepEnd) + '%';
    } else if (phase === 'live') {
      const bars = Math.round((show.end - 1500000 - show.t0) / show.barUs);
      const bar = Math.max(1, Math.min(bars, Math.floor((playhead - show.t0) / show.barUs) + 1));
      $('phase').textContent = 'Живой сет: такт ' + bar + ' из ' + bars;
      $('progress').firstElementChild.style.width = (100 * bar / bars) + '%';
    } else if (!show) {
      $('progress').firstElementChild.style.width = '0';
    }
    const now = performance.now();
    document.querySelectorAll('#lamps .lamp').forEach(el => {
      const i = +el.dataset.i;
      el.classList.toggle('on', lampHot[i] > now && lampHot[i] - now < 400);
    });
    for (const id of Object.keys(anims)) {
      const a = anims[id];
      const k = a.dur ? Math.min(1, Math.max(0, (now - a.start) / a.dur)) : 1;
      if (sliders[id]) sliders[id].value = a.from + (a.to - a.from) * k;
      if (k >= 1) delete anims[id];
    }
  }
  function frame() {
    drawOled();
    drawLights();
    drawStatus();
    requestAnimationFrame(frame);
  }

  // ─── Start ────────────────────────────────────────────────────
  RebirthCore().then(core => {
    device = new Device(core);
    hands = new Hands(device);

    buildPanel();
    oledCtx = $('oled').getContext('2d');
    oledImage = oledCtx.createImageData(128, 64);
    try {
      ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
      if (window.Rig) rig = new window.Rig(ctx);
    } catch (error) {
      console.error(error);
    }
    buildRigPanel();

    $('show-start').addEventListener('click', startShow);
    $('show-stop').addEventListener('click', () => {
      endShow('Артист остановлен.');
      // The button also takes the artist's finger to PLAY, so the music stops.
      if (device.inspect().state === 1) {
        device.play(true);
        device.play(false, device.now() + 80000);
      }
    });
    $('reboot').addEventListener('click', reboot);
    $('audio-toggle').addEventListener('click', () => {
      if (!ctx) return;
      if (ctx.state === 'running') { if (rig) rig.allOff(ctx.currentTime); ctx.suspend(); } else ctx.resume();
    });
    $('fault-keys').addEventListener('click', () => {
      device.chipReset(0x34);
      say('warn', 'Сканер пэдов TCA8418 сброшен. Прошивка заметит это при проверке раз в секунду и настроит его заново.');
    });
    $('fault-steps').addEventListener('click', () => {
      device.chipReset(0x20);
      say('warn', 'Расширитель кнопок шагов MCP23017 сброшен. До перенастройки его линии читаются как нажатые; прошивка не должна принять это за нажатия.');
    });
    document.addEventListener('keydown', e => {
      if (e.code !== 'Space' || e.repeat || e.target.tagName === 'BUTTON' || e.target.tagName === 'INPUT') return;
      e.preventDefault();
      takeOver();
      device.play(true);
      device.play(false, device.now() + 80000);
    });

    lastWall = wall();
    setInterval(tick, 10);
    requestAnimationFrame(frame);
    window.sim = { device, hands, get rig() { return rig; }, get ctx() { return ctx; }, get phase() { return phase; },
                   get show() { return show; }, startShow, endShow };
  });
})();
