// Standard MIDI File reader and ReBirth MIDI Song image writer. Source tracks
// remain separate even when their MIDI channel numbers are identical.
(function (root) {
  'use strict';

  const decoder = new TextDecoder();
  const headerSize = 40, eventSize = 8;
  const flashAddress = 0x100e0000, flashBytes = 1024 * 1024;
  const kindOf = e => e.status & 0xf0;
  const noteOn = e => kindOf(e) === 0x90 && e.d2 !== 0;
  const noteOff = e => kindOf(e) === 0x80 || (kindOf(e) === 0x90 && !e.d2);
  const sorted = (a, b) => a.tick - b.tick || a.track - b.track;

  function read(bytes, name) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (bytes.length < 14 || view.getUint32(0) !== 0x4d546864) throw Error('Нет заголовка MThd.');
    const headerEnd = 8 + view.getUint32(4);
    const format = view.getUint16(8), count = view.getUint16(10), ppqn = view.getUint16(12);
    if (format > 1 || !ppqn || ppqn & 0x8000 || !count || count > 256 || (format === 0 && count !== 1) ||
        headerEnd < 14 || headerEnd > bytes.length) throw Error('Нужен MIDI формата 0 или 1 с PPQN.');
    let pos = headerEnd;
    const tempos = [], events = [], tracks = [];
    let duration = 0;
    for (let track = 0; track < count; track++) {
      if (pos + 8 > bytes.length || view.getUint32(pos) !== 0x4d54726b) throw Error('Нет дорожки MTrk ' + track + '.');
      const end = pos + 8 + view.getUint32(pos + 4);
      if (end > bytes.length) throw Error('Оборванная MIDI-дорожка ' + track + '.');
      pos += 8;
      const byte = () => {
        if (pos >= end) throw Error('Оборванное событие на дорожке ' + track + '.');
        return bytes[pos++];
      };
      const variable = () => {
        let value = 0;
        for (let i = 0; i < 4; i++) {
          const b = byte();
          value = value * 128 + (b & 127);
          if (b < 128) return value;
        }
        throw Error('Слишком длинное MIDI-значение.');
      };
      let tick = 0, running = 0, title = 'Track ' + (track + 1), notes = 0, poly = 0, held = 0, sysex = 0;
      const active = new Uint16Array(16 * 128);
      const partEvents = [];
      while (pos < end) {
        tick += variable();
        let status = byte();
        if (status < 128) {
          if (!running) throw Error('Пропущен статус MIDI на дорожке ' + track + '.');
          pos--;
          status = running;
        }
        if (status >= 0xf0) {
          running = 0;
          if (status !== 0xff && status !== 0xf0 && status !== 0xf7) throw Error('Неизвестный статус SMF.');
          const meta = status === 0xff ? byte() : null;
          const size = variable();
          if (pos + size > end) throw Error('Оборванное метасобытие MIDI.');
          if (meta === 0x03) title = decoder.decode(bytes.subarray(pos, pos + size)).trim() || title;
          if (meta === 0x51 && size === 3) {
            const us = bytes[pos] * 65536 + bytes[pos + 1] * 256 + bytes[pos + 2];
            if (us) tempos.push({ tick, track, us });
          }
          if (meta === null) sysex++;
          pos += size;
          if (meta === 0x2f) break;
          continue;
        }
        running = status;
        const command = status & 0xf0;
        const d1 = byte(), d2 = command === 0xc0 || command === 0xd0 ? 0 : byte();
        if (d1 >= 128 || d2 >= 128) throw Error('Некорректные данные MIDI на дорожке ' + track + '.');
        const event = { track, tick, status, d1, d2 };
        const key = (status & 15) * 128 + d1;
        if (noteOn(event)) {
          notes++;
          active[key]++;
          poly = Math.max(poly, ++held);
        } else if (noteOff(event) && active[key]) {
          active[key]--;
          held--;
        }
        partEvents.push(event);
        events.push(event);
      }
      tracks.push({ track, name: title, events: partEvents, notes, poly, sysex });
      duration = Math.max(duration, tick);
      pos = end;
    }
    const map = [{ tick: 0, time: 0, us: 500000 }];
    for (const tempo of tempos.sort(sorted)) {
      const prior = map[map.length - 1];
      if (tempo.tick === prior.tick) prior.us = tempo.us;
      else if (tempo.us !== prior.us) {
        map.push({ tick: tempo.tick, time: prior.time + (tempo.tick - prior.tick) * prior.us / ppqn, us: tempo.us });
      }
    }
    const timeAt = tick => {
      let at = map.length - 1;
      while (map[at].tick > tick) at--;
      const segment = map[at];
      return Math.round(segment.time + (tick - segment.tick) * segment.us / ppqn);
    };
    events.sort(sorted);
    for (const event of events) event.t = timeAt(event.tick);
    const title = name.replace(/\.midi?$/i, '');
    return { id: title, title, duration: timeAt(duration), lastTick: duration, bpm: 60000000 / map[0].us,
      ppqn, timeAt, tempos: map, tracks, events };
  }

  const normalized = name => name.trim().normalize('NFKC').toLowerCase().replace(/[^\p{L}\p{N}]+/gu, ' ').trim();
  const drumNotes = [
    [/\bsnare|ghost beat/, 38], [/\b(clap|claps)\b/, 39], [/\b(tambourine|tambourines)\b/, 54],
    [/\b(hat|hats)\b/, 42], [/reverse cymbal/, 57], [/cymbal/, 49],
    [/percussion|perc loop|triangle|guiro|woodblock|\bhits\b/, 37],
  ];
  function tagPart(part) {
    const name = normalized(part.name);
    if (/^midi gate$/.test(name)) return { ...part, role: 'gate' };
    if (/\bkicks?\b/.test(name)) return { ...part, role: 'kick', drum: 36 };
    const drum = drumNotes.find(([pattern]) => pattern.test(name));
    if (drum) return { ...part, role: 'drums', drum: drum[1] };
    const role = /\b(pad|chords?|atmos)\b/.test(name) ? 'pad'
      : /\b(fx|sweep|noise)\b/.test(name) ? 'fx'
      : /\bbass\b/.test(name) ? 'bass'
      : /\b(seq|arp)\b/.test(name) ? 'arp' : 'lead';
    return { ...part, role, gated: /\bgated\b/.test(name) };
  }

  // TAL's normalized knob positions. Selectors are mapped to the corresponding
  // discrete positions on the virtual SH-101. Plugin extensions remain in XML.
  const talKnobs = {
    volume: 'volume', masterfinetune: 'tune', lforate: 'lfoRate', dcolfovalue: 'vcoMod',
    dcopwmvalue: 'pwm', pulsevolume: 'pulse', sawvolume: 'saw', suboscvolume: 'sub', noisevolume: 'noise',
    filtercutoff: 'cutoff', filterresonance: 'resonance', filterenvelopevalue: 'envMod',
    filtermodulationvalue: 'vcfMod', filterkeyboardvalue: 'keyFollow', portamentointensity: 'portaTime',
    adsrattack: 'attack', adsrdecay: 'decay', adsrsustain: 'sustain', adsrrelease: 'release',
    controlbenderdco: 'bendVco', controlbenderfilter: 'bendVcf', controlbendermodulation: 'bendLfo',
  };
  const talSwitches = {
    dcorange: ['range', 3], octavetranspose: ['transpose', 2], portamentomode: ['portaMode', 2],
    lfowaveform: ['lfoWave', 3], dcopwmmode: ['pwmMode', 2, true], suboscmode: ['subMode', 2],
    vcamode: ['vca', 1], adsrmode: ['trigger', 2, true],
  };
  function patchXML(xml, file) {
    const data = new DOMParser().parseFromString(xml, 'application/xml').querySelector('tal > programs > program');
    if (!data) throw Error('Не удалось прочитать пресет ' + file + '.');
    const settings = {};
    const value = key => {
      if (!data.hasAttribute(key)) return undefined;
      const n = Number(data.getAttribute(key));
      return Number.isFinite(n) ? Math.min(1, Math.max(0, n)) : undefined;
    };
    for (const [key, id] of Object.entries(talKnobs)) {
      const n = value(key);
      if (n !== undefined) settings[id] = n;
    }
    for (const [key, [id, last, reverse]] of Object.entries(talSwitches)) {
      const n = value(key);
      if (n !== undefined) settings[id] = Math.round((reverse ? 1 - n : n) * last);
    }
    return { file, name: data.getAttribute('programname'), settings };
  }

  async function load(files) {
    const all = Array.from(files);
    const presets = new Map();
    for (const file of all.filter(f => /\.bassline$/i.test(f.name))) {
      const patch = patchXML(await file.text(), file.name);
      presets.set(normalized(file.name.replace(/\.bassline$/i, '')), patch);
    }
    const songs = await Promise.all(all.filter(f => /\.midi?$/i.test(f.name)).map(async file => {
      const song = read(new Uint8Array(await file.arrayBuffer()), file.name);
      song.tracks = song.tracks.map(part => {
        const patch = presets.get(normalized(song.title + ' ' + part.name));
        if (patch) presets.delete(normalized(patch.file.replace(/\.bassline$/i, '')));
        return { ...tagPart(part), patch };
      });
      return song;
    }));
    if (!songs.length) throw Error('В выбранной папке нет MIDI-файлов.');
    return { songs: songs.sort((a, b) => a.title.localeCompare(b.title)), unmatched: [...presets.values()] };
  }

  function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
      crc ^= byte;
      for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
  }

  function image(song, routes = {}) {
    if (song.tracks.length > 64 || song.duration > 0x7fffffff) throw Error('Лимит MIDI Song — 64 дорожки и 35 минут.');
    const taken = new Set(), held = new Map();
    for (const [track, route] of Object.entries(routes)) {
      if (!song.tracks[track]) throw Error('Неизвестная MIDI-дорожка ' + track + '.');
      if (route.midi !== undefined && (!Number.isInteger(route.midi) || route.midi < 0 || route.midi > 15)) {
        throw Error('MIDI-каналы назначения должны быть от 1 до 16.');
      }
      if (route.cv === undefined) continue;
      if (!Number.isInteger(route.cv) || route.cv < 0 || route.cv >= 4 || taken.has(route.cv)) {
        throw Error('Каждой CV/Gate-паре нужна одна дорожка.');
      }
      if (song.tracks[track]?.poly !== 1) throw Error('CV/Gate требует монофоническую MIDI-дорожку.');
      taken.add(route.cv);
    }
    const events = [];
    for (const e of song.events) {
      const r = routes[e.track];
      if (r?.mute) continue;
      if (r?.cv === undefined) {
        const status = r?.midi === undefined ? e.status : (e.status & 0xf0) | r.midi;
        events.push(status === e.status ? e : { ...e, status });
      } else if (noteOn(e)) {
        const d1 = e.d1 + (r.transpose || 0);
        if (!Number.isInteger(d1) || d1 < 24 || d1 > 48) {
          throw Error(song.tracks[e.track].name + ' · нота ' + d1 + ' за пределами CV 24–48.');
        }
        held.set(e.track, e.d1);
        events.push({ ...e, status: 0x10 + r.cv, d1, d2: 1 });
      } else if (noteOff(e) && held.get(e.track) === e.d1) {
        held.delete(e.track);
        events.push({ ...e, status: 0x10 + r.cv, d1: 0, d2: 0 });
      }
    }
    // Clock follows the tempo map, including changes between notes. The wire
    // sends these bytes with real-time priority over the ordinary messages.
    for (let n = 0; n * song.ppqn / 24 < song.lastTick; n++) {
      const t = song.timeAt(n * song.ppqn / 24);
      events.push({ t, track: 255, status: 0xf8, d1: 0, d2: 0 });
    }
    events.sort((a, b) => a.t - b.t || (b.status === 0xf8) - (a.status === 0xf8));
    const bytes = new Uint8Array(headerSize + events.length * eventSize);
    if (bytes.length > flashBytes) throw Error('MIDI Song превышает 1 МиБ.');
    const view = new DataView(bytes.buffer);
    bytes.set([82, 66, 77, 49]); // RBM1
    view.setUint32(4, events.length, true);
    view.setUint32(8, song.duration, true);
    view.setUint8(12, song.tracks.length);
    const asciiTitle = song.title.replace(/[^\x20-\x7e]/g, ' ').slice(0, 19);
    bytes.set(new TextEncoder().encode(asciiTitle), 20);
    events.forEach(({ t, track, status, d1, d2 }, i) => {
      const at = headerSize + i * eventSize;
      view.setUint32(at, t, true);
      bytes.set([track, status, d1, d2], at + 4);
    });
    view.setUint32(16, crc32(bytes), true);
    return bytes;
  }

  function uf2(imageBytes, board = 'pico') {
    // RP2350 uses an absolute UF2 to update ReBirth's fixed data slot.
    let family;
    if (board === 'pico') family = 0xe48bff56;
    else if (board === 'pico2') family = 0xe48bff57;
    else throw Error('Неизвестная плата для MIDI Song UF2.');
    if (!imageBytes.length || imageBytes.length > flashBytes) throw Error('MIDI Song не помещается в слот.');
    const count = Math.ceil(imageBytes.length / 256);
    const bytes = new Uint8Array(count * 512);
    const view = new DataView(bytes.buffer);
    for (let i = 0; i < count; i++) {
      const at = i * 512;
      [0x0a324655, 0x9e5d5157, 0x2000, flashAddress + i * 256, 256, i, count, family]
        .forEach((word, j) => view.setUint32(at + 4 * j, word, true));
      bytes.set(imageBytes.subarray(i * 256, (i + 1) * 256), at + 32);
      view.setUint32(at + 508, 0x0ab16f30, true);
    }
    return bytes;
  }

  const api = { read, load, image, uf2, noteOn, noteOff };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.MidiSongs = api;
})(globalThis);
