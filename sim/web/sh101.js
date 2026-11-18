// SH-101 voice, from Roland Service Notes (1982), pp. 1, 2, 7. The
// AudioWorklet, file:// fallback and offline checks share this implementation.
(function (root) {
  'use strict';

  const REBIRTH_A4 = 440;
  const ROLAND_A4 = 442;  // Service Notes p. 4, VCO TUNE with the panel TUNE centered.
  const groups = [
    ['TUNE', [
      { id: 'tune', label: 'Tune', value: 0.5, dial: true },
      { id: 'pitchReference', label: 'Строй A4', value: 0, accessory: true,
        options: [REBIRTH_A4 + ' Гц · ReBirth', ROLAND_A4 + ' Гц · Roland'],
        tip: 'Задаёт опорную частоту этого VCO. Для заводской проверки Roland выбери 442 Гц и поставь TUNE в центр.' },
    ]],
    ['MODULATOR', [
      { id: 'lfoRate', label: 'LFO/CLK RATE', face: 'RATE', value: 0.47,
        tip: 'Скорость LFO от 0,1 до 30 Гц. На ROM-панели LFO также задаёт внутренний такт арпеджио и секвенсора.' },
      { id: 'lfoWave', label: 'WAVE FORM', face: 'WAVE', dial: true, value: 0, options: ['Triangle', 'Square', 'Random', 'Noise'],
        marks: ['TRI', 'SQR', 'RND', 'NOISE'],
        tip: 'Форма сигнала для VCO MOD и MOD VCF. PWM и рычаг BENDER используют треугольник.' },
    ]],
    ['VCO', [
      { id: 'vcoMod', label: 'VCO MOD', face: 'MOD', value: 0, tip: 'Глубина модуляции высоты тона выбранной формой LFO.' },
      { id: 'range', label: 'Диапазон', face: 'RANGE', dial: true, value: 1, options: ['16′', '8′', '4′', '2′'] },
      { id: 'pwm', label: 'Pulse width', face: 'P.W.', value: 0.38,
        tip: 'Ширина импульса в MAN и глубина её модуляции в ENV и LFO. Слышно при поднятом Pulse.' },
      { id: 'pwmMode', label: 'Источник PWM', face: 'PWM', value: 2, options: ['ENV', 'MAN', 'LFO'], order: [2, 1, 0],
        tip: 'Ширину импульса задаёт огибающая, ползунок Pulse width или треугольник LFO.' },
    ]],
    ['SOURCE MIXER', [
      { id: 'pulse', label: 'Pulse', value: 0.32 },
      { id: 'saw', label: 'Saw', value: 0.9 },
      { id: 'sub', label: 'Sub', value: 0.2 },
      { id: 'subMode', label: 'Форма Sub', face: 'MODE', value: 0, options: ['−1 октава, 50%', '−2 октава, 50%', '−2 октавы, узкая'],
        marks: ['−1', '−2', '−2 ▏'] },
      { id: 'noise', label: 'Noise', value: 0 },
    ]],
    ['VCF · IR3109', [
      { id: 'cutoff', label: 'FREQ', value: 0.54 },
      { id: 'resonance', label: 'RES', value: 0.25 },
      { id: 'diodes', label: 'D30/D31', value: 0, accessory: true, options: ['1S2473 · Roland', '1N4148 · Rev-A'],
        tip: 'Диоды в петле резонанса IR3109. Разница лучше слышна при высоком RES.' },
      { id: 'envMod', label: 'ENV VCF', face: 'ENV', value: 0.32 },
      { id: 'vcfMod', label: 'MOD VCF', face: 'MOD', value: 0, tip: 'Глубина модуляции частоты среза выбранной формой LFO.' },
      { id: 'keyFollow', label: 'KYBD', value: 0.5 },
    ]],
    ['VCA', [
      { id: 'vca', label: 'Режим VCA', face: 'MODE', value: 0, options: ['ENV', 'GATE'] },
    ]],
    ['ENV', [
      { id: 'trigger', label: 'Запуск огибающей', face: 'TRIG', value: 0, options: ['GATE + TRIG', 'GATE', 'LFO'],
        marks: ['G+T', 'GATE', 'LFO'],
        tip: 'GATE + TRIG запускает атаку на каждой ноте. GATE сохраняет огибающую в легато. LFO повторяет её, пока клавиша нажата.' },
      { id: 'attack', label: 'Attack', face: 'A', value: 0.1 },
      { id: 'decay', label: 'Decay', face: 'D', value: 0.51 },
      { id: 'sustain', label: 'Sustain', face: 'S', value: 0.65 },
      { id: 'release', label: 'Release', face: 'R', value: 0.49 },
    ]],
    ['CONTROLLERS', [
      { id: 'volume', label: 'VOLUME', face: 'VOL', dial: true, value: 0.75 },
      { id: 'portaTime', label: 'PORTAMENTO', face: 'PORTA', dial: true, value: 0.14 },
      { id: 'portaMode', label: 'Режим портаменто', face: 'MODE', value: 1, options: ['OFF', 'AUTO', 'ON'],
        order: [1, 0, 2], horizontal: true },
      { id: 'transpose', label: 'TRANSPOSE', face: 'TRANS', value: 1, options: ['L', 'M', 'H'], horizontal: true },
    ]],
    ['BENDER', [
      { id: 'bendVco', label: 'VCO BEND SENS', face: 'VCO', value: 1, tip: 'Диапазон рычага по высоте до ±12 полутонов.' },
      { id: 'bendVcf', label: 'VCF BEND SENS', face: 'VCF', value: 0, tip: 'Глубина воздействия рычага на частоту среза фильтра.' },
      { id: 'bendLfo', label: 'LFO MOD DEPTH', face: 'LFO', value: 0.25, tip: 'Глубина модуляции VCO и VCF треугольником при удержании LFO MOD.' },
    ]],
  ];
  const controls = [
    ...groups.flatMap(([, entries]) => entries),
    { id: 'bender', label: 'BENDER', value: 0.5 },
    { id: 'bendLfoPressed', label: 'LFO MOD', value: 0 },
  ];
  function defaults(role) {
    const p = Object.fromEntries(controls.map(({ id, value }) => [id, value]));
    if (role === 'bass') Object.assign(p, {
      saw: 0.8, sub: 0.56, pulse: 0.15, pwmMode: 1, pwm: 0.18, cutoff: 0.49,
      resonance: 0.38, envMod: 0.36, attack: 0, decay: 0.48, sustain: 0.48,
      release: 0.34, portaTime: 0.12,
    });
    if (role === 'arp') Object.assign(p, {
      saw: 0.48, pulse: 0.58, sub: 0, pwmMode: 1, pwm: 0.05, cutoff: 0.59,
      resonance: 0.32, envMod: 0.4, keyFollow: 0.25, attack: 0, decay: 0.35,
      sustain: 0.13, release: 0.32, portaTime: 0.095,
    });
    if (role === 'fx') Object.assign(p, {
      saw: 0.62, pulse: 0.25, sub: 0, noise: 0.8, resonance: 0.63, vca: 1,
      vcfMod: 0, vcoMod: 0, keyFollow: 0, envMod: 0,
    });
    if (role === 'pad') Object.assign(p, {
      saw: 0.58, pulse: 0.58, sub: 0.08, pwm: 0.46, lfoRate: 0.23, vcfMod: 0.07,
      cutoff: 0.51, resonance: 0.29, envMod: 0.21, attack: 0.64,
      decay: 0.59, sustain: 0.68, release: 0.72,
    });
    if (role === 'rom') p.diodes = 1;
    return p;
  }
  const max = (def) => def.options ? def.options.length - 1 : 1;

  const TAU = 2 * Math.PI;
  const MIN_CUTOFF = 10, MAX_CUTOFF = 20000;
  const cutoffPosition = (hz) => Math.log(hz / MIN_CUTOFF) / Math.log(MAX_CUTOFF / MIN_CUTOFF);
  const clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));
  const wrap = (phase) => phase - Math.floor(phase);
  const rc = (seconds, rate) => -Math.expm1(-1 / (rate * seconds));
  const sweep = (from, to, position) => from * (to / from) ** position;
  const lfoFrequency = (position) => sweep(0.1, 30, position);
  const VIRTUAL_GROUND = 5;
  const CONTROL_RAIL = 14;
  const THERMAL_VOLTAGE = 0.02569;  // 25 °C
  const ENV_PEAK = (CONTROL_RAIL - VIRTUAL_GROUND) * 100 / (82 + 100);  // R158/VR24
  const CHARGE_RAIL = (CONTROL_RAIL - VIRTUAL_GROUND) / ENV_PEAK;
  const ATTACK_THRESHOLD = -Math.log1p(-1 / CHARGE_RAIL);
  const T90 = Math.log(10);
  // Bender Board OPH178-1. The switched LFO feeds VCO and VCF separately
  // from the horizontal bend. Use a nominal AC swing at VR6.
  const BENDER_LFO_PEAK = 0.5;

  // Service Notes p. 7: C47/C48/C50/C51, 68k feedback and 560-ohm
  // attenuators. Pin 1 is the +5 V signal reference on this board.
  const STAGE_CAP = 240e-12;
  const STAGE_R = 68000;
  const INPUT_DIVIDER = 560 / (STAGE_R + 2 * 560);
  const FEEDBACK_R = 15000;  // R113
  const DIODE_R = 5600;     // R129
  const FEEDBACK_CAP = 10e-6;  // C52
  const FEEDBACK_SCALE = STAGE_R / FEEDBACK_R;
  // Nominal small-signal TR26 collector/emitter ratio and TR27 follower.
  const RES_GAIN = 15000 / 10000;
  // Four IR3109 OTA/buffer sections with their external local feedback.
  // With alternating signs absorbed into the stage voltages, the open loop
  // is the product of four one-pole sections. Solve the diode branch in the
  // same trapezoidal step as the four stages to retain the analog loop phase.
  class Filter {
    constructor(rate, diodes = 0) {
      this.rate = rate;
      this.z1 = this.z2 = this.z3 = this.z4 = this.cap = 0;
      this.r = DIODE_R + 0.5 / (rate * FEEDBACK_CAP);
      this.capRate = 1 / (rate * FEEDBACK_CAP * this.r);
      this.lastHz = -1;
      this.setDiodes(diodes);
    }

    setDiodes(diodes) {
      if (this.diodes === diodes) return;
      this.diodes = diodes;
      // D30/D31: Roland's 1S2473 or Rev-A's 1N4148. The latter fits
      // Vishay 81857 Fig. 2 at 25 °C and 10 µA–1 mA.
      this.diodeScale = this.r * (diodes === 1 ? 5.05e-9 : 5.5e-9);
      this.diodeVoltage = diodes === 1 ? 0.04994 : 2 * THERMAL_VOLTAGE;
    }

    step(input, hz, resonance) {
      if (hz !== this.lastHz) {
        // Roland specifies 1 µS to 10 mS for the OTA. The panel frequency
        // sets gm through the shared exponential converter.
        const gm = clamp(TAU * hz * STAGE_CAP / INPUT_DIVIDER, 1e-6, 1e-2);
        const pole = gm * INPUT_DIVIDER / (TAU * STAGE_CAP);
        const g = Math.tan(Math.PI * Math.min(pole, this.rate * 0.43) / this.rate);
        this.g = g / (1 + g);
        this.lastHz = hz;
      }
      const g = this.g, s = 1 - g;
      const { z1, z2, z3, z4 } = this;
      const free = g * (g * (g * (g * input + s * z1) + s * z2) + s * z3) + s * z4;
      const amp = RES_GAIN * resonance;
      const drive = amp * free - this.cap;
      let diode = 0;
      if (drive !== 0) {
        // R129 feeds the antiparallel D30/D31 pair and R113. The voltage
        // source is itself a function of this sample's fourth-stage output.
        const linear = 1 + this.r / FEEDBACK_R + amp * FEEDBACK_SCALE * g ** 4;
        const target = Math.abs(drive), scale = this.diodeScale, nvt = this.diodeVoltage;
        // Both initial estimates overbound the root. Newton then approaches
        // the single solution from above, including in deep clipping.
        diode = Math.min(target / linear, nvt * Math.asinh(target / (2 * scale)));
        for (let iteration = 0; iteration < 4; iteration++) {
          const exp = Math.exp(diode / nvt), inv = 1 / exp;
          diode -= (linear * diode + scale * (exp - inv) - target) /
            (linear + scale * (exp + inv) / nvt);
        }
        diode *= Math.sign(drive);
      }
      const y1 = g * (input - FEEDBACK_SCALE * diode) + s * z1;
      const y2 = g * y1 + s * z2;
      const y3 = g * y2 + s * z3;
      const y4 = g * y3 + s * z4;
      this.z1 = 2 * y1 - z1;
      this.z2 = 2 * y2 - z2;
      this.z3 = 2 * y3 - z3;
      this.z4 = 2 * y4 - z4;
      this.cap += this.capRate * (amp * y4 - this.cap - diode);
      return y4;
    }
  }

  function blep(phase, advance) {
    if (phase < advance) {
      const t = phase / advance;
      return 2 * t - t * t - 1;
    }
    if (phase > 1 - advance) {
      const t = (phase - 1) / advance;
      return t * t + 2 * t + 1;
    }
    return 0;
  }

  function pulseAt(phase, advance, width) {
    return (phase < width ? 1 : 0) +
      0.5 * (blep(phase, advance) - blep(wrap(phase - width), advance));
  }

  // VR13..16 are 100k linear pots. Source impedance includes the 68k
  // pulse pull-up and 47k sub divider resistors. R109..112 load the wipers.
  function mixLeg(level, sourceR, mixR) {
    const lower = level * 100000, upper = sourceR + 100000 - lower;
    const open = lower / (lower + upper);
    const resistance = mixR + upper * open;
    return { scale: STAGE_R * open / resistance, resistance };
  }

  class Voice {
    constructor(fs, effects = false, rom = false) {
      this.fs = fs;
      this.rate = fs * 2;
      this.effects = effects;
      this.rom = rom;
      this.cvInput = false;
      this.effect = null;
      this.filter = new Filter(this.rate, +rom);
      this.queue = [];
      this.keys = [];
      this.cv = this.note = 48;
      this.env = this.amp = 0;
      this.envStage = 'release';
      this.phase = this.lfoPhase = 0;
      this.noise = this.random = this.mixDC = this.vcaDC = 0;
      this.width = 0.5;
      this.glide = false;
      this.bend = 0;
      this.midiMod = 0;
      this.velocityGain = 1;
      this.seed = 0x53483101;
      this.auditionEnd = Infinity;
      this.trigger = 0;
      this.controlGate = this.envelopeGate = this.romReset = false;
      this.romRange = this.lfoStep = 0;
      this.gateRate = rc(0.00055, this.rate);
      this.pwmRate = rc((100000 / 2) * 10e-9, this.rate);  // R94/R96 and C37
      this.noiseRate = rc(220000 * 240e-12, this.rate);   // R86 and C32
      this.vcaDCRate = rc((150000 / 2 + 1000) * 10e-6, this.rate);  // C53
    }

    white() {
      let s = this.seed;
      s ^= s << 13;
      s ^= s >>> 17;
      s ^= s << 5;
      this.seed = s;
      return (s >>> 0) / 0x80000000 - 1;
    }

    enqueue(event) {
      const q = this.queue;
      if (event.type === 'kill' || event.type === 'source') {
        for (let i = q.length - 1; i >= 0 && q[i].t >= event.t; i--) {
          // A clock resync can kill notes at the same time as an input change.
          if (event.type === 'source' || q[i].type !== 'source') q.splice(i, 1);
        }
      }
      let i = q.length;
      while (i > 0 && q[i - 1].t > event.t) i--;
      q.splice(i, 0, event);
    }

    key(key, down) {
      const keys = this.keys, wasHeld = keys.length > 0;
      const i = keys.indexOf(key);
      if (down) keys.push(key);
      else if (i >= 0) keys.splice(i, 1);
      else return;
      if (!keys.length) {
        this.envStage = 'release';
        return;
      }
      // Owner's Manual p. 29: last priority in GATE+TRIG, lowest in
      // GATE and LFO. The special audition key has its own identity.
      this.chooseNote();
      this.glide = wasHeld;
      if (!wasHeld || (down && this.trigger === 0)) {
        // The trigger switches C59 to its charging path; its voltage is
        // continuous even when a second note arrives during the first.
        this.envStage = 'attack';
        if (this.trigger === 2) this.lfoPhase = 0;
      }
    }

    chooseNote() {
      this.note = this.trigger === 0 ? this.keys[this.keys.length - 1] & 127
        : Math.min(...this.keys.map(key => key & 127));
    }

    event(e, t) {
      switch (e.type) {
        case 'note':
          if (!this.rom && !this.cvInput) {
            this.key(e.note, e.down);
            if (e.down) this.velocityGain = e.gain ?? 1;
          }
          break;
        case 'bend': if (!this.rom && !this.cvInput) this.bend = e.value; break;
        case 'mod': this.midiMod = e.value; break;
        case 'audition':
          if (this.rom || this.cvInput) break;
          if (this.auditionEnd !== Infinity) this.key(this.auditionKey, false);
          this.auditionKey = 128 + e.note;
          this.key(this.auditionKey, true);
          this.auditionEnd = t + 0.65;
          break;
        case 'sweep':
          if (this.effects && !this.rom && !this.cvInput) this.effect = { start: t, duration: e.duration, rise: e.rise };
          break;
        case 'cv': {
          if (!this.cvInput) break;
          // 1 V/oct at the external jack. F3 at nominal RANGE 8′ is 17/12 V.
          const note = 36 + 12 * e.volts;
          this.glide = this.controlGate && e.gate;
          if (e.gate && (!this.controlGate || (this.trigger === 0 && note !== this.note))) {
            if (this.trigger !== 2) this.envStage = 'attack';
          }
          this.note = note;
          this.controlGate = e.gate;
          break;
        }
        case 'romClock':
          if (!this.rom) break;
          this.lfoPhase = e.phase;
          this.lfoStep = e.hz / this.rate;
          break;
        case 'rom': {
          if (!this.rom) break;
          // TRIG GEN monitors the keyboard CV before portamento. Range
          // CV feeds the oscillator separately and has no trigger path.
          const note = e.pitch + 36;  // F3 is code 17 with TRANSPOSE at M.
          if (this.trigger === 0 && e.gate && this.controlGate && note !== this.note) this.envStage = 'attack';
          this.note = note;
          this.romRange = (e.range - 24) / 12;  // 8′ is code 24.
          this.controlGate = e.gate;
          this.glide = e.glide;
          this.random = e.random / 31.5 - 1;
          this.romReset = e.reset;
          if (e.reset) this.lfoPhase = 0;
          break;
        }
        case 'source':
          this.rom = e.rom;
          this.cvInput = !!e.cv;
          this.amp = 0;
          // Switching the patched input also silences notes from that input.
          // Falls through.
        case 'kill':
          this.keys.length = 0;
          this.auditionEnd = Infinity;
          this.env = 0;
          this.envStage = 'release';
          this.bend = 0;
          this.midiMod = 0;
          this.velocityGain = 1;
          this.effect = null;
          this.controlGate = this.envelopeGate = this.romReset = this.glide = false;
          if (this.rom) this.amp = 0;
          break;
      }
    }

    update(p) {
      const trigger = Math.round(p.trigger);
      if (trigger !== this.trigger) {
        this.trigger = trigger;
        if (this.keys.length) {
          this.chooseNote();
          this.envStage = 'attack';
          if (trigger === 2) this.lfoPhase = 0;
        }
      }
      if (!this.rom) this.lfoStep = lfoFrequency(p.lfoRate) / this.rate;
      this.lfoWave = Math.round(p.lfoWave);
      this.rangeOffset = Math.round(p.range) - 1;
      this.transposeOffset = Math.round(p.transpose) - 1;
      this.a4 = p.pitchReference >= 0.5 ? ROLAND_A4 : REBIRTH_A4;
      this.tune = (p.tune - 0.5) / 12;
      this.pwmMode = Math.round(p.pwmMode);
      this.subDiv = Math.round(p.subMode) === 0 ? 2 : 4;
      // S5's third position takes D38/D39's OR, high for 3 of 4 clocks.
      this.subWidth = Math.round(p.subMode) === 2 ? 0.75 : 0.5;
      this.vcaMode = Math.round(p.vca);
      this.portaMode = Math.round(p.portaMode);
      this.portaRate = p.portaTime === 0 ? 1 : rc(5 * p.portaTime ** 2 / T90, this.rate);
      this.attack = rc(sweep(0.0015, 4, p.attack) / ATTACK_THRESHOLD, this.rate);
      this.decay = rc(sweep(0.002, 10, p.decay) / T90, this.rate);
      this.release = rc(sweep(0.002, 10, p.release) / T90, this.rate);
      this.cutoff = sweep(MIN_CUTOFF, MAX_CUTOFF, p.cutoff);
      // R189 (ENV), R191 (MOD) and R193 (KYBD) sum at IC18b. The width
      // calibration sets R193's full-range keyboard input to one octave/V.
      this.envOctaves = p.envMod * ENV_PEAK * 150 / 100;
      this.vcfModOctaves = p.vcfMod * 2.5 * 150 / 100;
      this.vcoModOctaves = p.vcoMod * 2.5 * 100 / 4700;  // R89/R90
      const bendVolts = 2 * p.bender - 1;  // nominal ±1 V at full travel
      this.panelPitchBend = bendVolts * p.bendVco;
      this.panelFilterBend = bendVolts * p.bendVcf * 150 / 100;  // R193/R185
      const lfoVolts = BENDER_LFO_PEAK * p.bendLfo * p.bendLfoPressed;
      this.panelPitchLfo = lfoVolts * 100 / 150;   // Bender R3/R2
      this.panelFilterLfo = lfoVolts * 150 / 220;  // R193/R184
      const saw = mixLeg(p.saw, 100, 470000), pulse = mixLeg(p.pulse, 68000, 680000);
      const sub = mixLeg(p.sub, 47000, 820000), noise = mixLeg(p.noise, 100, 150000);
      this.sawGain = saw.scale * 10;    // CEM3340 pin 8: 2/3 of +15 V
      this.pulseGain = pulse.scale * CONTROL_RAIL;
      this.subGain = sub.scale * CONTROL_RAIL;
      this.noiseGain = noise.scale * 2;  // nominal TR23/IC12a output level
      const conductance = 1 / saw.resistance + 1 / pulse.resistance +
        1 / sub.resistance + 1 / noise.resistance;
      this.mixDCRate = rc(10e-6 / conductance, this.rate);  // C45
    }

    step(p) {
      const patched = this.rom || this.cvInput;
      const held = patched ? this.controlGate : this.keys.length > 0 || !!this.effect;
      const lfo = this.lfoPhase, nextLfo = lfo + (this.romReset ? 0 : this.lfoStep);
      if (patched) {
        const envGate = held && (this.trigger !== 2 || lfo < 0.5);
        if (envGate !== this.envelopeGate) this.envStage = envGate ? 'attack' : 'release';
        this.envelopeGate = envGate;
      } else if (this.trigger === 2 && held) {
        if (nextLfo >= 1) this.envStage = 'attack';
        else if (lfo < 0.5 && nextLfo >= 0.5) this.envStage = 'release';
      }
      if (!this.rom && nextLfo >= 1) this.random = this.white();
      this.lfoPhase = wrap(nextLfo);

      // The selected capacitor path is exponential. The attack charges
      // towards the rail until IC16a switches it to the sustain path.
      if (this.envStage === 'attack') {
        this.env += this.attack * (CHARGE_RAIL - this.env);
        if (this.env >= 1) { this.env = 1; this.envStage = 'decay'; }
      } else if (this.envStage === 'decay') this.env += this.decay * (p.sustain - this.env);
      else this.env += this.release * -this.env;

      const glide = this.portaMode === 2 || (this.portaMode === 1 && this.glide);
      this.cv += (glide ? this.portaRate : 1) * (this.note - this.cv);
      const white = this.white();
      this.noise += this.noiseRate * (white - this.noise);
      const triangle = 1 - 4 * Math.abs(lfo - 0.5);
      let mod = triangle;
      if (this.lfoWave === 1) mod = lfo < 0.5 ? 1 : -1;
      if (this.lfoWave === 2) mod = this.random;
      if (this.lfoWave === 3) mod = this.noise;
      const keyboard = (this.cv - 69) / 12;
      const range = this.rom ? this.romRange : this.rangeOffset + (this.cvInput ? 0 : this.transposeOffset);
      const pitchBend = this.panelPitchBend + (this.panelPitchLfo + this.midiMod * p.bendLfo * 0.08) * triangle + this.bend / 6;
      const filterBend = this.panelFilterBend + this.panelFilterLfo * triangle;
      const freq = this.effect ? this.effectPitch * (this.a4 / REBIRTH_A4)
        : this.a4 * 2 ** (keyboard + range + this.tune + pitchBend + this.vcoModOctaves * mod);
      const advance = Math.min(0.24, freq / this.rate);
      const phase = wrap(this.phase);
      const saw = phase - 0.5 * blep(phase, advance);
      // PWM uses the triangle directly in LFO mode, independently of S2.
      const pwm = this.pwmMode === 0 ? this.env : this.pwmMode === 1 ? 1 : triangle;
      this.width += this.pwmRate * (clamp(0.5 - 0.5 * p.pwm * pwm, 0, 1) - this.width);
      const pulse = 1 - pulseAt(wrap(phase + 0.5 * (1 + this.width)), advance, this.width);  // TR24
      const sub = pulseAt(wrap(this.phase / this.subDiv), advance / this.subDiv, this.subWidth);
      this.phase = (this.phase + advance) % 4;
      const mix = this.sawGain * saw + this.pulseGain * pulse + this.subGain * sub + this.noiseGain * this.noise;
      this.mixDC += this.mixDCRate * (mix - this.mixDC);
      const filterOctaves = p.keyFollow * keyboard + this.envOctaves * this.env + this.vcfModOctaves * mod + filterBend;
      const hz = this.effect ? this.effectCutoff
        : clamp(this.cutoff * 2 ** filterOctaves, MIN_CUTOFF, MAX_CUTOFF);
      const filtered = this.filter.step(mix - this.mixDC + 1e-7 * white, hz, p.resonance);
      // TR26's emitter adds an in-phase path at R132, alongside the dry
      // filter output at R130. BA662A sees the two 150k legs through C53/R139.
      const vca = filtered * (1 + p.resonance);
      this.vcaDC += this.vcaDCRate * (vca - this.vcaDC);
      const gain = this.vcaMode === 0 && !this.effect ? this.env : +held;
      this.amp += this.gateRate * (gain - this.amp);
      const vcaInput = (vca - this.vcaDC) / (2 * THERMAL_VOLTAGE * 152);
      return 0.8 * Math.tanh(vcaInput) * this.amp * p.volume * this.velocityGain;
    }

    render(out, t0, p) {
      if (this.effects && !this.rom && !this.cvInput && !this.effect && !this.queue.length) {
        out.fill(0);
        return;
      }
      this.filter.setDiodes(Math.round(p.diodes));
      this.update(p);
      for (let i = 0; i < out.length; i++) {
        const t = t0 + i / this.fs;
        while (this.queue.length && this.queue[0].t <= t + 1e-9) this.event(this.queue.shift(), t);
        if (t >= this.auditionEnd) {
          this.auditionEnd = Infinity;
          this.key(this.auditionKey, false);
        }
        let level = 1;
        if (this.effects && !this.rom && !this.cvInput) {
          const e = this.effect;
          const elapsed = e ? t - e.start : 0;
          if (!e || elapsed >= e.duration) {
            this.effect = null;
            this.amp = 0;
            out[i] = 0;
            continue;
          }
          const position = elapsed / e.duration;
          const ramp = Math.min(1, elapsed / 0.01, (e.duration - elapsed) / 0.018);
          // Virtual gestures on the same synth: sweep pitch CV and FREQ
          // while the selected FX clip's one-shot gate is high.
          this.effectPitch = e.rise ? sweep(110, 1760, position) : sweep(2200, 82, position);
          this.effectCutoff = e.rise ? sweep(220, 9600, position) : sweep(7200, 150, position);
          level = ramp * (e.rise ? 0.12 + 0.88 * position ** 1.3 : Math.exp(-4 * position));
        }
        out[i] = 0.5 * level * (this.step(p) + this.step(p));
      }
    }
  }

  class MidiPart {
    constructor(fs, { role, patch, poly }) {
      this.settings = { ...defaults(role), ...patch };
      this.slots = Array.from({ length: Math.max(1, poly) }, () => ({ key: -1, voice: new Voice(fs) }));
      this.level = 1 / Math.sqrt(this.slots.length);
      this.bend = this.mod = 0;
      this.scratch = new Float32Array(128);
    }

    control(id, value) { this.settings[id] = value; }

    midi(e) {
      const type = e.status & 0xf0;
      if (type === 0xe0 || (type === 0xb0 && e.d1 === 1)) {
        if (type === 0xe0) this.bend = (((e.d2 << 7) + e.d1) - 8192) / 8192;
        else this.mod = e.d2 / 127;
        const control = type === 0xe0 ? { type: 'bend', value: this.bend, t: e.t } : { type: 'mod', value: this.mod, t: e.t };
        for (const { voice } of this.slots) voice.enqueue(control);
        return;
      }
      if (type !== 0x80 && type !== 0x90) return;
      const down = type === 0x90 && e.d2 > 0;
      const key = (e.status & 15) * 128 + e.d1;
      const slot = this.slots.length === 1 ? this.slots[0]
        : this.slots.find(s => s.key === (down ? -1 : key));
      if (!slot) return;
      slot.key = down ? key : -1;
      slot.voice.enqueue({ type: 'note', note: e.d1, down, gain: 0.55 + 0.45 * e.d2 / 127, t: e.t });
    }

    render(out, t) {
      out.fill(0);
      if (out.length !== this.scratch.length) this.scratch = new Float32Array(out.length);
      for (const { voice } of this.slots) {
        if (!voice.keys.length && !voice.queue.length && Math.max(voice.env, voice.amp) < 1e-5) continue;
        voice.render(this.scratch, t, this.settings);
        for (let i = 0; i < out.length; i++) out[i] += this.scratch[i] * this.level;
      }
    }
  }

  if (typeof registerProcessor === 'function') {
    class SH101Processor extends AudioWorkletProcessor {
      static get parameterDescriptors() {
        return controls.map(def => ({
          name: def.id, defaultValue: def.value, minValue: 0, maxValue: max(def), automationRate: 'k-rate',
        }));
      }
      constructor(options) {
        super();
        const role = options.processorOptions?.role;
        this.voice = new Voice(sampleRate, role === 'fx');
        this.settings = defaults(role);
        this.port.onmessage = ({ data }) => this.voice.enqueue(data);
      }
      process(_inputs, outputs, parameters) {
        for (const { id } of controls) this.settings[id] = parameters[id][0];
        this.voice.render(outputs[0][0], currentTime, this.settings);
        return true;
      }
    }
    registerProcessor('rebirth-sh101', SH101Processor);
    class MidiPartProcessor extends AudioWorkletProcessor {
      constructor(options) {
        super();
        this.part = new MidiPart(sampleRate, options.processorOptions);
        this.port.onmessage = ({ data }) => {
          if (data.type === 'control') this.part.control(data.id, data.value);
          else this.part.midi(data);
        };
      }
      process(_inputs, outputs) {
        this.part.render(outputs[0][0], currentTime);
        return true;
      }
    }
    registerProcessor('rebirth-midi-part', MidiPartProcessor);
  } else {
    const api = { Voice, Filter, MidiPart, groups, controls, defaults, max, cutoffPosition, lfoFrequency };
    if (typeof module !== 'undefined' && module.exports) module.exports = api;
    else root.SH101 = api;
  }
})(globalThis);
