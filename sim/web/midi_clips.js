// A Standard MIDI File laid out as the device's own clips. Each SMF track
// with notes becomes one or more device tracks (a polyphonic part one track
// per voice, a drum part one track per drum sound), each bar a 16-step clip,
// the distinct bars of a track its locations, and the song's order of bars a
// Song-mode chain. The result is a save image that the firmware loads with
// LOAD, as if it had saved it itself; the page writes it into the simulated
// flash (Device.writeStore). Notes are quantised to the 1/16 grid; controllers
// and pitch bend are not carried over. The chain holds as many rows as the core
// does (Device.songEntries(): 384, a version 8 save; 256 in an older core, a
// version 7 save): a longer song plays up to the bar where the row past them
// would start.
(function (root) {
  'use strict';

  const MidiSongs = root.MidiSongs || (typeof require !== 'undefined' ? require('./midi_songs.js') : null);
  const NUM_TRACKS = 64, LOCATIONS = 32, STEPS = 16, SILENT = LOCATIONS - 1;
  const DRUM_CHANNEL = 9, GATE_CHANNEL = 15;
  const MELODIC_CHANNELS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14];
  const ACTIVE = 1, TIE = 8;

  // The save (src/flash_storage.c) as the simulator's wasm32 build lays out
  // pattern_t, song_t and kit_config_t (4-byte enums): version 8 with 384 song
  // rows of 68 bytes, or version 7 with 256 rows of 65, for an older core. A
  // row is its 64 locations and then the repeats, at byte 64 in both; the rest
  // of a row is 0. sim/test/midi_clips_check.js checks this against a save the
  // build writes. The RP2040 build packs enums into a byte (track_config_t 7
  // bytes, pattern_t 4108, song_t 26124), so this image is for the simulator
  // only: an image for the board needs that layout. PAGE is the flash page.
  const PAGE = 256, PATTERN_BYTES = 4684, KIT_BYTES = 15;
  const PAT_OFFSET = PAGE, SONG_OFFSET = PAT_OFFSET + Math.ceil(PATTERN_BYTES * LOCATIONS / PAGE) * PAGE;
  const STORE_BYTES = 192 * 1024;
  const FORMATS = { 256: { VERSION: 7, ROW_BYTES: 65 }, 384: { VERSION: 8, ROW_BYTES: 68 } };
  function layoutFor(rows) {
    const f = typeof rows === 'number' && Object.prototype.hasOwnProperty.call(FORMATS, rows) ? FORMATS[rows] : null;
    if (!f) throw Error('Цепочка прибора — 256 или 384 строки, не ' + rows + '.');
    const TRAILER = rows * f.ROW_BYTES, SONG_BYTES = TRAILER + 20;
    return { ROWS: rows, VERSION: f.VERSION, ROW_BYTES: f.ROW_BYTES, TRAILER, PAGE, PAT_OFFSET, SONG_OFFSET,
             SONG_BYTES, KIT_OFFSET: SONG_OFFSET + Math.ceil(SONG_BYTES / PAGE) * PAGE, PATTERN_BYTES, STORE_BYTES };
  }
  const KIT_DEFAULT = [36, 38, 45, 47, 50, 37, 39, 42, 46, 49, 51, 80, 110, 127, 3];
  const DRUM_NAME = { 36: 'BD', 38: 'SD', 39: 'CP', 42: 'CH', 44: 'PH', 46: 'OH', 49: 'CY', 54: 'TB', 57: 'RC', 37: 'RS' };

  function notesOf(part) {
    const notes = [], open = new Map();
    for (const e of part.events) {
      const key = (e.status & 15) * 128 + e.d1;
      if (MidiSongs.noteOn(e)) {
        const n = { tick: e.tick, end: null, note: e.d1, vel: e.d2 };
        notes.push(n);
        if (!open.has(key)) open.set(key, []);
        open.get(key).push(n);
      } else if (MidiSongs.noteOff(e)) {
        const held = open.get(key);
        if (held && held.length) held.shift().end = e.tick;
      }
    }
    for (const n of notes) if (n.end === null) n.end = n.tick + 1;
    return notes;
  }

  // One voice per track: each note goes to the lowest voice that is free when
  // it starts, so a chord's notes land on separate tracks. A voice is free
  // when its last note ends by this one's start on the grid, or in the file
  // up to half a step after it (a legato overlap); two notes that follow each
  // other within one step stay one voice, where the grid keeps the louder.
  function voices(notes, step) {
    const sorted = notes.slice().sort((a, b) => a.tick - b.tick || a.note - b.note);
    const ends = [], out = [];
    for (const n of sorted) {
      const s = Math.round(n.tick / step), e = s + Math.max(1, Math.round((n.end - n.tick) / step));
      let v = ends.findIndex(end => end.grid <= s || end.tick <= n.tick + step / 2);
      if (v < 0) { v = ends.length; ends.push(null); out.push([]); }
      ends[v] = { grid: e, tick: n.end };
      out[v].push(n);
    }
    return out;
  }

  // The track's notes on the global 16th grid: a note of n steps is n active
  // steps of its pitch, tied but for the last, cut at the next note's start.
  function grid(notes, step, bars) {
    const cells = new Array(bars * STEPS).fill(null);
    const byStep = new Map();
    let offGrid = 0;
    for (const n of notes) {
      if (n.tick % step) offGrid++;
      const s = Math.round(n.tick / step);
      if (s >= cells.length) continue;
      const prior = byStep.get(s);
      if (!prior || n.vel > prior.vel) byStep.set(s, n);
    }
    const starts = [...byStep.keys()].sort((a, b) => a - b);
    const fracs = [];
    starts.forEach((s, i) => {
      const n = byStep.get(s);
      const want = Math.max(1, Math.round((n.end - n.tick) / step));
      const end = Math.min(s + want, i + 1 < starts.length ? starts[i + 1] : cells.length);
      if (end - s === want) fracs.push(Math.min(1, Math.max(0.1, (n.end - n.tick) / step - (want - 1))));
      for (let g = s; g < end; g++) {
        cells[g] = { note: n.note, vel: Math.max(1, n.vel), flags: ACTIVE | (g < end - 1 ? TIE : 0) };
      }
    });
    fracs.sort((a, b) => a - b);
    const gate = fracs.length ? Math.round(Math.min(100, Math.max(10, fracs[fracs.length >> 1] * 100))) : 50;
    return { cells, gate, offGrid, merged: notes.length - byStep.size };
  }

  // The plan for a chain of `rows` rows (the core's Device.songEntries()).
  function plan(song, { rows = 384 } = {}) {
    layoutFor(rows);
    const step = song.ppqn / 4;
    const tracks = [], parts = [];
    for (const raw of song.tracks) {
      const part = raw.role ? raw : MidiSongs.tag(raw);
      const notes = notesOf(part);
      if (!notes.length) continue;
      const lost = part.events.filter(e => (e.status & 0xf0) === 0xb0 || (e.status & 0xf0) === 0xe0).length;
      const info = { part, lost, tracks: [] };
      parts.push(info);
      if (part.role === 'gate') {
        info.tracks.push({ name: part.name, role: 'gate', type: 0, channel: GATE_CHANNEL, notes });
      } else if (part.drum) {
        const gm = n => part.drum === 42 && [42, 44, 46].includes(n.note) ? n.note : part.drum;
        const sounds = [...new Set(notes.map(gm))].sort((a, b) => a - b);
        for (const sound of sounds) {
          info.tracks.push({ name: part.name + (sounds.length > 1 ? ' ' + (DRUM_NAME[sound] || sound) : ''),
            role: part.role, type: 1, drum: sound, channel: DRUM_CHANNEL,
            notes: notes.filter(n => gm(n) === sound).map(n => ({ ...n, note: sound })) });
        }
      } else {
        const split = voices(notes, step);
        split.forEach((voice, v) => info.tracks.push({ name: part.name + (split.length > 1 ? ' ' + (v + 1) : ''),
          role: part.role, type: 0, notes: voice }));
      }
      tracks.push(...info.tracks);
    }
    if (tracks.length > NUM_TRACKS) throw Error('Партий с голосами ' + tracks.length + ', а треков 64.');

    // The bars up to the end of the track chunks (an end on a bar line ends
    // the bar before it), and the bar of any note quantised past that.
    let bars = Math.max(1, Math.ceil(song.lastTick / (step * STEPS)));
    for (const track of tracks) for (const n of track.notes) bars = Math.max(bars, Math.floor(Math.round(n.tick / step) / STEPS) + 1);
    for (const track of tracks) Object.assign(track, grid(track.notes, step, bars));

    // Melodic parts share a channel with parts of the same kind (role and
    // gating) that never play in the same or a neighbouring bar. Each kind
    // gets a channel first, so a late kind is not left without one; when the
    // channels run out, a part joins the one of its kind it overlaps least
    // and that channel's voice gets its notes as extra polyphony.
    const infos = [];
    for (const info of parts) {
      if (info.tracks[0].channel !== undefined) continue;
      info.active = new Set();
      for (const t of info.tracks) t.cells.forEach((c, g) => { if (c) info.active.add(Math.floor(g / STEPS)); });
      info.key = info.part.role + (info.part.gated ? ' gated' : '');
      infos.push(info);
    }
    // The busiest parts get a channel each while channels last; a later part
    // joins a channel none of whose parts plays in its bars or next to them,
    // of its own kind if there is one, else of any kind.
    const free = MELODIC_CHANNELS.slice(), channels = [];
    const overlap = (ch, info) => [...info.active].filter(b => ch.bars.has(b) || ch.bars.has(b - 1) || ch.bars.has(b + 1)).length;
    infos.sort((a, b) => b.tracks.reduce((n, t) => n + t.notes.length, 0) - a.tracks.reduce((n, t) => n + t.notes.length, 0));
    for (const info of infos) {
      const clear = channels.filter(c => !overlap(c, info));
      const role = c => c.key.split(' ')[0] === info.part.role;
      // A part with a preset of its own keeps it on a channel of its own.
      let ch = info.part.patch && free.length ? null : clear.find(c => c.key === info.key);
      if (!ch && free.length) {
        ch = { key: info.key, channel: free.shift(), bars: new Set(), parts: [], poly: 0 };
        channels.push(ch);
      }
      // Not with a part gated otherwise: the channel's voice is gated or not.
      const gating = c => c.key.endsWith(' gated') === !!info.part.gated;
      // With no channel left, a part of the same kind that overlaps least
      // (its notes add voices) comes before a quiet channel of another role,
      // whose preset would be wrong; a role gated otherwise comes last.
      const same = channels.filter(c => c.key === info.key);
      const least = pool => pool.reduce((a, b) => overlap(b, info) < overlap(a, info) ? b : a);
      if (ch) ch.poly = Math.max(ch.poly, info.tracks.length);
      else if (same.length) {
        ch = least(same);
        ch.poly += info.tracks.length;
      } else {
        ch = clear.find(c => role(c) && gating(c)) || clear.find(gating) || clear.find(role) || clear[0];
        if (ch) ch.poly = Math.max(ch.poly, info.tracks.length);
        else {
          ch = least(channels);
          ch.poly += info.tracks.length;
        }
      }
      for (const b of info.active) ch.bars.add(b);
      ch.parts.push(info.part);
      for (const t of info.tracks) t.channel = ch.channel;
    }

    // Each track's distinct bars are its clips, in the order they first play.
    // A track holds 31 of them and an empty one; a part with more goes on in
    // a track of its own right after it ("+2", same routing), which plays the
    // bars of its clips while the first is silent, and so on.
    const laid = [];
    for (const track of tracks) {
      const layers = [];
      const layer = () => {
        const t = { ...track, name: layers.length ? track.name + ' +' + (layers.length + 1) : track.name,
                    cells: new Array(track.cells.length).fill(null), clips: [], ids: new Map(), locations: [] };
        if (layers.length) t.offGrid = t.merged = 0;
        layers.push(t);
        return t;
      };
      layer();
      const sigOf = b => track.cells.slice(b * STEPS, (b + 1) * STEPS)
        .map(c => c ? c.note + '/' + c.vel + '/' + c.flags : '').join(',');
      // Bars a tied note crosses into go on one track together: a tie
      // sounds on only within a track.
      const tiedInto = b => {
        const last = track.cells[b * STEPS - 1], first = track.cells[b * STEPS];
        return b > 0 && last && first && (last.flags & TIE) && first.note === last.note;
      };
      for (let b = 0; b < bars;) {
        let e = b;
        while (e + 1 < bars && tiedInto(e + 1)) e++;
        const group = [];
        for (let k = b; k <= e; k++) if (/\d/.test(sigOf(k))) group.push(k);
        const sigs = [...new Set(group.map(sigOf))];
        let owner = null;
        if (group.length) {
          const missing = l => sigs.filter(x => !l.ids.has(x)).length;
          owner = layers.filter(l => l.clips.length + missing(l) <= SILENT)
            .sort((x, y) => missing(x) - missing(y))[0] || layer();
          for (const x of sigs) if (!owner.ids.has(x) && owner.clips.length < SILENT) {
            owner.ids.set(x, owner.clips.length);
            owner.clips.push(track.cells.slice(group.find(k => sigOf(k) === x) * STEPS, (group.find(k => sigOf(k) === x) + 1) * STEPS));
          }
        }
        for (let k = b; k <= e; k++) {
          const x = sigOf(k), here = owner && owner.ids.has(x) ? owner : layers.find(l => l.ids.has(x));
          if (here) here.cells.splice(k * STEPS, STEPS, ...track.cells.slice(k * STEPS, (k + 1) * STEPS));
          for (const l of layers) l.locations[k] = l === here ? l.ids.get(x) : null;
        }
        b = e + 1;
      }
      // A silent bar plays the first free location, an empty clip next to
      // the track's own, so it stays in bank A for most tracks (a MUTE key
      // blinks while its track plays a location outside the shown bank).
      for (const l of layers) {
        l.silent = Math.min(l.clips.length, SILENT);
        l.locations = Array.from({ length: bars }, (_, b) => l.locations[b] ?? l.silent);
        delete l.ids;
      }
      laid.push(...layers);
    }
    if (laid.length > NUM_TRACKS) throw Error('С продолжениями партий нужно ' + laid.length + ' треков, а их 64.');
    tracks.splice(0, tracks.length, ...laid);

    const entries = [];
    for (let b = 0; b < bars; b++) {
      const locations = Array.from({ length: NUM_TRACKS }, (_, t) => tracks[t] ? tracks[t].locations[b] : 0);
      const last = entries[entries.length - 1];
      if (last && last.repeats < 255 && last.locations.every((l, t) => l === locations[t])) last.repeats++;
      else entries.push({ locations, repeats: 1 });
    }

    const played = entries.slice(0, rows).reduce((n, e) => n + e.repeats, 0);
    // The device plays one tempo: the song's, the one it keeps longest
    // (midi_songs.js), to 0.1 BPM within the firmware's 20-300 BPM.
    const bpm = Math.min(300, Math.max(20, Math.round(song.bpm * 10) / 10));
    return {
      title: song.title, bpm, bars, played, rows, tracks, entries, channels,
      ppqn: song.ppqn, meters: song.meters || [],
      gateChannel: GATE_CHANNEL, drumChannel: DRUM_CHANNEL,
      lost: {
        offGrid: tracks.reduce((n, t) => n + t.offGrid, 0),
        merged: tracks.reduce((n, t) => n + t.merged, 0),
        controllers: parts.reduce((n, p) => n + p.lost, 0),
        entries: Math.max(0, entries.length - rows),  // rows past those the device holds
      },
    };
  }

  // ─── The live MUTE part (docs/superpowers/specs/2026-11-25-sham-live-mutes-design.md) ───
  // Moves derived from the layout: a kick roll before a section line, a drop
  // (kick and bass) before a big one, a hats cut or the snare out inside a
  // steady groove, each back on a phrase line. Chain bars are 0-based.
  const LIVE = { SPACE: 8, GROOVE_SPACE: 16, SECTION: 4, BIG: 8, QUIET: 2, DROP_EVERY: 64, HATS: 4, SNARE: 2 };

  function mutes(p) {
    const P = p.played, T = p.tracks, barTicks = (p.ppqn || 480) * 4;
    const sounds = T.map(t => Array.from({ length: P }, (_, b) => t.cells.slice(b * STEPS, (b + 1) * STEPS).some(c => c && (c.flags & ACTIVE))));
    const on = (t, b) => b >= 0 && b < P && sounds[t][b];
    const change = b => b < 1 ? 0 : T.reduce((n, t, i) => n + (t.role !== 'gate' && on(i, b) !== on(i, b - 1) ? 1 : 0), 0);

    // Musical bar lines from the time signatures; a chain bar is usable when
    // it starts on one of a 4/4 bar. Runs of usable bars are the stretches,
    // each with its own phrase grid.
    // A time signature that makes no bar (a zero numerator, a denominator
    // past 1/64) is left out: the one before it goes on, 4/4 at the start.
    const valid = m => m.num >= 1 && m.den >= 1 && m.den <= 64 && m.num * barTicks / m.den >= 1;
    const meters = (p.meters || []).filter(valid);
    if (!meters.length || meters[0].tick > 0) meters.unshift({ tick: 0, num: 4, den: 4 });
    const usable = new Set();
    meters.forEach((m, k) => {
      const until = Math.min(k + 1 < meters.length ? meters[k + 1].tick : P * barTicks, P * barTicks);
      const len = m.num * barTicks / m.den;
      for (let t = m.tick; t < until; t += len) if (m.num === 4 && m.den === 4 && t % barTicks === 0) usable.add(t / barTicks);
    });
    const stretches = [];
    for (let b = 0; b < P;) {
      if (!usable.has(b)) { b++; continue; }
      let e = b;
      while (e < P && usable.has(e)) e++;
      stretches.push([b, e]);
      b = e;
    }
    const lines = [];
    for (const [a, e] of stretches) {
      let best = -1, phase = 0;
      for (let f = 0; f < 8; f++) {
        let sum = 0;
        for (let b = a; b < e; b++) if (b >= 1 && b % 8 === f) sum += change(b);
        if (sum > best) { best = sum; phase = f; }
      }
      for (let b = a; b < e; b++) if (b % 8 === phase) lines.push(b);
    }
    // The set ends on the last phrase line inside a chain that is cut short,
    // when that line is in its last 8 bars; else (no 4/4 there) the chain
    // plays out.
    let setEnd = P;
    if (P < p.bars) {
      const inside = lines.filter(b => b <= P && b >= P - 8);
      if (inside.length) setEnd = Math.max(...inside);
    }

    // The groups over every track, continuations (+2) included; a move with
    // a track past page 2 is not made, so a group is never muted in part.
    const pick = test => T.map((t, i) => i).filter(i => T[i].role !== 'gate' && test(T[i]));
    const groups = {
      kick: pick(t => t.role === 'kick'),
      hats: pick(t => [42, 44, 46].includes(t.drum)),
      snare: pick(t => [38, 40].includes(t.drum) && !/fill/i.test(t.name)),
      bass: pick(t => t.role === 'bass'),
    };
    const tieInto = (t, b) => {
      const cell = T[t].cells[b * STEPS], prior = T[t].cells[b * STEPS - 1];
      return !!(b > 0 && cell && prior && prior.note === cell.note && (prior.flags & TIE));
    };
    // A group's tracks that play anywhere from the press bar to the return,
    // each of them in every one of those bars; else null.
    const whole = (group, press, end) => {
      const tracks = groups[group].filter(t => { for (let b = press; b <= end; b++) if (on(t, b)) return true; return false; });
      if (!tracks.length) return null;
      for (const t of tracks) for (let b = press; b <= end; b++) if (!on(t, b)) return null;
      return tracks;
    };
    const make = (kind, names, start, end) => {
      const press = start - 1;
      if (!stretches.some(([a, e]) => press >= a && end < e)) return null;
      for (let b = start; b < end; b++) if (change(b) > LIVE.QUIET) return null;
      const tracks = [];
      for (const name of names) {
        const these = whole(name, press, end);
        if (!these) return null;
        tracks.push(...these);
      }
      if (tracks.some(t => t >= 16)) return null;
      if (tracks.some(t => tieInto(t, start) || tieInto(t, end))) return null;
      return { kind, press, start, end, tracks };
    };

    const moves = [];
    let prior = -Infinity, lastDrop = -Infinity, count = 0;
    for (const L of lines) {
      if (L < prior + LIVE.SPACE || L > setEnd - LIVE.SPACE) continue;
      const c = change(L);
      let m = null;
      if (c >= LIVE.SECTION) {
        if (c >= LIVE.BIG && groups.bass.length && L - lastDrop >= LIVE.DROP_EVERY) m = make('drop', ['kick', 'bass'], L - 2, L);
        if (!m) m = make('kick', ['kick'], L - 1, L);
      } else if (c <= LIVE.QUIET && L >= prior + LIVE.GROOVE_SPACE) {
        let steady = groups.kick.length > 0;
        for (let b = L - 8; b <= L && steady; b++) steady = groups.kick.some(t => on(t, b));
        if (steady) {
          const hats = () => groups.hats.length ? make('hats', ['hats'], L - LIVE.HATS, L) : null;
          const snare = () => groups.snare.length ? make('snare', ['snare'], L - LIVE.SNARE, L) : null;
          m = count++ % 2 === 0 ? hats() || snare() : snare() || hats();
        }
      }
      if (!m) continue;
      moves.push(m);
      prior = L;
      if (m.kind === 'drop') lastDrop = L;
    }
    return { setEnd, stretches, groups, moves };
  }

  function crc32(crc, bytes) {
    for (const byte of bytes) {
      crc ^= byte;
      for (let i = 0; i < 8; i++) crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
    return crc >>> 0;
  }

  // The save image. With opts.bars (the performed mode) the chain keeps only
  // the rows of its first opts.bars bars, so the device stops there.
  function chain(p, bars) {
    const entries = p.entries.slice(0, p.rows);
    if (bars === undefined) return entries;
    const out = [];
    let n = 0;
    for (const e of entries) {
      if (n >= bars) break;
      const repeats = Math.min(e.repeats, bars - n);
      out.push(repeats === e.repeats ? e : { ...e, repeats });
      n += repeats;
    }
    return out;
  }

  function store(p, opts) {
    const { VERSION, ROW_BYTES, TRAILER, SONG_BYTES, KIT_OFFSET } = layoutFor(p.rows);
    const bytes = new Uint8Array(STORE_BYTES).fill(0xff);
    const view = new DataView(bytes.buffer);
    const text = (at, s, size) => {
      for (let i = 0; i < size; i++) bytes[at + i] = i < size - 1 ? (s.charCodeAt(i) || 0) : 0;
    };
    for (let loc = 0; loc < LOCATIONS; loc++) {
      const at = PAT_OFFSET + loc * PATTERN_BYTES;
      bytes.fill(0, at, at + PATTERN_BYTES);
      for (let t = 0; t < NUM_TRACKS; t++) {
        const track = p.tracks[t];
        const clip = track && track.clips[loc];
        if (clip) clip.forEach((c, s) => {
          if (!c) return;
          const o = at + (t * STEPS + s) * 3;
          bytes[o] = c.note; bytes[o + 1] = c.vel; bytes[o + 2] = c.flags;
        });
        const o = at + 3072 + t * 16;
        view.setUint32(o, track ? track.type : 0, true);
        bytes[o + 4] = track ? track.channel : t % 16;
        bytes[o + 5] = track && track.drum ? track.drum : Math.min(127, 36 + t);
        view.setUint32(o + 8, 0, true);         // MIDI only
        bytes[o + 12] = 0;
        bytes[o + 13] = track ? track.gate : 50;
        bytes[o + 14] = 0;
        bytes[at + 4097 + t] = STEPS;
      }
      bytes[at + 4096] = STEPS;
      bytes[at + 4161] = 50;
      text(at + 4162, ('P' + (loc + 1)).padEnd(8, ' '), 9);
    }
    bytes.fill(0, SONG_OFFSET, SONG_OFFSET + SONG_BYTES);
    const rows = chain(p, opts && opts.bars);
    rows.forEach((entry, i) => {
      bytes.set(entry.locations, SONG_OFFSET + i * ROW_BYTES);
      bytes[SONG_OFFSET + i * ROW_BYTES + 64] = entry.repeats;
    });
    view.setUint16(SONG_OFFSET + TRAILER, rows.length, true);
    view.setUint32(SONG_OFFSET + TRAILER + 4, 1, true);   // SONG_END_STOP
    text(SONG_OFFSET + TRAILER + 8, p.title.replace(/^\d+\s*/, '').toUpperCase().replace(/[^ -~]/g, ' ').slice(0, 8), 9);
    for (let t = 0; t < NUM_TRACKS; t++) bytes.set(KIT_DEFAULT, KIT_OFFSET + t * KIT_BYTES);
    view.setUint32(0, 0x52425448, true);            // 'RBTH'
    bytes[4] = VERSION;
    bytes.fill(0xff, 5, 8);
    let crc = crc32(0xffffffff, bytes.subarray(PAT_OFFSET, PAT_OFFSET + PATTERN_BYTES * LOCATIONS));
    crc = crc32(crc, bytes.subarray(SONG_OFFSET, SONG_OFFSET + SONG_BYTES));
    crc = ~crc32(crc, bytes.subarray(KIT_OFFSET, KIT_OFFSET + KIT_BYTES * NUM_TRACKS)) >>> 0;
    view.setUint32(8, crc, true);
    return bytes;
  }

  // The panel moves that load it and play the chain: tempo, LOAD, Song mode,
  // back to the Session view, PLAY. Actions in Artist.schedule's form. With
  // opts.moves (mutes()) the artist then performs them with MUTE until the
  // chain, cut at opts.bars (store with the same bars), stops by itself.
  function show(p, start = 600000, opts) {
    const actions = [];
    let t = start;
    const push = a => actions.push({ ...a, t });
    const wait = ms => { t += Math.round(ms * 1000); };
    const press = (ctl, hold, gap) => { push({ ...ctl, down: true }); wait(hold); push({ ...ctl, down: false }); wait(gap); };
    const say = text => push({ op: 'say', kind: 'device', text });
    const VALUE_SW = { op: 'encsw', enc: 1 }, TRACK_SW = { op: 'encsw', enc: 0 };
    const worth = n => n <= 4 ? n : n <= 7 ? 4 + 4 * (n - 4) : 16 + 8 * (n - 7);
    say('VALUE: темп 120 → ' + p.bpm + ' BPM');
    let left = Math.round((p.bpm - 120) * 10);
    const dir = left > 0 ? 1 : -1;
    left = Math.abs(left);
    while (left > 0) {
      wait(110);
      let n = 1;
      while (worth(n + 1) <= left) n++;
      for (let i = 0; i < n; i++) {
        for (let e = 0; e < 4; e++) { push({ op: 'edge', enc: 1, dir }); wait(3); }
        wait(13);
      }
      left -= worth(n);
    }
    wait(300);
    say('VALUE долго → селектор; кнопка 10 — LOAD: «' + p.title + '», ' + p.tracks.length + ' треков');
    press(VALUE_SW, 620, 200);
    press({ op: 'step', index: 9 }, 80, 400);
    const rows = chain(p, opts && opts.bars);
    say('TRACK долго → цепочка песни; кнопка 2 — режим Song (' + rows.length + ' строк, ' +
        rows.reduce((n, e) => n + e.repeats, 0) + ' тактов)');
    press(TRACK_SW, 620, 200);
    press({ op: 'step', index: 1 }, 80, 200);
    press(VALUE_SW, 80, 200);
    press(VALUE_SW, 620, 300);
    say('PLAY: прибор играет песню из своих клипов');
    push({ op: 'play', down: true });
    const playAt = t;
    wait(80);
    push({ op: 'play', down: false });
    if (!opts || !opts.moves) return { actions, playAt };
    return perform(p, actions, playAt, opts);
  }

  // The MUTE part after PLAY. Each move is two changes, each landing on a
  // downbeat: pressed on beat 4 of the bar before (beat 2 when the page
  // turns, beat 2 of the muted bar for the return of a one-bar move) plus
  // 40-160 ms, seeded by the title; at most two keys at once, two columns
  // apart; a page turn of 8 TRACK detents 100 ms after the last press on the
  // old page (the other hand turns while that one lifts). The last press is
  // 60 ms before the downbeat at least, else a beat earlier but after the
  // mute it returns from has landed, else the move is not made.
  const START_US = 23000;           // PLAY to the first step (artist.js START_LATENCY_US)
  const KIND = {
    kick: ['бочка уходит на такт перед новой частью', 'бочка возвращается с сильной долей'],
    drop: ['бочка и бас уходят на два такта перед дропом', 'бочка и бас возвращаются вместе: дроп'],
    hats: ['хэты уходят на четыре такта', 'хэты возвращаются с фразой'],
    snare: ['малый уходит на два такта', 'малый возвращается с фразой'],
  };
  function perform(p, loading, playAt, opts) {
    const stepUs = Math.floor(60000000 / (p.bpm * 96)) * 24, barUs = stepUs * 16, beatUs = stepUs * 4;
    const t0 = playAt + START_US;
    const at = (bar, beat) => t0 + bar * barUs + (beat - 1) * beatUs;
    let seed = [...p.title].reduce((h, c) => Math.imul(h ^ c.charCodeAt(0), 16777619) >>> 0, 2166136261);
    const rnd = () => {
      seed = (seed + 0x6D2B79F5) >>> 0;
      let x = seed;
      x = Math.imul(x ^ (x >>> 15), x | 1);
      x ^= x + Math.imul(x ^ (x >>> 7), x | 61);
      return ((x ^ (x >>> 14)) >>> 0) / 4294967296;
    };
    const name = t => p.tracks[t].name;
    let page = 0;
    // One change landing on bar B: its actions, or null if it does not fit.
    const change = (B, tracks, beat, after, text) => {
      const pages = [...new Set(tracks.map(t => t >> 3))];
      const jitter = Math.round((40 + 120 * rnd()) * 1000);
      for (let b = beat; b >= 1; b--) {
        let t = at(B - 1, b) + jitter;
        if (t < after) continue;
        const out = [{ op: 'say', kind: 'device', text, t }];
        let shown = page, last = 0;
        for (const pg of pages.includes(shown) ? [shown, ...pages.filter(x => x !== shown)] : pages) {
          if (pg !== shown) {
            if (last) t = last + 100000;
            out.push({ op: 'say', kind: 'device', text: 'TRACK: страница ' + (pg + 1), t });
            const dir = pg > shown ? 1 : -1;
            let edge = t;
            for (let d = 0; d < 8; d++) {
              for (let e = 0; e < 4; e++) { edge = t; out.push({ op: 'edge', enc: 0, dir, t }); t += 3000; }
              t += 13000;
            }
            t = edge + 80000;
            shown = pg;
          }
          const cols = tracks.filter(x => x >> 3 === pg).map(x => x & 7).sort((x, y) => x - y);
          for (let i = 0; i < cols.length;) {
            const two = i + 1 < cols.length && cols[i + 1] - cols[i] <= 2 ? 2 : 1;
            for (let k = 0; k < two; k++) {
              out.push({ op: 'mute', track: cols[i + k], down: true, t: t + k * 12000 });
              out.push({ op: 'mute', track: cols[i + k], down: false, t: t + k * 12000 + 80000 });
              last = t + k * 12000;
            }
            i += two;
            t = last + 150000;
          }
        }
        if (last <= at(B, 1) - 60000) return { out, page: shown };
      }
      return null;
    };
    const actions = loading.slice(), kept = [];
    for (const m of opts.moves) {
      // A change turns the page when it needs a page other than the one shown.
      const turns = () => new Set([page, ...m.tracks.map(t => t >> 3)]).size > 1;
      const names = m.tracks.map(name).join(', ');
      const before = page;
      const off = change(m.start, m.tracks, turns() ? 2 : 4, 0, 'MUTE ' + names + ': ' + KIND[m.kind][0] + ', с такта ' + (m.start + 1));
      if (!off) continue;
      page = off.page;
      const back = change(m.end, m.tracks, turns() ? 2 : m.end - m.start === 1 ? 2 : 4, at(m.start, 1) + 100000,
        'MUTE ' + names + ': ' + KIND[m.kind][1] + ' такта ' + (m.end + 1));
      if (!back) { page = before; continue; }
      page = back.page;
      actions.push(...off.out, ...back.out);
      kept.push(m);
    }
    actions.sort((a, b) => a.t - b.t);
    const bars = Math.min(opts.bars === undefined ? p.played : opts.bars, p.played);
    return { actions, playAt, t0, end: t0 + bars * barUs + 1500000, moves: kept };
  }

  const api = { plan, mutes, store, show, layoutFor, layout: layoutFor(384) };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.MidiClips = api;
})(globalThis);
