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
        pad.dataset.tip = 'Пэд: клип дорожки T' + (track + 1) + ' в ряду ' + (row + 1) + '.\n' +
          'На остановленном приборе клип выбирается сразу, во время игры встаёт в очередь до границы квантования (мигает). ' +
          'Ярко светится играющий клип, тускло — клип с шагами. Пустой пэд тоже запускается: дорожка замолкает.';
      }
      const scene = box('scene', 1040, PAD_Y(row) + 7, 65, 65);
      els.scene[row] = scene;
      bindHold(scene, down => device.scene(row, down));
      scene.dataset.tip = 'SCENE ' + (row + 1) + ': запускает ряд ' + (row + 1) + ' на всех восьми дорожках сразу. ' +
        'Пока сцена ждёт в очереди, повторное нажатие отменяет все очереди.';
    }
    label('MUTE', 236, 1131);
    for (let track = 0; track < 8; track++) {
      const key = box('mutekey', PAD_X(track), 1113, 80, 37);
      els.mute[track] = key;
      bindHold(key, down => device.mute(track, down));
      key.dataset.tip = 'MUTE T' + (track + 1) + ': выключает и возвращает дорожку сразу, без ожидания границы такта. Светится, пока дорожка слышна.';
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
      'Session: поворот — дорожка или линия KIT; нажать — экран Pattern; удержать — следующий банк A-D.\n' +
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
  };

  // ─── Synths ───────────────────────────────────────────────────
  // The mixer's strips: a name, the rig knob that is its fader, and the lamp
  // that shows the device sending it a note (the kick and the kit share
  // channel 10; the kick is note 36 there).
  const STRIPS = [
    { name: 'KICK', id: 'kick' }, { name: '909', id: 'drums' }, { name: 'BASS', id: 'bass' },
    { name: 'ACID 1', id: 'acid1', ownFx: true }, { name: 'ACID 2', id: 'acid2', ownFx: true }, { name: 'LEAD', id: 'lead' },
    { name: 'ARP', id: 'arp' }, { name: 'FX', id: 'fx' }, { name: 'MASTER', level: 'master.volume' },
  ];
  for (const s of STRIPS) if (s.id) s.level = s.id + '.level';
  const stripOf = (channel, note) => channel === 9 ? (note === 36 ? 0 : 1) : channel >= 2 && channel <= 7 ? channel : undefined;
  // The other synths' knobs that stay as plain sliders.
  const KNOBS = ['bass.cutoff', 'lead.cutoff', 'delay.feedback', 'delay.level', 'reverb.level'];
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
    delay: 'Delay: сколько сигнала уходит в пинг-понг дилей.',
    time: 'Time: время дилея в долях такта, по темпу прибора (MIDI Clock).',
    fdbk: 'Feedback: сколько раз повторяется эхо.',
    space: 'Space: сколько сигнала уходит в ревербератор.',
    size: 'Size: длина хвоста ревербератора, от 1,2 до 12 секунд.',
    tone: 'Tone: яркость хвоста ревербератора.',
    feedback: 'Обратная связь общей задержки: сколько раз повторяется эхо.',
    level: 'Уровень в общем миксе.',
  };
  const NOT_DEVICE = '\nЭто ручка синтезатора, не прибора: прибор CC не шлёт.';
  const lampHot = new Array(8).fill(0);
  const sliders = {};             // rig knob id -> something with a `value`: a range input or a drawn knob
  const meters = [];              // what drawStatus keeps moving: () => void

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
  const userSet = (id, value) => { delete anims[id]; rig.setParam(id, value); };

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
  function benchKnob(container, id, label) {
    const p = rig.params.find(x => x.id === id), name = id.split('.')[1], format = window.Rig.format[name];
    const cell = document.createElement('div');
    cell.className = 'bcell';
    cell.dataset.tip = KNOB_TIP[name] + '\nТянуть вверх и вниз (с Shift точнее), стрелки на клавиатуре, двойной клик — исходное положение.' + NOT_DEVICE;
    cell.innerHTML = `<div class="bknob" role="slider" tabindex="0" aria-label="${p.label}" aria-valuemin="0" aria-valuemax="10">${knobSVG()}</div>
      <span class="bval"></span><span class="legend">${label || p.label}</span>`;
    container.appendChild(cell);
    const el = cell.querySelector('.bknob'), shown = cell.querySelector('.bval'), def = p.value;
    let v = p.value;
    const draw = () => {
      el.querySelector('.ptr').setAttribute('transform', `rotate(${START + SWEEP * v} 50 50)`);
      el.querySelector('.arc').setAttribute('d', arcPath(v));
      const text = format ? format(v) : (v * 10).toFixed(1);
      el.setAttribute('aria-valuenow', (v * 10).toFixed(1));
      el.setAttribute('aria-valuetext', text);
      shown.textContent = text;
    };
    handleKnob(el, id, () => v, (to) => { v = to; draw(); }, def);
  }

  // What every drawn knob does: drag up and down (Shift for fine), arrow and
  // Page keys, Home and End, double click for the default. `get` reads the
  // knob, `show` draws a new value; the rig is set here.
  function handleKnob(el, id, get, show, def) {
    const clamp = (x) => Math.min(1, Math.max(0, x));
    const turn = (to) => { show(clamp(to)); userSet(id, get()); };
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
    sliders[id] = { get value() { return get(); }, set value(to) { show(clamp(+to)); } };
  }

  // A small knob on a mixer strip. A pan knob draws its arc from the centre.
  const MIX_KNOB = {
    delay: { label: 'DLY', tip: (s) => 'Посыл канала «' + s + '» в общую задержку (пинг-понг по темпу прибора).',
             text: (v) => (v * 10).toFixed(1) },
    reverb: { label: 'REV', tip: (s) => 'Посыл канала «' + s + '» в общий ревербератор.', text: (v) => (v * 10).toFixed(1) },
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
      <section class="board" aria-label="${title}: effects">
        <div class="bhead"><span class="legend">Effects</span>
          <span class="small">Дисторшн, дилей и ревербератор со страницы Bench. В самом x0xb0x их нет.</span></div>
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
    for (const [name, label] of [['drive'], ['color'], ['delay'], ['time'], ['fdbk', 'Feed­back'], ['space'], ['size'], ['tone']]) {
      benchKnob(effects, id + '.' + name, label);
    }
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

  function buildMixer() {
    const box = $('mixer');
    box.innerHTML = '';
    STRIPS.forEach((strip, i) => {
      const p = rig && rig.params.find(x => x.id === strip.level), master = i === STRIPS.length - 1;
      const el = document.createElement('div');
      el.className = 'strip' + (master ? ' master' : '');
      el.innerHTML = `<span class="lamp">${strip.name}</span><div class="mknobs"></div><div class="well"><input class="fader" type="range" aria-label="${strip.name}">` +
        (master ? '<div class="vu"><i></i></div>' : '') + `</div><span class="val"></span>` +
        (master ? '' : '<button type="button" aria-pressed="false">M</button>');
      box.appendChild(el);
      const lamp = el.querySelector('.lamp'), fader = el.querySelector('.fader'), val = el.querySelector('.val'), mute = el.querySelector('button');
      if (!master) lamp.dataset.i = i;
      lamp.dataset.tip = master ? 'Общий выход всех синтезаторов.'
        : 'Вспыхивает, когда прибор шлёт ноту этому инструменту.' + (strip.name.startsWith('ACID') ? ' Это модель x0xb0x (схема TB-303) из x0xb0x VCF Bench.' : '');
      if (!p) { fader.disabled = true; return; }
      // Sends, then pan, as on a console; a 303 has its own effects, so only a pan.
      const knobs = el.querySelector('.mknobs');
      for (const name of ['delay', 'reverb', 'pan']) {
        const id = strip.id && strip.id + '.' + name;
        // (a 303's `delay` is the Delay of its own effects, on its own board)
        if (id && !(strip.ownFx && name !== 'pan') && rig.params.some(x => x.id === id)) mixKnob(knobs, id, strip.name);
        else knobs.appendChild(Object.assign(document.createElement('div'), { className: 'mknob empty' }));
      }
      fader.min = p.min; fader.max = p.max; fader.step = (p.max - p.min) / 100; fader.value = p.value;
      fader.dataset.tip = (master ? 'Общая громкость.' : 'Уровень канала «' + strip.name + '» в общем миксе.') + NOT_DEVICE;
      const show = () => { val.textContent = (10 * fader.value / p.max).toFixed(1); };
      let saved = null;           // the level a muted strip returns to
      const unmute = () => { saved = null; el.classList.remove('muted'); if (mute) mute.setAttribute('aria-pressed', 'false'); };
      fader.addEventListener('input', () => { unmute(); userSet(strip.level, +fader.value); show(); });
      if (mute) {
        mute.dataset.tip = 'Заглушить канал в микшере страницы. Это не MUTE прибора: прибор продолжает слать ноты.';
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
        const vu = el.querySelector('.vu i'), samples = new Float32Array(rig.analyser.fftSize);
        el.querySelector('.vu').dataset.tip = 'Пик на выходе, от −40 дБ внизу до полной шкалы вверху.';
        let held = 0;
        meters.push(() => {
          let peak = 0;
          if (ctx && ctx.state === 'running') {
            rig.analyser.getFloatTimeDomainData(samples);
            for (let k = 0; k < samples.length; k++) { const a = Math.abs(samples[k]); if (a > peak) peak = a; }
          }
          held = Math.max(peak, held * 0.9);      // falls back by about 1 dB a frame
          const db = 20 * Math.log10(Math.max(held, 1e-4));
          vu.style.height = Math.min(100, Math.max(0, (db + 40) * 2.5)) + '%';
          vu.style.background = held > 0.89 ? 'var(--warn)' : 'var(--accent)';
        });
      }
    });
  }

  function buildRigPanel() {
    buildMixer();
    if (!rig) {
      $('knobs').textContent = 'Синтезаторы не загрузились (rig.js).';
      return;
    }
    buildAcid('acid1', 2, 'ACID 1', 4);
    buildAcid('acid2', 3, 'ACID 2', 5);
    const byId = {};
    for (const p of rig.params) byId[p.id] = p;
    for (const id of KNOBS) {
      const p = byId[id];
      if (!p) continue;
      const wrap = document.createElement('label');
      wrap.textContent = (p.group ? p.group + ' · ' : '') + (p.label || id);
      wrap.dataset.tip = (id.endsWith('.delay') ? 'Сколько сигнала инструмента уходит в общую задержку.' : KNOB_TIP[id.split('.')[1]]) + NOT_DEVICE;
      const input = document.createElement('input');
      input.type = 'range'; input.min = p.min; input.max = p.max; input.step = (p.max - p.min) / 200; input.value = p.value;
      input.addEventListener('input', () => userSet(id, +input.value));
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
      const inst = stripOf(channel, m.d1);
      if (inst !== undefined) lampHot[inst] = performance.now() + (m.t - playhead) / 1000 + 90;
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
    const variant = $('show-variant').value;
    show = Artist.buildShow(undefined, { song: +$('show-song').value, form: $('show-form').value || undefined,
                                         fw2: variant !== 'base', keys: variant === 'keys', pads: variant === 'pads' });
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
    } catch (error) {
      console.error(error);
    }
    buildRigPanel();
    initKeyboard();
    initTips();

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
