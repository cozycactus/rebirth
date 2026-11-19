// The page: draws the panel from what the device drives (LED frames, the OLED
// bytes, the step LED latches), turns mouse input into pin and key levels,
// keeps device time in step with the audio clock, and patches the device's
// MIDI and CV/Gate outputs to the synths.
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
  let showPatch = [];
  let importedSongs = [], midiRun = null, launchSerial = 0;
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
    frame.dataset.tip = 'Экран OLED 128 × 64: ровно те точки, что выводит прошивка.';

    [[583, 'TRACK'], [770, 'VALUE']].forEach(([cx, name], enc) => {
      const knob = box('knob', cx - 46, 187, 92, 92);
      knob.appendChild(document.createElement('i'));
      els.enc[enc] = knob;
      knob.dataset.tip = ENC_TIP[enc];
      label(name, cx, 305);
      bindHold(knob, down => device.encoderSwitch(enc, down));
      knob.addEventListener('wheel', e => { e.preventDefault(); wheel(enc, e.deltaY); }, { passive: false });
      [[-1, '◀', cx - 70], [1, '▶', cx + 70]].forEach(([dir, glyph, x]) => {
        const b = document.createElement('button');
        b.className = 'encbtn'; b.textContent = glyph;
        b.dataset.tip = name + ': один щелчок ' + (dir < 0 ? 'влево' : 'вправо');
        Object.assign(b.style, { left: U(x), top: U(233) });
        b.addEventListener('click', () => userTurn(enc, dir));
        deviceEl.appendChild(b);
      });
    });

    els.play = box('', 955, 200, 67, 67); els.play.id = 'play';
    bindHold(els.play, down => device.play(down));
    els.play.dataset.tip = 'PLAY: запуск с начала или остановка. Светится зелёным, пока прибор играет. На клавиатуре — пробел.';
    label('PLAY', 989, 305);
    els.beat = box('', 1112, 206, 16, 16); els.beat.id = 'beat';
    els.beat.dataset.tip = 'Индикатор долей: вспыхивает на каждой четверти, первая доля такта ярче.';
    label('BEAT', 1120, 267);
    label('SCENE', 1072, 337);

    const well = box('', 270, 350, 860, 752); well.id = 'pad-well';
    for (let row = 0; row < 8; row++) {
      label(String(row + 1), 236, PAD_Y(row) + 40);
      for (let track = 0; track < 8; track++) {
        const pad = box('pad', PAD_X(track), PAD_Y(row), 80, 80);
        (els.pad[track] = els.pad[track] || [])[row] = pad;
        bindHold(pad, down => device.pad(track, row, down));
        pad.dataset.tip = 'Клип в столбце ' + (track + 1) + ', слот ' + (row + 1) + ' текущего банка.\n' +
          'На остановленном приборе клип выбирается сразу, во время игры встаёт в очередь до границы квантования (мигает). ' +
          'Ярко светится играющий клип, тускло — клип с шагами. Пустой пэд тоже запускается: дорожка замолкает.';
      }
      const scene = box('scene', 1040, PAD_Y(row) + 7, 65, 65);
      els.scene[row] = scene;
      bindHold(scene, down => device.scene(row, down));
      scene.dataset.tip = 'SCENE ' + (row + 1) + ': запускает ряд ' + (row + 1) + ' на всех 64 дорожках сразу. ' +
        'Пока сцена ждёт в очереди, повторное нажатие отменяет все очереди.';
    }
    label('MUTE', 236, 1131);
    for (let track = 0; track < 8; track++) {
      const key = box('mutekey', PAD_X(track), 1113, 80, 37);
      els.mute[track] = key;
      bindHold(key, down => device.mute(track, down));
      key.dataset.tip = 'MUTE · столбец ' + (track + 1) + ': выключает и возвращает дорожку сразу, без ожидания границы такта. Светится, пока дорожка слышна.';
    }
    for (let i = 0; i < 16; i++) {
      els.stepLed[i] = box('stepled', STEP_X(i) + 19, 1173, 14, 14);
      els.stepLed[i].dataset.tip = 'Индикатор шага ' + (i + 1) + ': шаги клипа в фокусе; бегущий шаг отмечен отдельно.';
      const key = box('stepkey', STEP_X(i), 1212, 52, 58);
      els.step[i] = key;
      bindHold(key, down => device.step(i, down), true);
      key.dataset.tip = 'Кнопка шага ' + (i + 1) + '.\nКоротко: включить или выключить шаг (на дорожке KIT: выкл → нота → акцент).\n' +
        'На экране Pattern держать и крутить TRACK: нота; нажать TRACK: акцент; удержать TRACK: слайд.\n' +
        'На экране SELECT PATTERN: ' + STEP_SELECT_TIP(i) + '.\nShift+клик оставляет кнопку нажатой до следующего клика.';
      label(String(i + 1), STEP_X(i) + 26, 1290);
    }
  }

  // What the controls do, as docs/manual-ru.md describes the firmware.
  const ENC_TIP = [
    'TRACK. Колесо мыши или стрелки по бокам — поворот, клик — нажатие, держать полсекунды — удержание.\n' +
      'Session: поворот — дорожка T1–T64 или линия KIT; на панели видны восемь соседних дорожек; нажать — экран Pattern; удержать — следующий банк A-D.\n' +
      'Pattern, с удержанными шагами: поворот — нота; нажать — акцент; удержать — слайд.\n' +
      'Обе ручки вместе — Panic.',
    'VALUE. Колесо мыши или стрелки по бокам — поворот, клик — нажатие, держать полсекунды — удержание.\n' +
      'Session: поворот — темп (быстрый поворот считает ×4 и ×8); нажать — квантование запуска; удержать — SELECT PATTERN.\n' +
      'Pattern, с удержанными шагами: поворот — velocity; удержать — TIE.\n' +
      'Обе ручки вместе — Panic.',
  ];
  const STEP_SELECT_TIP = i => i < 8 ? 'коротко — MUTE дорожки ' + (i + 1) + ', удержать — её настройка'
    : ['SAVE, сохранить всё во flash', 'LOAD, загрузить из flash', 'LEN−, укоротить клип', 'LEN+, удлинить клип',
       'сброс счётчиков диагностики'][i - 8] || 'нет действия';

  // Press and hold with the pointer; Shift+click latches a control down.
  function bindHold(el, set, latchable) {
    const release = () => {
      if (!held.has(el)) return;
      held.delete(el);
      el.classList.remove('down', 'latched');
      set(false);
    };
    el.addEventListener('pointerdown', e => {
      if (e.button !== 0) return;
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
    releaseKeys();      // a MIDI key left down would keep its step held on a device about to restart
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
      if (a.op === 'key') {
        // The artist's own keyboard: the lamp, and the key if it is on screen.
        lightMidiLamp();
        const key = document.querySelector('#kbd-keys [data-n="' + a.note + '"]');
        if (key) key.classList.toggle('kdown', a.down);
        return;
      }
      const el = elementFor(a);
      if (!el) return;
      el.classList.toggle('finger', a.down);
      el.classList.toggle('down', a.down);
    },
    rig(id, value, ramp) { moveKnob(id, value, ramp, device.now()); },
    padMidi(m) {
      if (sounding()) rig.midi(m.status, m.d1, m.d2, audioTime(m.t));
      if (m.d2) flashStrip(Artist.PAD_CHANNEL, m.t);
    },
  };

  // ─── Synths ───────────────────────────────────────────────────
  // The mixer's strips: a name, the rig knob that is its fader, and the lamp
  // that shows the device sending it a note (the kick and the kit share
  // channel 10; notes 35 and 36 there play the kick, as in Rig's Drums).
  const STRIPS = [
    { name: 'KICK', id: 'kick' }, { name: '909', id: 'drums' }, { name: 'BASS', id: 'bass' },
    { name: 'ACID 1', id: 'acid1' }, { name: 'ACID 2', id: 'acid2' }, { name: 'LEAD', id: 'lead' },
    { name: 'ARP', id: 'arp' }, { name: 'FX', id: 'fx' }, { name: 'PAD', id: 'pad' },
    { name: 'MASTER', level: 'master.volume' },
  ];
  const CV_ROLES = Artist.CV_TRACKS.map(track => STRIPS[track].id);
  const SH101_CHANNEL = { bass: 3, lead: 6, arp: 7, fx: 8, pad: 9 };
  const CV_PATCH = {
    bass: { range: 0 },
    lead: { range: 3 },
    arp: { range: 3 },
    fx: { range: 2, trigger: 1, portaMode: 1, portaTime: 0.18, keyFollow: 0.85,
      cutoff: 0.5, envMod: 0.36, vca: 0, attack: 0.07, sustain: 0.73, release: 0.28 },
  };
  const sh101Panels = {};
  for (const s of STRIPS) if (s.id) s.level = s.id + '.level';
  const stripOf = (channel, note) => channel === 9 ? (note === 35 || note === 36 ? 0 : 1) : channel >= 2 && channel <= 8 ? channel : undefined;
  const KNOB_TIP = {
    tune: 'Tune: подстройка высоты генератора, в середине — без сдвига.',
    cutoff: 'Cut off freq: частота среза фильтра. Артист открывает и прикрывает её по ходу сета.',
    resonance: 'Resonance: насколько фильтр подчёркивает частоту среза, вплоть до свиста.',
    envmod: 'Env mod: как сильно огибающая открывает фильтр на каждой ноте.',
    decay: 'Decay: как быстро фильтр закрывается после начала ноты. На акцентах спад всегда короткий.',
    accent: 'Accent: насколько акцентированная нота громче и ярче обычной.',
    volume: 'Volume: громкость самого инструмента, как ручка на его панели. В общий микс он идёт через фейдер микшера.',
    drive: 'Drive: дисторшн после фильтра. В нуле выключен полностью.',
    color: 'Color: яркость искажённого звука (фильтр после дисторшна).',
    time: 'Time: время дилея в долях такта, по темпу прибора (MIDI Clock).',
    size: 'Size: длина хвоста ревербератора, от 1,2 до 12 секунд.',
    tone: 'Tone: яркость хвоста ревербератора.',
    feedback: 'Обратная связь общей задержки: сколько раз повторяется эхо.',
    level: 'Уровень в общем миксе.',
  };
  const NOT_DEVICE = '\nЭто ручка синтезатора, не прибора: прибор CC не шлёт.';
  const lampHot = new Array(STRIPS.length - 1).fill(0);
  const sliders = {};             // rig knob id -> something with a `value`: a range input or a drawn knob
  const meters = [];              // what drawStatus keeps moving: () => void

  function audioTime(deviceUs) {
    return Math.max(ctx.currentTime, ctx.currentTime + (deviceUs - playhead) / 1e6);
  }
  const sounding = () => rig && ctx && ctx.state === 'running' && speed === 1;

  function moveKnob(id, value, ramp, deviceUs) {
    if (rig) value = Rig.scoreValue(id, value);
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
  const userSet = (id, value) => { delete anims[id]; rig.setParam(id, value); };
  function setVoiceControl(role, name, value) {
    const parts = sh101Panels[role]?.targets ?? [];
    if (!parts.length) { userSet(role + '.' + name, value); return; }
    for (const part of parts) {
      (part.controls ||= {})[name] = value;
      midiRun?.player?.control(part.track, name, value);
    }
  }

  // A knob drawn as the x0xb0x VCF Bench page draws its knobs: ticks, a gold
  // arc for the setting, a pointer. Drag up and down (Shift for fine), arrow
  // keys, double click for the default. Returns something with a `value`.
  const SWEEP = 300, START = -150;   // degrees, 0 at twelve o'clock
  function knobSVG() {
    const ticks = [];
    for (let i = 0; i <= 10; i++) {
      const a = (START + SWEEP * i / 10) * Math.PI / 180;
      const r1 = 44, r2 = i % 5 === 0 ? 49 : 47;
      ticks.push(`<line x1="${50 + r1 * Math.sin(a)}" y1="${50 - r1 * Math.cos(a)}" x2="${50 + r2 * Math.sin(a)}" y2="${50 - r2 * Math.cos(a)}" stroke="var(--b-ink)" stroke-width="${i % 5 === 0 ? 1.6 : 1}"/>`);
    }
    return `<svg viewBox="0 0 100 100" aria-hidden="true">${ticks.join('')}
      <path class="arc" fill="none" stroke="var(--b-gold)" stroke-width="3" stroke-linecap="round"/>
      <circle cx="50" cy="50" r="36" fill="var(--b-bg)" stroke="var(--b-line)" stroke-width="1"/>
      <circle cx="50" cy="50" r="29" fill="var(--b-mask)" stroke="var(--b-line)" stroke-width="1"/>
      <line class="ptr" x1="50" y1="50" x2="50" y2="24" stroke="var(--b-ink)" stroke-width="3.2" stroke-linecap="round"/></svg>`;
  }
  function arcPath(v) {
    const r = 40, a0 = START * Math.PI / 180, a1 = (START + SWEEP * v) * Math.PI / 180;
    const p = (a) => `${50 + r * Math.sin(a)} ${50 - r * Math.cos(a)}`;
    return v <= 0.001 ? '' : `M ${p(a0)} A ${r} ${r} 0 ${SWEEP * v > 180 ? 1 : 0} 1 ${p(a1)}`;
  }
  function benchKnob(container, id, label, tip) {
    const p = rig.params.find(x => x.id === id), name = id.split('.')[1], format = window.Rig.format[name];
    const cell = document.createElement('div');
    cell.className = 'bcell';
    cell.dataset.tip = (tip || KNOB_TIP[name]) + '\nТянуть вверх и вниз (с Shift точнее), стрелки на клавиатуре, двойной клик — исходное положение.' + NOT_DEVICE;
    cell.innerHTML = `<div class="bknob" role="slider" tabindex="0" aria-label="${p.label}" aria-valuemin="0" aria-valuemax="10">${knobSVG()}</div>
      <span class="bval"></span><span class="legend">${label || p.label}</span>`;
    container.appendChild(cell);
    // v is the knob's turn, 0 to 1; the knob's own range may be narrower
    const el = cell.querySelector('.bknob'), shown = cell.querySelector('.bval'), def = (p.value - p.min) / (p.max - p.min);
    let v = def;
    const draw = () => {
      el.querySelector('.ptr').setAttribute('transform', `rotate(${START + SWEEP * v} 50 50)`);
      el.querySelector('.arc').setAttribute('d', arcPath(v));
      const text = format ? format(p.min + v * (p.max - p.min)) : (v * 10).toFixed(1);
      el.setAttribute('aria-valuenow', (v * 10).toFixed(1));
      el.setAttribute('aria-valuetext', text);
      shown.textContent = text;
    };
    handleKnob(el, id, () => v, (to) => { v = to; draw(); }, def);
  }

  // What every drawn knob does: drag up and down (Shift for fine), arrow and
  // Page keys, Home and End, double click for the default. `get` reads the
  // knob's turn (0 to 1), `show` draws a new one; the rig is set here, in the
  // knob's own range.
  function handleKnob(el, id, get, show, def) {
    const p = rig.params.find(x => x.id === id), span = p.max - p.min;
    const clamp = (x) => Math.min(1, Math.max(0, x));
    const turn = (to) => { show(clamp(to)); userSet(id, p.min + get() * span); };
    let startY = 0, startV = 0, dragging = false;
    el.addEventListener('pointerdown', (e) => { dragging = true; startY = e.clientY; startV = get(); el.setPointerCapture(e.pointerId); e.preventDefault(); });
    el.addEventListener('pointermove', (e) => { if (dragging) turn(startV + (startY - e.clientY) / (e.shiftKey ? 600 : 180)); });
    const end = () => { dragging = false; };
    el.addEventListener('pointerup', end);
    el.addEventListener('pointercancel', end);
    el.addEventListener('dblclick', () => turn(def));
    el.addEventListener('keydown', (e) => {
      const step = { ArrowUp: 0.01, ArrowRight: 0.01, ArrowDown: -0.01, ArrowLeft: -0.01, PageUp: 0.1, PageDown: -0.1 }[e.key];
      if (step !== undefined) { e.preventDefault(); turn(get() + step); }
      if (e.key === 'Home') { e.preventDefault(); turn(0); }
      if (e.key === 'End') { e.preventDefault(); turn(1); }
    });
    show(get());
    sliders[id] = { get value() { return p.min + get() * span; }, set value(to) { show(clamp((+to - p.min) / span)); } };
  }

  // A small knob on a mixer strip. A pan knob draws its arc from the centre.
  const MIX_KNOB = {
    delay: { label: 'DLY', tip: (s) => 'Посыл канала «' + s + '» в дилей блока Effects (пинг-понг по темпу прибора).',
             text: (v) => (v * 10).toFixed(1) },
    reverb: { label: 'REV', tip: (s) => 'Посыл канала «' + s + '» в ревербератор блока Effects.', text: (v) => (v * 10).toFixed(1) },
    pan: { label: 'PAN', tip: (s) => 'Панорама канала «' + s + '»: влево, по центру (C), вправо.',
           text: (v) => Math.abs(v - 0.5) < 0.005 ? 'C' : (v < 0.5 ? 'L' : 'R') + Math.round(Math.abs(v - 0.5) * 200) },
  };
  function mixKnob(container, id, stripName) {
    const name = id.split('.')[1], kind = MIX_KNOB[name], p = rig.params.find(x => x.id === id);
    const cell = document.createElement('div');
    cell.className = 'mknob';
    cell.dataset.tip = kind.tip(stripName) + (name === 'reverb' && id === 'drums.reverb' ? ' Только малый, клэп и крэш: хэты, томы и райд идут сухими.' : '') +
      '\nТянуть вверх и вниз (с Shift точнее), стрелки, двойной клик — исходное положение.' + NOT_DEVICE;
    cell.innerHTML = `<span class="mname">${kind.label}</span><div class="dial" role="slider" tabindex="0" aria-label="${stripName} ${kind.label}" aria-valuemin="0" aria-valuemax="1">` +
      `<svg viewBox="0 0 40 40" aria-hidden="true"><circle cx="20" cy="20" r="13"/><path class="arc"/>` +
      `<line class="ptr" x1="20" y1="20" x2="20" y2="9"/></svg></div><span class="mval"></span>`;
    container.appendChild(cell);
    const el = cell.querySelector('.dial'), shown = cell.querySelector('.mval');
    const r = 17, at = (deg) => { const a = deg * Math.PI / 180; return `${20 + r * Math.sin(a)} ${20 - r * Math.cos(a)}`; };
    let v = p.value;
    const draw = (to) => {
      v = to;
      const deg = START + SWEEP * v, from = name === 'pan' ? 0 : START, a = Math.min(from, deg), b = Math.max(from, deg);
      el.querySelector('.ptr').setAttribute('transform', `rotate(${deg} 20 20)`);
      el.querySelector('.arc').setAttribute('d', b - a < 0.5 ? '' : `M ${at(a)} A ${r} ${r} 0 ${b - a > 180 ? 1 : 0} 1 ${at(b)}`);
      el.setAttribute('aria-valuenow', v.toFixed(2));
      el.setAttribute('aria-valuetext', kind.text(v));
      shown.textContent = kind.text(v);
    };
    handleKnob(el, id, () => v, draw, p.value);
  }

  // One 303 as the Bench page shows it: the panel with its seven knobs, the
  // waveform switch and the ladder-current meter, and the Effects board.
  function buildAcid(id, index, title, track) {
    const voice = rig.inst[index], root = $(id);
    root.innerHTML = `<section class="board" aria-label="${title}">
        <div class="bhead"><span class="legend">${title} · x0xb0x</span>
          <span class="small">Модель из x0xb0x VCF Bench: фильтр, огибающая, акцент, VCA и генератор. Ноты приходят с дорожки T${track} прибора.</span></div>
        <div class="bknobs"></div>
        <div class="bmeter" data-tip="Ток диодной лестницы фильтра: чем он больше, тем выше срез. Видно, как его двигают ручки, огибающая и акцент.">
          <span class="legend">Ladder current, peak over 30 ms</span><span class="breadout">– µA</span>
          <div class="bbar" aria-hidden="true"><i></i></div><div class="bticks" aria-hidden="true"></div>
        </div>
      </section>
      <section class="board" aria-label="${title}: distortion">
        <div class="bhead"><span class="legend">Distortion</span>
          <span class="small">Дисторшн со страницы Bench, в канале этой 303 до фейдера. В самом x0xb0x его нет.
            Дилей и реверб общие: блок Effects над 303, посылы на полосе микшера.</span></div>
        <div class="bknobs"></div>
      </section>`;
    const [panel, effects] = root.querySelectorAll('.bknobs');
    for (const [name, label] of [['tune'], ['cutoff'], ['resonance', 'Reso­nance'], ['envmod'], ['decay'], ['accent'], ['volume']]) {
      benchKnob(panel, id + '.' + name, label);
    }
    // the SAW / SQR switch sits in the eighth cell
    const sw = document.createElement('div');
    sw.className = 'bcell';
    sw.dataset.tip = 'Форма волны генератора: пила или меандр, как переключатель на панели.';
    sw.innerHTML = `<div class="bswitch" role="group" aria-label="Waveform"><button type="button">Saw</button><button type="button">Sqr</button></div>
      <span class="bval">&nbsp;</span><span class="legend">Waveform</span>`;
    panel.appendChild(sw);
    const [saw, sqr] = sw.querySelectorAll('button');
    const setWave = (w) => {
      voice.setWave(w);
      saw.setAttribute('aria-pressed', String(voice.wave === 'saw'));
      sqr.setAttribute('aria-pressed', String(voice.wave === 'square'));
    };
    saw.addEventListener('click', () => setWave('saw'));
    sqr.addEventListener('click', () => setWave('square'));
    setWave(voice.wave);
    for (const name of ['drive', 'color']) benchKnob(effects, id + '.' + name);
    // ladder-current meter, log scale 2 to 300 uA
    const LO = Math.log(2), HI = Math.log(300);
    const pos = (ua) => Math.min(1, Math.max(0, (Math.log(Math.max(ua, 2)) - LO) / (HI - LO)));
    const ticks = root.querySelector('.bticks'), bar = root.querySelector('.bbar i'), readout = root.querySelector('.breadout');
    ticks.innerHTML = [3, 10, 30, 100, 300].map(u => `<span style="left:${(pos(u) * 100).toFixed(1)}%">${u}</span>`).join('');
    ticks.lastElementChild.style.transform = 'translateX(-100%)';
    meters.push(() => {
      const ua = voice.ua;
      if (!(ua > 0) || !ctx || ctx.state !== 'running') { bar.style.width = '0'; readout.textContent = '– µA'; return; }
      bar.style.width = (pos(ua) * 100).toFixed(1) + '%';
      readout.textContent = (ua < 10 ? ua.toFixed(1) : ua.toFixed(0)) + ' µA';
    });
  }

  // The shared Effects board: the Bench page's delay and reverb, which every
  // strip of the mixer sends to.
  const FX_TIP = {
    'delay.level': 'Delay: громкость возврата дилея в общий микс. Сколько в него идёт от каждого инструмента — ручка DLY на его полосе.',
    'delay.time': KNOB_TIP.time,
    'delay.feedback': 'Feedback: сколько раз повторяется эхо. Артист прибавляет его к пикам.',
    'reverb.level': 'Space: громкость возврата ревербератора в общий микс. Сколько в него идёт от каждого инструмента — ручка REV на его полосе. ' +
      'Артист открывает его в брейках.',
    'reverb.size': KNOB_TIP.size,
    'reverb.tone': KNOB_TIP.tone,
  };
  function buildEffects() {
    const root = $('effects');
    root.innerHTML = `<section class="board" aria-label="Effects">
        <div class="bhead"><span class="legend">Effects</span>
          <span class="small">Дилей и ревербератор со страницы Bench, общие для всех инструментов: посылы DLY и REV на полосах микшера.
            Эхо дилея тоже уходит в ревербератор.</span></div>
        <div class="bknobs"></div>
      </section>`;
    const box = root.querySelector('.bknobs');
    for (const [id, label] of [['delay.level', 'Delay'], ['delay.time'], ['delay.feedback', 'Feed­back'],
      ['reverb.level', 'Space'], ['reverb.size'], ['reverb.tone']]) benchKnob(box, id, label, FX_TIP[id]);
  }

  function buildMixer() {
    const box = $('mixer');
    box.innerHTML = '';
    STRIPS.forEach((strip, i) => {
      const p = rig && rig.params.find(x => x.id === strip.level), master = i === STRIPS.length - 1;
      const el = document.createElement('div');
      el.className = 'strip' + (master ? ' master' : '');
      const synthPanel = window.SH101 && p && $('sh101-' + strip.id + '-panel');
      const lampMarkup = synthPanel
        ? `<a class="lamp" href="#${synthPanel.id}" aria-label="${strip.name} · SH-101">${strip.name}</a>`
        : `<span class="lamp">${strip.name}</span>`;
      el.innerHTML = lampMarkup + `<div class="mknobs"></div><div class="well"><input class="fader" type="range" aria-label="${strip.name}">` +
        (master ? '<div class="vu"><i></i></div>' : '') + `</div><span class="val"></span>` +
        (master ? '' : `<button type="button" aria-pressed="false" aria-label="Mute ${strip.name}">M</button>`);
      box.appendChild(el);
      const lamp = el.querySelector('.lamp'), fader = el.querySelector('.fader'), val = el.querySelector('.val'), mute = el.querySelector('button');
      if (!master) lamp.dataset.i = i;
      lamp.dataset.tip = master ? 'Общий выход всех синтезаторов.'
        : 'Вспыхивает, когда прибор шлёт ноту этому инструменту.' + (strip.name.startsWith('ACID') ? ' Это модель x0xb0x (схема TB-303) из x0xb0x VCF Bench.' : '');
      if (synthPanel) {
        lamp.dataset.tip = 'Открыть панель SH-101 канала ' + strip.name + '. ' + lamp.dataset.tip;
        lamp.addEventListener('click', () => {
          if (fullMidiSong()) sh101Panels[strip.id]?.choosePart(-2);
          synthPanel.open = true;
        });
      }
      if (!p) { fader.disabled = true; return; }
      // Sends to the Effects board, then pan, as on a console.
      const knobs = el.querySelector('.mknobs');
      for (const name of ['delay', 'reverb', 'pan']) {
        const id = strip.id && strip.id + '.' + name;
        if (id && rig.params.some(x => x.id === id)) mixKnob(knobs, id, strip.name);
        else knobs.appendChild(Object.assign(document.createElement('div'), { className: 'mknob empty' }));
      }
      fader.min = p.min; fader.max = p.max; fader.step = (p.max - p.min) / 100; fader.value = p.value;
      fader.dataset.tip = (master ? 'Общая громкость.' : 'Уровень канала «' + strip.name + '» в общем миксе.') + NOT_DEVICE;
      const show = () => { val.textContent = (10 * fader.value / p.max).toFixed(1); };
      let saved = null;           // the level a muted strip returns to
      const unmute = () => { saved = null; el.classList.remove('muted'); if (mute) mute.setAttribute('aria-pressed', 'false'); };
      fader.addEventListener('input', () => { unmute(); userSet(strip.level, +fader.value); show(); });
      if (mute) {
        mute.dataset.tip = 'Заглушить канал в микшере страницы. Это не MUTE прибора: прибор продолжает слать ноты. ' +
          'Как на пульте, посылы глохнут вместе с каналом, а то, что уже ушло в дилей и реверб, доигрывает.';
        mute.addEventListener('click', () => {
          if (saved === null) {
            saved = +fader.value;
            el.classList.add('muted');
            mute.setAttribute('aria-pressed', 'true');
            userSet(strip.level, 0);
          } else {
            const back = saved;
            unmute();
            userSet(strip.level, back);
          }
        });
      }
      show();
      // a move the artist makes shows on the fader too
      sliders[strip.level] = { get value() { return fader.value; }, set value(to) { fader.value = to; show(); } };
      if (master) {
        const vu = el.querySelector('.vu i'), samples = new Float32Array(rig.analysers[0].fftSize);
        el.querySelector('.vu').dataset.tip = 'Пик на выходе, от −40 дБ внизу до полной шкалы вверху.';
        let held = 0;
        meters.push(() => {
          let peak = 0;
          if (ctx && ctx.state === 'running') {
            for (const analyser of rig.analysers) {
              analyser.getFloatTimeDomainData(samples);
              for (let k = 0; k < samples.length; k++) { const a = Math.abs(samples[k]); if (a > peak) peak = a; }
            }
          }
          held = Math.max(peak, held * 0.9);      // falls back by about 1 dB a frame
          const db = 20 * Math.log10(Math.max(held, 1e-4));
          vu.style.height = Math.min(100, Math.max(0, (db + 40) * 2.5)) + '%';
          vu.style.background = held > 0.89 ? 'var(--warn)' : 'var(--accent)';
        });
      }
    });
  }

  function buildSH101Fields(role, voiceControls, performance, accessories, clock) {
    const makeControl = (def) => {
      const id = role + '.' + def.id;
      const label = document.createElement('label');
      const title = document.createElement('span');
      title.textContent = def.face || def.label;
      label.appendChild(title);
      label.dataset.tip = def.tip || def.label;
      const input = document.createElement(def.options ? 'select' : 'input');
      input.id = 'sh101-' + role + '-' + def.id;
      input.setAttribute('aria-label', def.label);
      const order = def.order || def.options?.map((_, index) => index);
      if (order) for (const index of order) input.add(new Option(def.options[index], index));
      else {
        input.type = 'range';
        input.min = 0;
        input.max = 1;
        input.step = 0.005;
      }
      input.value = rig.getParam(id);
      if (def.accessory) {
        label.appendChild(input);
        accessories.prepend(label);
        input.addEventListener('input', () => setVoiceControl(role, def.id, +input.value));
        sliders[id] = input;
        return null;
      }
      const actuator = document.createElement('span');
      const setting = document.createElement('span');
      const max = window.SH101.max(def);
      actuator.className = 'sh101-actuator';
      setting.className = 'sh101-setting';
      setting.setAttribute('aria-hidden', 'true');
      label.className = 'sh101-control ' + (def.dial ? 'sh101-dial' : def.options ? 'sh101-switch' : 'sh101-fader');
      if (def.horizontal) label.classList.add('sh101-horizontal');
      actuator.appendChild(input);
      if (def.dial || def.options) actuator.appendChild(document.createElement('i'));
      label.append(actuator, setting);
      const reflect = () => {
        const value = +input.value;
        if (def.dial) actuator.style.setProperty('--angle', (value / max * 270 - 135) + 'deg');
        if (def.options) {
          actuator.style.setProperty('--travel', order.indexOf(value) / max);
          setting.textContent = (def.marks || def.options)[value];
        }
      };
      reflect();
      input.addEventListener('input', () => { reflect(); setVoiceControl(role, def.id, +input.value); });
      sliders[id] = { get value() { return input.value; }, set value(value) { input.value = value; reflect(); } };
      return label;
    };
    for (const [group, entries] of window.SH101.groups) {
      const bank = document.createElement('section');
      const heading = document.createElement('h4');
      const controls = document.createElement('div');
      bank.className = 'sh101-bank';
      controls.className = 'sh101-bank-controls';
      heading.textContent = group === 'TUNE' ? '' : group;
      if (group === 'TUNE') heading.setAttribute('aria-hidden', 'true');
      for (const def of entries) {
        const label = makeControl(def);
        if (label) controls.appendChild(label);
      }
      bank.append(heading, controls);
      if (group === 'MODULATOR') bank.appendChild(clock);
      if (group === 'CONTROLLERS' || group === 'BENDER') performance.insertBefore(bank, performance.lastElementChild);
      else voiceControls.appendChild(bank);
    }
  }

  function bindSH101Audition(role, play) {
    const audition = $('sh101-' + role + '-audition'), status = $('sh101-' + role + '-status');
    const strip = STRIPS.findIndex(s => s.id === role);
    audition.addEventListener('click', async () => {
      try {
        await startAudio();
        if (ctx.state === 'running') {
          play(ctx.currentTime + 0.01);
          lampHot[strip] = performance.now() + 100;
          status.textContent = '';
          return;
        }
      } catch { /* Surface a rejected audio resume in the same status. */ }
      status.textContent = 'Браузер не запустил звук.';
    });
    return audition;
  }

  function buildSH101() {
    if (!window.SH101) return;
    for (const [role, voice] of Object.entries(rig.sh101)) sh101Panels[role] = buildSH101Panel(role, voice);
    $('sh101').hidden = false;
    const openArpeggio = () => {
      $('sh101-arp-panel').open = true;
      sh101Panels.arp.selectInput('rom');
    };
    $('sh101-arp-link').addEventListener('click', openArpeggio);
    const openLinkedPanel = () => {
      // Keep links to the earlier arpeggio panel working.
      if (location.hash === '#sh101-rom') openArpeggio();
      const target = $(location.hash === '#sh101-rom' ? 'sh101-arp-panel' : location.hash.slice(1));
      if (target?.matches('#sh101 > details')) {
        target.open = true;
        target.scrollIntoView();
      }
    };
    window.addEventListener('hashchange', openLinkedPanel);
    openLinkedPanel();
    loadSH101ROM(Object.values(sh101Panels));
  }

  const importStatus = message => { $('midi-import-status').textContent = message; };
  function selectedMidiSong() {
    const value = $('show-song').value;
    return value.startsWith('midi:') ? importedSongs[Number(value.slice(5))] : null;
  }

  function fullMidiSong() {
    if (midiRun) return midiRun.player ? midiRun.song : null;
    if (phase === 'prep' || phase === 'live') return null;
    return $('midi-song-mode').value === 'full' ? selectedMidiSong() : null;
  }

  function refreshMidiParts() {
    const song = fullMidiSong();
    for (const [role, panel] of Object.entries(sh101Panels)) {
      panel.showParts(song?.tracks.filter(part => part.role === role && part.notes && !part.drum) ?? []);
    }
  }

  function openMidiPart(part) {
    if (!part || !part.events.length) return;
    $('caption').textContent = 'MIDI ' + (part.track + 1) + ' · ' + part.name;
    if (part.drum || !part.notes) return;
    const panel = sh101Panels[part.role];
    if (!panel) return;
    panel.choosePart(part.track);
    const details = $('sh101-' + part.role + '-panel');
    details.open = true;
    details.scrollIntoView({block: 'start'});
  }

  function showMidiTracks(song) {
    $('midi-song-settings').hidden = !song;
    for (const id of ['show-form', 'show-prep', 'prep-speed', 'show-variant']) $(id).disabled = !!song;
    const list = $('midi-song-tracks');
    list.replaceChildren();
    refreshMidiParts();
    if (!song) return;
    for (const part of song.tracks.filter(p => p.notes || p.events.length)) {
      const row = document.createElement('div');
      row.className = 'row';
      const route = document.createElement('select');
      route.setAttribute('aria-label', 'Выход дорожки ' + part.track + ' ' + part.name);
      route.add(new Option('MIDI из файла', ''));
      for (let i = 0; i < 16; i++) route.add(new Option('MIDI ' + (i + 1), 'm' + i));
      for (let i = 0; i < 4; i++) {
        const option = new Option('CV/Gate ' + (i + 1), 'c' + i);
        option.disabled = part.poly !== 1;
        route.add(option);
      }
      route.add(new Option('Пропустить', 'mute'));
      const transposition = document.createElement('input');
      transposition.type = 'number';
      transposition.min = -127;
      transposition.max = 127;
      transposition.value = part.route?.transpose ?? 0;
      transposition.title = 'Сдвиг высоты CV в полутонах';
      transposition.setAttribute('aria-label', 'Сдвиг высоты CV дорожки ' + part.track);
      const routeValue = part.route;
      if (routeValue?.mute) route.value = 'mute';
      else if (routeValue?.cv !== undefined) route.value = 'c' + routeValue.cv;
      else if (routeValue?.midi !== undefined) route.value = 'm' + routeValue.midi;
      const update = () => {
        const value = route.value, n = Number(value.slice(1));
        transposition.hidden = !value.startsWith('c');
        part.route = value === 'mute' ? { mute: true }
          : value.startsWith('c') ? { cv: n, transpose: Number(transposition.value) || 0 }
          : value.startsWith('m') ? { midi: n } : undefined;
      };
      route.addEventListener('change', update);
      transposition.addEventListener('input', update);
      update();
      const name = document.createElement('button');
      name.type = 'button';
      name.className = 'btn';
      name.disabled = !part.notes || !!part.drum || part.role === 'gate';
      name.textContent = (part.track + 1) + ' · ' + part.name + (part.patch ? ' · TAL ' + part.patch.name : '');
      name.addEventListener('click', () => openMidiPart(part));
      row.append(route, transposition, name);
      list.appendChild(row);
    }
  }

  function firmwareMidiImage(song) {
    return MidiSongs.image(song, Object.fromEntries(song.tracks.filter(p => p.route).map(p => [p.track, p.route])));
  }

  function initMidiLibrary() {
    const select = $('show-song');
    const group = document.createElement('optgroup');
    group.label = 'Импортированные MIDI';
    select.appendChild(group);
    async function loadFiles(files, fromLocal = false) {
      try {
        const result = await MidiSongs.load(files);
        importedSongs = result.songs;
        group.replaceChildren();
        importedSongs.forEach((song, i) => group.appendChild(new Option(song.title + ' · ' +
          Math.round(song.bpm) + ' BPM · ' + song.tracks.length + ' дор.', 'midi:' + i)));
        const request = new URLSearchParams(location.search).get('song');
        const index = importedSongs.findIndex(song => song.id === request);
        if (index >= 0 || !fromLocal) select.value = 'midi:' + Math.max(0, index);
        select.dispatchEvent(new Event('change'));
        const count = importedSongs.flatMap(song => song.tracks).filter(part => part.patch).length;
        importStatus('Загружено песен: ' + importedSongs.length + ' · привязано пресетов TAL: ' + count +
          (result.unmatched.length ? ' · без совпадения: ' + result.unmatched.map(p => p.file).join(', ') : ''));
      } catch (error) { importStatus(error.message); }
    }
    const input = $('midi-folder');
    $('midi-song-mode').addEventListener('change', refreshMidiParts);
    input.addEventListener('change', () => { if (input.files.length) loadFiles(input.files); });
    $('midi-song-export').addEventListener('click', () => {
      const song = selectedMidiSong();
      if (!song) return;
      try {
        const board = $('midi-song-board').value;
        const url = URL.createObjectURL(new Blob([MidiSongs.uf2(firmwareMidiImage(song), board)]));
        const link = document.createElement('a');
        link.href = url;
        link.download = song.title + (board === 'pico2' ? ' - ReBirth Pico 2 MIDI Song.uf2' : ' - ReBirth MIDI Song.uf2');
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
      } catch (error) { importStatus(error.message); }
    });
    async function loadBundledLibrary() {
      const local = ['127.0.0.1', 'localhost', '[::1]'].includes(location.hostname);
      const folders = local ? ['./local-songs/twisted/', './songs/twisted/'] : ['./songs/twisted/'];
      for (const folder of folders) {
        const response = await fetch(folder + 'catalog.json');
        if (response.status === 404 && folder.startsWith('./local-songs/')) continue;
        if (!response.ok) throw Error('Не удалось загрузить список песен Twisted.');
        const paths = await response.json();
        const files = await Promise.all(paths.map(async path => {
          const result = await fetch(folder + path.split('/').map(encodeURIComponent).join('/'));
          if (!result.ok) throw Error('Файл не найден: ' + path);
          return new File([await result.blob()], path.split('/').pop());
        }));
        await loadFiles(files, true);
        return;
      }
    }
    if (location.protocol === 'https:' || location.protocol === 'http:') {
      loadBundledLibrary().catch(error => importStatus(error.message));
    }
  }

  function buildSH101Panel(role, voice) {
    const api = window.SH101ROM, fx = role === 'fx';
    const details = $('sh101-' + role + '-panel'), fields = $('sh101-' + role);
    const status = $('sh101-' + role + '-status');
    const fragment = $('sh101-rom-controls').content.cloneNode(true);
    const control = name => details.querySelector(`[data-rom="${name}"]`);
    details.querySelector('.sh101-transport').prepend(fragment.querySelector('[data-rom="input-label"]'));
    fields.before(fragment.querySelector('[data-rom="accessories"]'), fragment.querySelector('[data-rom="arp-hint"]'));
    fields.after(fragment.querySelector('[data-rom="hint"]'));
    fields.appendChild(fragment);
    const input = control('input');
    const cvPair = CV_ROLES.indexOf(role), romOption = input.querySelector('[value="rom"]');
    input.options[0].textContent = 'MIDI · CH ' + SH101_CHANNEL[role];
    if (cvPair >= 0) input.add(new Option('CV/Gate ' + (cvPair + 1), 'cv'), romOption);
    const inputBadge = details.querySelector('summary .sub');
    const partPicker = document.createElement('label');
    const partSelect = document.createElement('select');
    const partActivity = document.createElement('span');
    partActivity.className = 'sub';
    partPicker.hidden = true;
    partPicker.append('MIDI-партия ', partSelect, partActivity);
    details.querySelector('.sh101-transport').appendChild(partPicker);
    const trackLevel = document.createElement('label');
    const gain = document.createElement('input'), levelReadout = document.createElement('span');
    gain.type = 'range';
    gain.min = -18;
    gain.max = 24;
    gain.step = 1;
    gain.setAttribute('aria-label', 'Уровень MIDI-партии, дБ');
    trackLevel.hidden = true;
    trackLevel.append('Уровень партии ', gain, levelReadout);
    details.querySelector('.sh101-transport').appendChild(trackLevel);
    let parts = [], selectedPart = null, groupSelected = false;
    const targets = () => groupSelected ? parts : selectedPart ? [selectedPart] : [];
    function showPartActivity() {
      if (partPicker.hidden || !details.open) return;
      const active = targets().filter(part => midiRun?.active?.[part.track]).length;
      const text = !targets().length ? '' : active ? ' · звучит ' + active : ' · ждёт ноты';
      if (partActivity.textContent !== text) partActivity.textContent = text;
    }
    function showPartLevel() {
      if (!selectedPart) return;
      gain.value = selectedPart.levelDb ?? 0;
      levelReadout.textContent = ' ' + gain.value + ' дБ';
    }
    function choosePart(track) {
      groupSelected = track === -2 && !!parts.length;
      selectedPart = parts.find(part => part.track === track) ?? null;
      partSelect.value = groupSelected ? '-2' : selectedPart ? String(selectedPart.track) : '-1';
      trackLevel.hidden = !selectedPart;
      const representative = selectedPart ?? (groupSelected ? parts.find(p => midiRun?.active?.[p.track]) ?? parts[0] : null);
      const patch = representative && { ...window.SH101.defaults(role), ...representative.patch?.settings, ...representative.controls };
      for (const def of window.SH101.controls) {
        const id = role + '.' + def.id;
        if (sliders[id]) sliders[id].value = patch?.[def.id] ?? rig.getParam(id);
      }
      showPartLevel();
      showPartActivity();
      syncControls();
    }
    function showParts(next) {
      if (next.length === parts.length && next.every((part, i) => part === parts[i])) { showPartLevel(); return; }
      parts = next;
      partPicker.hidden = !parts.length;
      partSelect.replaceChildren(new Option('Весь канал ' + role.toUpperCase(), '-2'), new Option('Панель · MIDI/ROM', '-1'));
      for (const part of parts) partSelect.add(new Option((part.track + 1) + ' · ' + part.name, part.track));
      choosePart(-2);
    }
    partSelect.addEventListener('change', () => choosePart(Number(partSelect.value)));
    gain.addEventListener('input', () => {
      if (!selectedPart) return;
      selectedPart.levelDb = Number(gain.value);
      levelReadout.textContent = ' ' + gain.value + ' дБ';
      midiRun?.player?.level(selectedPart.track, selectedPart.levelDb);
    });
    const source = control('clock-source'), pulse = control('clock'), readout = control('readout');
    const arpHint = control('arp-hint'), arpExt = control('arp-ext');
    const power = control('power'), rateLED = control('lfo-led'), rateLabel = control('lfo-hz');
    const bender = control('bender'), lfoMod = control('bend-lfo');
    const fxMode = $('show-fx');
    const note = { bass: 36, lead: 60, arp: 72, fx: 36, pad: 60 }[role];
    const audition = bindSH101Audition(role, t => {
      if (!voice.external) return;
      if (fx) rig.fx.on(note, 100, t);
      else voice.send({ type: 'audition', note, t });
    });
    const leds = {}, releases = [];
    let board, player, available = false, wantedInput = 'midi', serviceMode = false;
    const button = (parent, label, cls = 'btn') => {
      const el = document.createElement('button');
      el.type = 'button';
      el.className = cls;
      el.textContent = label;
      parent.appendChild(el);
      return el;
    };
    const resume = () => startAudio()?.catch(() => { status.textContent = 'Браузер не запустил звук.'; });
    const bind = (el, change, latchable = false, keyboard = true) => {
      let pressedAt = 0, timer, latched = false, fromKeyboard = false;
      const set = (down) => { el.classList.toggle('rom-down', down); change(down); };
      const press = () => {
        clearTimeout(timer);
        pressedAt = performance.now();
        if (voice.powered && keyboard && !voice.useROM) selectInput('rom');
        set(true);
        if (voice.powered) resume();
      };
      const release = () => {
        if (latched) return;
        clearTimeout(timer);
        // Even a quick tap must survive a complete keyboard scan.
        timer = setTimeout(() => set(false), Math.max(0, 24 - (performance.now() - pressedAt)));
      };
      releases.push(() => {
        latched = fromKeyboard = false;
        clearTimeout(timer);
        if (el.classList.contains('rom-down')) set(false);
      });
      el.addEventListener('pointerdown', e => {
        if (e.button !== 0) return;
        if (latched) { latched = false; release(); return; }
        latched = latchable && e.shiftKey;
        el.setPointerCapture(e.pointerId);
        press();
      });
      el.addEventListener('lostpointercapture', release);
      el.addEventListener('pointerup', release);
      el.addEventListener('pointercancel', release);
      el.addEventListener('keydown', e => {
        if (e.key !== ' ' && e.key !== 'Enter') return;
        e.preventDefault();
        if (e.repeat) return;
        if (latched) { latched = false; release(); return; }
        fromKeyboard = true;
        press();
      });
      const releaseKeyboard = () => {
        if (!fromKeyboard) return;
        fromKeyboard = false;
        release();
      };
      el.addEventListener('keyup', e => { if (e.key === ' ' || e.key === 'Enter') releaseKeyboard(); });
      el.addEventListener('blur', releaseKeyboard);
      el.addEventListener('click', e => { if (e.detail === 0) { press(); release(); } });
    };
    const put = (word, mask, down) => down ? word | mask : word & ~mask;
    const keys = control('keys');
    for (const [parent, buttons] of [
      [control('sequencer'), { load: 'LOAD', play: 'PLAY' }],
      [control('arpeggio'), { down: 'DOWN', upDown: 'U&D', up: 'UP' }],
      [control('keyboard-functions'), { hold: 'HOLD', transpose: 'KEY TRANSPOSE' }],
    ]) {
      for (const [id, label] of Object.entries(buttons)) {
        const el = leds[id] = button(parent, label);
        el.setAttribute('aria-pressed', 'false');
        bind(el, down => {
          board.buttons = put(board.buttons, api.BUTTON[id], down);
          if (id === 'hold' && down) serviceMode = false;
        }, id === 'load' || id === 'hold' || id === 'transpose');
      }
    }
    const pedal = button(control('accessories'), 'Педаль HOLD');
    pedal.dataset.tip = 'Педаль на входе HOLD. Shift + клик удерживает педаль.';
    bind(pedal, down => { board.pedal = down; }, true);
    let white = 0;
    for (let i = 0; i < 32; i++) {
      const note = i + 53, name = NOTE[note % 12] + (Math.floor(note / 12) - 1);
      const accidental = name.includes('#');
      const el = button(keys, name, 'rom-key' + (accidental ? ' accidental' : ''));
      if (accidental) {
        el.style.left = (100 * white / 19) + '%';
        el.style.width = (64 / 19) + '%';
      } else white++;
      bind(el, down => { board.keys = put(board.keys, 1 << i, down); }, true);
    }
    bind(pulse, down => { board.clockHigh = down; });
    bind(lfoMod, down => { setVoiceControl(role, 'bendLfoPressed', Number(down)); }, false, false);
    const centerBender = () => {
      if (Number(bender.value) === 0.5) return;
      bender.value = 0.5;
      setVoiceControl(role, 'bender', 0.5);
    };
    bender.addEventListener('input', () => { resume(); setVoiceControl(role, 'bender', +bender.value); });
    bender.addEventListener('pointerdown', e => { if (e.button === 0) bender.setPointerCapture(e.pointerId); });
    for (const event of ['pointerup', 'pointercancel', 'lostpointercapture', 'blur']) {
      bender.addEventListener(event, centerBender);
    }
    bender.addEventListener('keyup', centerBender);
    source.addEventListener('change', () => {
      board.external = source.value === 'external';
      pulse.disabled = !voice.powered || !board.external;
      arpExt.hidden = !board.external;
    });
    const releaseAll = () => {
      for (const release of releases) release();
      centerBender();
    };
    window.addEventListener('blur', releaseAll);
    details.addEventListener('toggle', () => { if (!details.open) releaseAll(); });
    buildSH101Fields(role, control('voice-controls'), control('performance-controls'),
      control('accessories'), control('clock-indicator'));
    rateLabel.htmlFor.value = 'sh101-' + role + '-lfoRate';

    const inputs = [...Object.values(leds), ...keys.children, pedal, source];
    function syncControls() {
      const { powered, useROM } = voice;
      input.disabled = !available;
      romOption.disabled = !board;
      input.value = voice.input;
      inputBadge.textContent = groupSelected ? 'Все MIDI-партии' : selectedPart?.name ?? input.selectedOptions[0].textContent;
      power.setAttribute('aria-pressed', String(powered));
      power.textContent = powered ? 'ON' : 'OFF';
      for (const el of inputs) {
        const serviceSwitch = el === leds.load || el === leds.transpose;
        el.disabled = !board || !available || (!powered && !serviceSwitch);
      }
      pulse.disabled = !powered || !available || !board?.external;
      bender.disabled = lfoMod.disabled = !powered || !available;
      audition.hidden = groupSelected || !!selectedPart || voice.input !== 'midi';
      readout.hidden = !useROM;
      audition.disabled = !powered || !available;
      if (fx) $('sh101-fx-sequencer').hidden = groupSelected || !!selectedPart || voice.input !== 'midi';
    }

    function selectInput(value) {
      wantedInput = value;
      if (value === 'rom' && targets().length) choosePart(-1);
      if (!available || (value === 'rom' && !board) || voice.input === value) return;
      if (fx) rig.fx.kill(ctx.currentTime);
      if (value !== 'rom') releaseAll();
      voice.setInput(value);
      if (board) player = new api.Player(board, voice.send);
      if (value === 'cv') syncCV(cvPair);
      syncControls();
      update();
    }
    input.addEventListener('change', () => selectInput(input.value));

    power.addEventListener('click', () => {
      if (fx) rig.fx.kill(ctx.currentTime);
      voice.setPower(!voice.powered);
      syncControls();
      if (voice.powered) {
        if (board) {
          // The 80C49 reads LOAD and KEY TRANSPOSE during its service-mode boot.
          const held = board.buttons;
          const service = api.BUTTON.load | api.BUTTON.transpose;
          board.powerOn();
          board.buttons = held;
          player = new api.Player(board, voice.send);
          serviceMode = (held & service) === service;
          if (serviceMode) { selectInput('rom'); resume(); }
        }
        if (voice.input === 'cv') syncCV(cvPair);
        update();
      } else {
        releaseAll();
        arpHint.hidden = true;
        for (const el of Object.values(leds)) el.setAttribute('aria-pressed', 'false');
        rateLED.dataset.on = 'false';
        readout.textContent = 'KCV — · RANGE — · Gate 0';
      }
    });

    function update() {
      if (!board || !voice.powered) return;
      board.range = rig.getParam(role + '.range');
      board.octave = rig.getParam(role + '.transpose');
      board.trigger = rig.getParam(role + '.trigger');
      board.rate = window.SH101.lfoFrequency(rig.getParam(role + '.lfoRate'));
      const audible = voice.useROM && ctx.state === 'running';
      const s = player.tick(performance.now() / 1000, audible ? ctx.currentTime : null);
      if (!details.open) return;
      const showArpHint = !serviceMode && voice.useROM && (s.mode === 'up' || s.mode === 'down' || s.mode === 'upDown');
      if (arpHint.hidden === showArpHint) arpHint.hidden = !showArpHint;
      const lfoOn = String(board.phase < 0.5);
      if (rateLED.dataset.on !== lfoOn) rateLED.dataset.on = lfoOn;
      const hz = board.rate.toFixed(1) + ' Гц';
      if (rateLabel.textContent !== hz) rateLabel.textContent = hz;
      for (const [id, el] of Object.entries(leds)) {
        const on = String(s.mode === id || (id === 'hold' && s.hold) || (id === 'transpose' && s.transpose));
        if (el.getAttribute('aria-pressed') !== on) el.setAttribute('aria-pressed', on);
      }
      const value = 'KCV ' + (s.pitch / 12).toFixed(2) + ' В · RANGE ' + (s.range / 12).toFixed(2) + ' В · Gate ' + Number(s.gate);
      if (readout.textContent !== value) readout.textContent = value;
    }

    if (fx) {
      const riser = $('sh101-fx-riser-bars');
      const id = 'fx.riserBars', duration = rig.params.find(p => p.id === id);
      for (let bars = duration.min; bars <= duration.max; bars++) riser.add(new Option(bars, bars));
      riser.value = duration.value;
      riser.addEventListener('input', () => userSet(id, +riser.value));
      sliders[id] = riser;
      fxMode.addEventListener('change', () => { rig.fx.setSH101(fxMode.value === 'sh101'); syncControls(); });
    }
    const ready = mode => {
      available = mode !== 'none';
      if (fx) {
        fxMode.disabled = !available;
        if (!available) fxMode.value = 'original';
        rig.fx.setSH101(available && fxMode.value === 'sh101');
      } else if (!available) status.textContent = 'SH-101 недоступен в этом браузере.';
      syncControls();
      selectInput(wantedInput);
    };
    voice.ready.then(ready, () => ready('none'));
    return {
      selectInput, update, showParts, choosePart, showPartActivity,
      get input() { return wantedInput; }, get targets() { return targets(); },
      attach(next) {
        board = next;
        player = new api.Player(board, voice.send);
        syncControls();
        selectInput(wantedInput);
      },
    };
  }

  function loadSH101ROM(panels) {
    const api = window.SH101ROM, createCore = window.SH101Core, status = $('rom-status');
    if (!api || !createCore) { status.textContent = 'Эмулятор ROM 80C49 недоступен.'; return; }
    const file = $('rom-file'), fileLabel = $('rom-file-label');

    async function load(readImage) {
      if (file.disabled) return;
      file.disabled = true;
      status.textContent = 'Загружается ROM 80C49-6-7301…';
      try {
        const image = await readImage();
        const boards = await api.Board.loadAll(createCore, image, panels.length);
        panels.forEach((panel, i) => panel.attach(boards[i]));
        status.textContent = 'ROM 80C49-6-7301 загружен в ' + boards.length + ' SH-101';
        fileLabel.hidden = file.hidden = true;
        setInterval(() => { for (const panel of panels) panel.update(); }, 10);
      } catch (error) {
        status.textContent = error.message;
        fileLabel.hidden = file.hidden = false;
        file.disabled = false;
        file.value = '';
      }
    }

    file.addEventListener('change', () => { if (file.files.length) load(() => file.files[0].arrayBuffer()); });
    const loopback = ['127.0.0.1', 'localhost', '[::1]'].includes(location.hostname);
    const local = loopback && new URLSearchParams(location.search).get('sh101') === 'local';
    // Use the upstream dump at a fixed revision. The emulator checks its SHA-256.
    const romURL = local ? './sh101.rom'
      : 'https://raw.githubusercontent.com/joebritt/SH101_Disassembled/708cde01906312de7801204989628aa4610a4e45/80C49_SH101.bin';
    load(async () => {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), 6000);
      try {
        const response = await fetch(romURL, { signal: controller.signal });
        if (response.ok) return await response.arrayBuffer();
      } catch {
        // Network errors and timeouts both allow the user to pick the ROM.
      } finally {
        clearTimeout(timer);
      }
      throw new Error('Не удалось загрузить ROM автоматически. Выбери локальный образ.');
    });
  }

  function buildRigPanel() {
    buildMixer();
    if (!rig) {
      $('synth-status').textContent = 'Синтезаторы не загрузились.';
      return;
    }
    buildEffects();
    buildAcid('acid1', 2, 'ACID 1', 4);
    buildAcid('acid2', 3, 'ACID 2', 5);
    buildSH101();
  }
  function startAudio() {
    return ctx?.resume();
  }

  // The device runs slightly ahead of audio. Apply each change on the audio
  // clock using its pin/SPI timestamp, including changes while Gate stays high.
  const cvCodes = new Uint16Array(CV_ROLES.length);
  let gateMask = 0;
  function syncCV(pair, t = ctx?.currentTime) {
    const role = CV_ROLES[pair];
    rig?.sh101[role]?.cvGate(device.dacVolts(cvCodes[pair]), !!(gateMask & (1 << pair)), t);
  }
  function onCV(e) {
    if (e.type === 'dac') {
      cvCodes[e.channel] = e.code;
      if (sounding()) syncCV(e.channel, audioTime(e.t));
      return;
    }
    const changed = gateMask ^ e.mask;
    gateMask = e.mask;
    for (let pair = 0; pair < CV_ROLES.length; pair++) {
      const bit = 1 << pair;
      if (!(changed & bit)) continue;
      if (sounding()) syncCV(pair, audioTime(e.t));
      if ((gateMask & bit) && rig?.sh101[CV_ROLES[pair]]?.input === 'cv') {
        flashStrip(Artist.CV_TRACKS[pair], e.t);
      }
    }
  }

  function patchShow(song, importedPairs = []) {
    for (const { panel, input, priorInput, controls } of showPatch) {
      if (panel.input === input) panel.selectInput(priorInput);
      for (const { id, value, prior } of controls) {
        if (rig.getParam(id) === value) moveKnob(id, prior);
      }
    }
    showPatch = [];
    for (const [role, panel] of Object.entries(sh101Panels)) {
      if (!song?.cv && !importedPairs.includes(CV_ROLES.indexOf(role))) continue;
      const input = role === 'pad' ? 'midi' : 'cv';
      const controls = Object.entries(song?.cv ? CV_PATCH[role] || {} : {}).map(([name, value]) => {
        const id = role + '.' + name;
        return { id, value, prior: rig.getParam(id) };
      });
      showPatch.push({ panel, input, priorInput: panel.input, controls });
      for (const { id, value } of controls) moveKnob(id, value);
      panel.selectInput(input);
    }
  }

  function bootDevice() {
    device.boot(false);
    gateMask = device.gates();
    for (let pair = 0; pair < cvCodes.length; pair++) cvCodes[pair] = device.dac(pair);
  }

  // ─── MIDI wire → synths, monitor ──────────────────────────────
  const midiLines = [];
  let lastClock = 0, clockPeriod = 0, tempoSent = 0;
  const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  function flashStrip(strip, t) {
    lampHot[strip] = performance.now() + (t - playhead) / 1000 + 90;
  }
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
    if (m.status === 0xFC && sounding()) rig.allOff(audioTime(m.t), false);
    if (m.status >= 0xF0) {
      midiLines.push((m.t / 1e6).toFixed(3) + '  ' + ({ 0xFA: 'Start', 0xFC: 'Stop', 0xFB: 'Continue' }[m.status] || m.status.toString(16)));
      return;
    }
    const channel = m.status & 15, kind = m.status & 0xF0;
    if (sounding()) rig.midi(m.status, m.d1, m.d2, audioTime(m.t));
    if (kind === 0x90 && m.d2 > 0) {
      const inst = stripOf(channel, m.d1);
      if (inst !== undefined) flashStrip(inst, m.t);
      midiLines.push((m.t / 1e6).toFixed(3) + '  ch' + String(channel + 1).padStart(2) + '  ' +
        (NOTE[m.d1 % 12] + (Math.floor(m.d1 / 12) - 1)).padEnd(4) + ' vel ' + m.d2);
    }
  }

  // ─── MIDI keyboard: on-screen, computer keys and Web MIDI → the device's MIDI IN ───
  const down = { mouse: new Map(), kbd: new Map(), midi: new Map() };   // source -> key -> Note Off bytes
  const KBD_CODES = ['KeyZ', 'KeyS', 'KeyX', 'KeyD', 'KeyC', 'KeyV', 'KeyG', 'KeyB', 'KeyH', 'KeyN', 'KeyJ', 'KeyM',
                     'KeyQ', 'Digit2', 'KeyW', 'Digit3', 'KeyE', 'KeyR', 'Digit5', 'KeyT', 'Digit6', 'KeyY', 'Digit7', 'KeyU', 'KeyI'];
  const NO_MIDI = 'Web MIDI недоступен в этом браузере или для страницы, открытой с диска';
  let kbdBase = 48, kbdKeyEls = [], lampTimer = 0, mousePointer = -1;

  // The one place bytes reach the device, so an older core without midiIn just does nothing.
  // The device wrapper may be older than this page, and the firmware inside
  // may be one that never reads its DIN input.
  const hasMidiIn = () => !!device && typeof device.midiIn === 'function' &&
    (typeof device.midiInListening !== 'function' || device.midiInListening());
  function lightMidiLamp() {
    $('kbd-lamp').classList.add('on');
    clearTimeout(lampTimer);
    lampTimer = setTimeout(() => $('kbd-lamp').classList.remove('on'), 120);
  }
  function sendMidiIn(bytes) {
    if (!hasMidiIn()) return false;
    device.midiIn(bytes);
    lightMidiLamp();
    return true;
  }
  function press(src, key, on, off) {
    takeOver();         // before the note is registered: it may release everything held
    down[src].set(key, off);
    sendMidiIn(on);
    repaintKeys();
  }
  function release(src, key) {
    const off = down[src].get(key);
    if (!off) return;
    down[src].delete(key);
    sendMidiIn(off);
    repaintKeys();
  }
  function releaseSrc(src) { for (const key of [...down[src].keys()]) release(src, key); }
  function releaseKeys() { for (const src in down) releaseSrc(src); }
  function repaintKeys() {
    const on = new Set();
    for (const m of Object.values(down)) for (const off of m.values()) on.add(off[1]);
    for (const el of kbdKeyEls) el.classList.toggle('kdown', on.has(+el.dataset.n));
  }
  const velocity = () => +$('kbd-vel').value;

  function buildKeys() {
    const box = $('kbd-keys'), c = Math.floor(kbdBase / 12) - 1;
    box.replaceChildren();
    kbdKeyEls = [];
    let whites = 0;
    for (let i = 0; i <= 24; i++) {         // two octaves and the C above
      const n = kbdBase + i, white = [0, 2, 4, 5, 7, 9, 11].includes(i % 12);
      const el = document.createElement('div');
      el.className = white ? 'kw' : 'kb';
      el.dataset.n = n;
      Object.assign(el.style, white ? { left: whites * 100 / 15 + '%', width: 100 / 15 + '%' }
                                    : { left: whites * 100 / 15 - 2.2 + '%', width: '4.4%' });
      if (n % 12 === 0) el.textContent = 'C' + (n / 12 - 1);
      if (white) whites++;
      box.appendChild(el);
      kbdKeyEls.push(el);
    }
    $('kbd-range').textContent = 'C' + c + ' – C' + (c + 2);
    $('kbd-down').disabled = kbdBase <= 12;
    $('kbd-up').disabled = kbdBase >= 84;   // the top C of the range stays within C8
  }
  function shiftOctave(by) {
    releaseKeys();      // the keys' notes change under a held finger
    kbdBase = Math.max(12, Math.min(84, kbdBase + 12 * by));
    buildKeys();
  }
  function fromPort(e) {
    const d = e.data;
    if (!d || !d.length || d[0] === 0xF0 || d[0] >= 0xF8) return;    // no SysEx, no real-time
    const bytes = Array.from(d), kind = d[0] & 0xF0, key = (d[0] & 15) * 128 + d[1];
    if (kind === 0x90 && d[2] > 0) { press('midi', key, bytes, [0x80 | (d[0] & 15), d[1], 0]); return; }
    if (kind === 0x80 || kind === 0x90) { down.midi.delete(key); repaintKeys(); }
    sendMidiIn(bytes);
  }
  function connectMidi() {
    const status = $('kbd-status');
    let request;
    try { request = navigator.requestMIDIAccess && navigator.requestMIDIAccess(); } catch (error) { request = null; }
    if (!request) { status.textContent = NO_MIDI; return; }
    request.then(access => {
      const refresh = () => {
        const names = [];
        for (const port of access.inputs.values()) {
          port.onmidimessage = fromPort;      // also for inputs plugged in later
          if (port.state === 'connected') names.push(port.name);
        }
        status.textContent = names.length ? 'Подключено: ' + names.join(', ') : 'Нет подключённых MIDI-входов';
      };
      access.onstatechange = e => {
        if (e.port.type === 'input' && e.port.state !== 'connected') releaseSrc('midi');   // its Note Offs will never come
        refresh();
      };
      refresh();
    }, () => { status.textContent = NO_MIDI; });
  }
  function initKeyboard() {
    buildKeys();
    $('kbd-nofw').hidden = hasMidiIn();
    // The artist's score must match what the firmware inside can do. A firmware
    // that listens on its MIDI input also starts new steps at the last note and
    // copies clips; the score written for the older firmware would go wrong on it.
    const songs = $('show-song');
    Artist.SONGS.forEach((song, i) => {
      const option = document.createElement('option');
      option.value = i;
      option.textContent = '«' + song.title + '» — ' + song.style + ', ' + song.key + ', ' + song.bpm + ' BPM';
      songs.appendChild(option);
    });
    const requestedSong = new URLSearchParams(location.search).get('song');
    const songIndex = Artist.SONGS.findIndex(song => song.id === requestedSong);
    if (songIndex >= 0) songs.value = String(songIndex);
    const forms = $('show-form');
    const own = document.createElement('option');
    own.value = '';
    forms.appendChild(own);
    for (const form of Artist.FORMS) {
      const option = document.createElement('option');
      option.value = form.id;
      option.textContent = '«' + form.title + '»: ' + form.about;
      forms.appendChild(option);
    }
    const ownForm = () => {
      const midiSong = selectedMidiSong();
      showMidiTracks(midiSong);
      if (midiSong) { $('midi-library').open = true; return; }
      const form = Artist.FORMS.find(f => f.id === Artist.SONGS[+songs.value].form);
      own.textContent = 'как задумано для песни: «' + form.title + '»';
    };
    songs.addEventListener('change', ownForm);
    ownForm();
    const variants = $('show-variant');
    const can = device.features();
    const fits = { base: !can.lastNote, fw2: can.lastNote, keys: can.lastNote && can.midiIn, pads: can.lastNote && can.padKeys };
    for (const option of variants.options) option.disabled = !fits[option.value];
    variants.value = fits.pads ? 'pads' : fits.keys ? 'keys' : fits.fw2 ? 'fw2' : 'base';
    const box = $('kbd-keys');
    const glide = e => {
      const el = document.elementFromPoint(e.clientX, e.clientY), k = el && el.closest('[data-n]');
      const n = k && box.contains(k) ? +k.dataset.n : -1;
      if (down.mouse.has(n)) return;
      releaseSrc('mouse');
      if (n >= 0) press('mouse', n, [0x90, n, velocity()], [0x80, n, 0]);
    };
    box.addEventListener('pointerdown', e => {
      e.preventDefault();
      mousePointer = e.pointerId;
      box.setPointerCapture(e.pointerId);
      glide(e);
    });
    box.addEventListener('pointermove', e => { if (e.pointerId === mousePointer) glide(e); });
    const up = e => { if (e.pointerId === mousePointer) { mousePointer = -1; releaseSrc('mouse'); } };
    box.addEventListener('pointerup', up);
    box.addEventListener('pointercancel', up);
    $('kbd-down').addEventListener('click', e => { shiftOctave(-1); e.target.blur(); });
    $('kbd-up').addEventListener('click', e => { shiftOctave(1); e.target.blur(); });
    $('kbd-vel').addEventListener('input', () => { $('kbd-vel-n').textContent = velocity(); });
    $('kbd-vel').addEventListener('change', e => e.target.blur());   // else the slider keeps the keys from playing
    $('kbd-connect').addEventListener('click', connectMidi);
    document.addEventListener('keydown', e => {
      const off = KBD_CODES.indexOf(e.code);
      if (off < 0 || e.repeat || e.ctrlKey || e.metaKey || e.altKey || down.kbd.has(e.code) ||
          /^(INPUT|SELECT|TEXTAREA|BUTTON)$/.test(e.target.tagName)) return;
      const n = kbdBase + off;
      press('kbd', e.code, [0x90, n, velocity()], [0x80, n, 0]);
    });
    document.addEventListener('keyup', e => release('kbd', e.code));
    window.addEventListener('blur', releaseKeys);
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
      for (const e of device.drain()) {
        if (e.type === 'midi') onMidi(e);
        else onCV(e);
      }
    }
  }
  function tick() {
    const w = wall();
    // A tick that comes late (a busy page, a hidden tab) advances the device by
    // less than the look-ahead, so nothing it produces is already in the past:
    // the music slows for a moment instead of playing a bunch of notes at once.
    const dt = Math.min(Math.max(w - lastWall, 0), MAX_STEP_S);
    lastWall = w;
    if (midiRun?.player) {
      const { player, song, start } = midiRun;
      const now = ctx.currentTime;
      const until = (now + 0.075 - start) * 1e6;
      while (midiRun.tempo < song.tempos.length && song.tempos[midiRun.tempo].time <= until) {
        const segment = song.tempos[midiRun.tempo++];
        rig.setTempo(60000000 / segment.us, Math.max(now, start + segment.time / 1e6));
      }
      while (midiRun.cursor < song.events.length && song.events[midiRun.cursor].t <= until) {
        const e = song.events[midiRun.cursor++];
        player.midi(e, Math.max(now, start + e.t / 1e6));
        const on = MidiSongs.noteOn(e);
        if (on) midiRun.active[e.track]++;
        else if (MidiSongs.noteOff(e) && midiRun.active[e.track]) midiRun.active[e.track]--;
        const role = song.tracks[e.track].role;
        const strip = STRIPS.findIndex(s => s.id === role);
        if (on && strip >= 0) lampHot[strip] = performance.now() + 100;
      }
      if (now >= start + song.duration / 1e6 + 1) endShow('MIDI Song сыграна.');
    }
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
    const imported = selectedMidiSong();
    if (imported) { startMidiSong(imported); return; }
    startAudio();
    hands.clear();
    releaseAll();
    bootDevice();
    silence();      // a note the old device state left sounding gets no Note Off any more
    $('log').innerHTML = '';
    lastLed.fill('');
    const variant = $('show-variant').value;
    show = Artist.buildShow(undefined, { song: +$('show-song').value, form: $('show-form').value || undefined,
                                         fw2: variant !== 'base', keys: variant === 'keys', pads: variant === 'pads' });
    patchShow(show.song);
    Artist.schedule(show, device, hands, io);
    phase = 'prep';
    refreshMidiParts();
    speed = $('show-prep').checked ? +$('prep-speed').value : Infinity;
    playhead = 0;
    lastWall = wall();
    $('show-start').disabled = true;
    $('show-stop').disabled = false;
  }

  async function startMidiSong(song) {
    const request = ++launchSerial;
    const firmware = $('midi-song-mode').value === 'firmware';
    $('show-start').disabled = true;
    $('show-stop').disabled = false;
    try {
      await startAudio();
      if (request !== launchSerial) return;
      hands.clear();
      releaseAll();
      bootDevice();
      silence();
      patchShow();
      show = null;
      playhead = device.now() - LOOKAHEAD_US;
      speed = 1;
      lastWall = wall();
      $('log').replaceChildren();
      if (firmware) {
        if (!device.openMidiSong(firmwareMidiImage(song))) throw Error('Прошивка отклонила образ MIDI Song.');
        midiRun = { song, firmware, start: device.now() };
        phase = 'midi-firmware';
        refreshMidiParts();
        // PLAY, just as on the loaded hardware.
        device.play(true);
        device.play(false, device.now() + 80000);
        patchShow(null, song.tracks.flatMap(p => p.route?.cv === undefined ? [] : [p.route.cv]));
      } else {
        if (!rig) throw Error('Нет звукового движка.');
        phase = 'midi-loading';
        const player = await rig.midiParts(song);
        if (request !== launchSerial) { player.stop(ctx.currentTime); return; }
        midiRun = {
          song, player, cursor: 0, tempo: 0, active: new Uint16Array(song.tracks.length),
          start: ctx.currentTime + 0.1,
        };
        phase = 'midi-full';
        refreshMidiParts();
      }
    } catch (error) {
      if (request === launchSerial) endShow(error.message);
    }
  }
  function endShow(message) {
    launchSerial++;
    midiRun?.player?.stop(ctx.currentTime);
    midiRun = null;
    hands.clear();
    releaseAll();
    if (show?.song?.padNotes) rig?.sh101.pad.kill(ctx.currentTime);
    phase = 'idle';
    refreshMidiParts();
    speed = 1;
    playhead = device.now() - LOOKAHEAD_US;
    $('show-start').disabled = false;
    $('show-stop').disabled = true;
    if (message) say('info', message);
  }
  // A hand on the panel during the show takes it away from the artist.
  function takeOver() {
    if (phase === 'prep' || phase === 'live') endShow('Артист остановлен: управление у вас.');
  }
  function reboot() {
    if (phase !== 'idle') endShow();
    hands.clear();
    releaseAll();
    bootDevice();
    lastLed.fill('');
    playhead = device.now() - LOOKAHEAD_US;
    silence();
    patchShow();
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
    $('d-cv').textContent = CV_ROLES.map((_, i) => device.dacVolts(device.dac(i)).toFixed(2)).join(' ');
    $('midi').textContent = midiLines.join('\n');
    if (fullMidiSong()) {
      for (const panel of Object.values(sh101Panels)) panel.showPartActivity();
    }
    if (info.fatal) $('phase').textContent = 'Ошибка симулятора: ' + info.fatal;
    else if (phase === 'midi-loading') $('phase').textContent = 'Подготовка MIDI-дорожек…';
    else if (midiRun) {
      const { song, firmware, start } = midiRun;
      const microseconds = firmware ? Math.max(0, device.now() - start) : Math.max(0, (ctx.currentTime - start) * 1e6);
      if (firmware && microseconds > 150000 && info.state === 0) { endShow('MIDI Song остановлена.'); return; }
      $('phase').textContent = (firmware ? 'MIDI Song · ReBirth · ' : 'Полный MIDI · ') + song.title + ' · ' +
        (microseconds / 1e6).toFixed(0) + ' / ' + (song.duration / 1e6).toFixed(0) + ' с';
      $('progress').firstElementChild.style.width = Math.min(100, 100 * microseconds / song.duration) + '%';
    }
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
    for (const move of meters) move();
    document.querySelectorAll('#mixer .lamp[data-i]').forEach(el => {
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

  // ─── Hints ────────────────────────────────────────────────────
  // One floating box for every element that carries data-tip. It waits a
  // moment, so that it does not flicker under a pointer that only passes by,
  // and goes away on a press: a held pad must stay visible.
  // Which firmware this is: the commit sim/build.sh wrote into version.js.
  function showBuild() {
    const b = window.SIM_BUILD, el = $('build');
    if (!el) return;
    if (!b) { el.textContent = 'Версия прошивки неизвестна: нет version.js (соберите sim/build.sh web).'; return; }
    const day = b.date ? b.date.slice(0, 10) : '';
    el.textContent = 'Прошивка: коммит ' + b.short + (day ? ' от ' + day : '') + (b.dirty ? ', с незакоммиченными правками' : '') + '.';
    el.dataset.tip = (b.subject ? '«' + b.subject + '»\n' : '') + 'Полный коммит ' + b.commit + '. Emscripten ' + b.emscripten + '.' +
      (location.protocol.startsWith('http') && !/^(localhost|127\.)/.test(location.hostname)
        ? '\nИсходники этой сборки (GPL-3.0) лежат рядом со страницей, в папке source/.' : '');
  }

  function initTips() {
    const tip = $('tip');
    let over = null, timer = 0;
    const hide = () => { clearTimeout(timer); over = null; tip.hidden = true; };
    const place = el => {
      tip.textContent = el.dataset.tip;
      tip.hidden = false;
      const r = el.getBoundingClientRect(), w = tip.offsetWidth, h = tip.offsetHeight;
      const below = r.bottom + 8 + h <= window.innerHeight || r.top - 8 - h < 0;
      tip.style.left = Math.max(8, Math.min(r.left + r.width / 2 - w / 2, window.innerWidth - w - 8)) + 'px';
      tip.style.top = (below ? r.bottom + 8 : r.top - 8 - h) + 'px';
    };
    document.addEventListener('pointerover', e => {
      const el = e.target.closest ? e.target.closest('[data-tip]') : null;
      if (el === over) return;
      hide();
      if (!el || e.pointerType === 'touch') return;
      over = el;
      timer = setTimeout(() => place(el), 400);
    });
    document.addEventListener('pointerout', e => { if (!e.relatedTarget) hide(); });
    document.addEventListener('pointerdown', hide, true);
    document.addEventListener('keydown', hide, true);
    window.addEventListener('scroll', hide);     // the page itself, not the log, which scrolls all through a set
    window.addEventListener('blur', hide);
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
      ctx.addEventListener('statechange', () => {
        if (sounding()) CV_ROLES.forEach((_, pair) => syncCV(pair));
      });
    } catch (error) {
      console.error(error);
    }
    buildRigPanel();
    initKeyboard();
    initMidiLibrary();
    initTips();
    showBuild();

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
      if (e.code !== 'Space' || e.repeat || ['BUTTON', 'INPUT', 'SELECT', 'TEXTAREA', 'A'].includes(e.target.tagName)) return;
      e.preventDefault();
      takeOver();
      device.play(true);
      device.play(false, device.now() + 80000);
    });

    lastWall = wall();
    setInterval(tick, 10);
    requestAnimationFrame(frame);
    window.sim = { device, hands, get rig() { return rig; }, get ctx() { return ctx; }, get phase() { return phase; },
                   get show() { return show; }, startShow, endShow, sendMidiIn };
  });
})();
