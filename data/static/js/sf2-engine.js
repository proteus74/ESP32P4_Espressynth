

function fourCCAt(view, offset) {
  return String.fromCharCode(
    view.getUint8(offset),
    view.getUint8(offset + 1),
    view.getUint8(offset + 2),
    view.getUint8(offset + 3)
  );
}

function readCString20(view, offset) {
  let name = "";
  for (let i = 0; i < 20; i += 1) {
    const ch = view.getUint8(offset + i);
    if (ch === 0) break;
    name += String.fromCharCode(ch);
  }
  return name;
}

function walkChunks(view, start, end) {
  const chunks = [];
  let offset = start;
  while (offset + 8 <= end) {
    const id = fourCCAt(view, offset);
    const size = view.getUint32(offset + 4, true);
    const dataStart = offset + 8;
    chunks.push({ id, size, dataStart });
    offset = dataStart + size + (size % 2);
  }
  return chunks;
}

function parseRecords(view, start, count, size, readOne) {
  const out = [];
  for (let i = 0; i < count; i += 1) out.push(readOne(view, start + i * size));
  return out;
}

const readPhdr = (view, o) => ({
  presetName: readCString20(view, o),
  preset: view.getUint16(o + 20, true),
  bank: view.getUint16(o + 22, true),
  presetBagNdx: view.getUint16(o + 24, true),
});
const readBag = (view, o) => ({ genNdx: view.getUint16(o, true), modNdx: view.getUint16(o + 2, true) });
const readGen = (view, o) => {
  const raw = view.getUint16(o + 2, true);
  return {
    genOper: view.getUint16(o, true),
    raw,
    signed: raw >= 0x8000 ? raw - 0x10000 : raw,
    lo: raw & 0xff,
    hi: (raw >> 8) & 0xff,
  };
};
const readInst = (view, o) => ({ instName: readCString20(view, o), instBagNdx: view.getUint16(o + 20, true) });
const readShdr = (view, o) => ({
  sampleName: readCString20(view, o),
  start: view.getUint32(o + 20, true),
  end: view.getUint32(o + 24, true),
  startLoop: view.getUint32(o + 28, true),
  endLoop: view.getUint32(o + 32, true),
  sampleRate: view.getUint32(o + 36, true),
  originalPitch: view.getUint8(o + 40),
  pitchCorrection: view.getInt8(o + 41),
});

function parsePdta(view, start, end) {
  const hydra = {};
  const specs = {
    phdr: [38, readPhdr],
    pbag: [4, readBag],
    pgen: [4, readGen],
    inst: [22, readInst],
    ibag: [4, readBag],
    igen: [4, readGen],
    shdr: [46, readShdr],
  };
  walkChunks(view, start, end).forEach((chunk) => {
    const spec = specs[chunk.id];
    if (!spec) return; 
    const [size, reader] = spec;
    hydra[chunk.id] = parseRecords(view, chunk.dataStart, Math.floor(chunk.size / size), size, reader);
  });
  return hydra;
}

function parseSdta(view, start, end) {
  let samples = new Float32Array(0);
  walkChunks(view, start, end).forEach((chunk) => {
    if (chunk.id !== "smpl") return; 
    const count = Math.floor(chunk.size / 2);
    samples = new Float32Array(count);
    for (let i = 0; i < count; i += 1) samples[i] = view.getInt16(chunk.dataStart + i * 2, true) / 32768;
  });
  return samples;
}


const GEN_INSTRUMENT = 41;
const GEN_SAMPLEID = 53;

const GEN_TABLE = {
  0: ["uintAdd", "offset"],
  1: ["uintAdd", "end"],
  2: ["uintAdd", "loop_start"],
  3: ["uintAdd", "loop_end"],
  4: ["uintAdd15", "offset"],
  8: ["int", "initialFilterFc"],
  9: ["int", "initialFilterQ"],
  12: ["uintAdd15", "end"],
  17: ["float", "pan"],
  33: ["float", "ampenv.delay"],
  34: ["float", "ampenv.attack"],
  35: ["float", "ampenv.hold"],
  36: ["float", "ampenv.decay"],
  37: ["float", "ampenv.sustain"],
  38: ["float", "ampenv.release"],
  43: ["keyrange"],
  44: ["velrange"],
  45: ["uintAdd15", "loop_start"],
  48: ["float", "attenuation"],
  50: ["uintAdd15", "loop_end"],
  51: ["int", "transpose"],
  52: ["int", "tune"],
  54: ["loopmode"],
  56: ["int", "pitch_keytrack"],
  57: ["group"],
  58: ["keycenter"],
};

function getField(region, path) {
  return path.indexOf(".") < 0 ? region[path] : region[path.split(".")[0]][path.split(".")[1]];
}
function setField(region, path, value) {
  if (path.indexOf(".") < 0) region[path] = value;
  else region[path.split(".")[0]][path.split(".")[1]] = value;
}

function applyGenerator(region, gen) {
  const entry = GEN_TABLE[gen.genOper];
  if (!entry) return;
  const [kind, field] = entry;
  switch (kind) {
    case "float":
    case "int":
      setField(region, field, gen.signed);
      return;
    case "uintAdd":
      setField(region, field, getField(region, field) + gen.signed);
      return;
    case "uintAdd15":
      setField(region, field, getField(region, field) + (gen.signed << 15));
      return;
    case "keyrange":
      region.lokey = gen.lo;
      region.hikey = gen.hi;
      return;
    case "velrange":
      region.lovel = gen.lo;
      region.hivel = gen.hi;
      return;
    case "loopmode":
      region.loop_mode = (gen.raw & 3) === 3 || (gen.raw & 3) === 1 ? 1 : 0; 
      return;
    case "group":
      region.group = gen.raw;
      return;
    case "keycenter":
      region.pitch_keycenter = gen.signed;
      return;
    default:
  }
}

function clampNum(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

function clearRegion(forRelative) {
  const r = {
    loop_mode: 0,
    sample_rate: 0,
    lokey: 0,
    hikey: 127,
    lovel: 0,
    hivel: 127,
    group: 0,
    offset: 0,
    end: 0,
    loop_start: 0,
    loop_end: 0,
    transpose: 0,
    tune: 0,
    pitch_keycenter: 60,
    pitch_keytrack: 0,
    attenuation: 0,
    pan: 0,
    ampenv: { delay: 0, attack: 0, hold: 0, decay: 0, sustain: 0, release: 0 },
    initialFilterQ: 0,
    initialFilterFc: 0,
  };
  if (forRelative) return r;
  r.pitch_keytrack = 100;
  r.pitch_keycenter = -1;
  r.ampenv.delay = r.ampenv.attack = r.ampenv.hold = r.ampenv.decay = r.ampenv.release = -12000;
  r.initialFilterFc = 13500;
  return r;
}

function cloneRegion(region) {
  return { ...region, ampenv: { ...region.ampenv } };
}




function mergeRegion(zone, preset) {
  zone.offset += preset.offset;
  zone.end += preset.end;
  zone.loop_start += preset.loop_start;
  zone.loop_end += preset.loop_end;

  zone.transpose += preset.transpose;
  zone.tune += preset.tune;
  zone.pitch_keytrack += preset.pitch_keytrack;

  zone.initialFilterFc = clampNum(zone.initialFilterFc + preset.initialFilterFc, 1500, 13500);
  zone.initialFilterQ = clampNum(zone.initialFilterQ + preset.initialFilterQ, 0, 960);
  zone.pan = clampNum((zone.pan + preset.pan) * 0.001, -0.5, 0.5);
  zone.attenuation = clampNum((zone.attenuation + preset.attenuation) * 0.01, 0, 14.4);

  const env = (field, vmin, vmax) => {
    zone.ampenv[field] = clampNum(zone.ampenv[field] + preset.ampenv[field], vmin, vmax);
  };
  env("delay", -12000, 5000);
  env("attack", -12000, 8000);
  env("hold", -12000, 5000);
  env("decay", -12000, 8000);
  env("sustain", 0, 1440);
  env("release", -12000, 8000);
}

export function timecents2Secs(timecents) {
  return 2 ** (timecents / 1200);
}
export function cents2Hertz(cents) {
  return 8.176 * 2 ** (cents / 1200);
}
export function decibelsToGain(db) {
  return db > -100 ? 10 ** (db * 0.05) : 0;
}

function envToSecs(env) {
  env.delay = env.delay < -11950 ? 0 : timecents2Secs(env.delay);
  env.attack = env.attack < -11950 ? 0 : timecents2Secs(env.attack);
  env.hold = env.hold < -11950 ? 0 : timecents2Secs(env.hold);
  env.decay = env.decay < -11950 ? 0 : timecents2Secs(env.decay);
  env.release = env.release < -11950 ? 0 : timecents2Secs(env.release);
  env.sustain = env.sustain < 0 ? 0 : decibelsToGain(-env.sustain / 10);
}

function buildPresetRegions(hydra, phdrIndex, sampleCount) {
  const pphdr = hydra.phdr[phdrIndex];
  const pphdrNext = hydra.phdr[phdrIndex + 1];
  const regions = [];
  let globalRegion = clearRegion(true);

  for (let pbagIdx = pphdr.presetBagNdx; pbagIdx < pphdrNext.presetBagNdx; pbagIdx += 1) {
    const pbag = hydra.pbag[pbagIdx];
    const pbagNext = hydra.pbag[pbagIdx + 1];
    let presetRegion = cloneRegion(globalRegion);
    let hadGenInstrument = false;

    for (let pgenIdx = pbag.genNdx; pgenIdx < pbagNext.genNdx; pgenIdx += 1) {
      const pgen = hydra.pgen[pgenIdx];
      if (pgen.genOper === GEN_INSTRUMENT) {
        let instRegion = clearRegion(false);
        const pinst = hydra.inst[pgen.raw];
        const pinstNext = hydra.inst[pgen.raw + 1];
        if (!pinst || !pinstNext) { hadGenInstrument = true; continue; }

        for (let ibagIdx = pinst.instBagNdx; ibagIdx < pinstNext.instBagNdx; ibagIdx += 1) {
          const ibag = hydra.ibag[ibagIdx];
          const ibagNext = hydra.ibag[ibagIdx + 1];
          const zoneRegion = cloneRegion(instRegion);
          let hadSampleID = false;

          for (let igenIdx = ibag.genNdx; igenIdx < ibagNext.genNdx; igenIdx += 1) {
            const igen = hydra.igen[igenIdx];
            if (igen.genOper === GEN_SAMPLEID) {
              if (zoneRegion.hikey < presetRegion.lokey || zoneRegion.lokey > presetRegion.hikey) continue;
              if (zoneRegion.hivel < presetRegion.lovel || zoneRegion.lovel > presetRegion.hivel) continue;
              if (presetRegion.lokey > zoneRegion.lokey) zoneRegion.lokey = presetRegion.lokey;
              if (presetRegion.hikey < zoneRegion.hikey) zoneRegion.hikey = presetRegion.hikey;
              if (presetRegion.lovel > zoneRegion.lovel) zoneRegion.lovel = presetRegion.lovel;
              if (presetRegion.hivel < zoneRegion.hivel) zoneRegion.hivel = presetRegion.hivel;

              mergeRegion(zoneRegion, presetRegion);
              envToSecs(zoneRegion.ampenv);

              const pshdr = hydra.shdr[igen.raw];
              if (pshdr) {
                zoneRegion.offset += pshdr.start;
                zoneRegion.end += pshdr.end;
                zoneRegion.loop_start += pshdr.startLoop;
                zoneRegion.loop_end += pshdr.endLoop;
                if (pshdr.endLoop > 0) zoneRegion.loop_end -= 1;
                if (zoneRegion.loop_end > sampleCount) zoneRegion.loop_end = sampleCount;
                if (zoneRegion.pitch_keycenter === -1) zoneRegion.pitch_keycenter = pshdr.originalPitch;
                zoneRegion.tune += pshdr.pitchCorrection;
                zoneRegion.sample_rate = pshdr.sampleRate;
              }
              if (zoneRegion.end && zoneRegion.end < sampleCount) zoneRegion.end += 1;
              else zoneRegion.end = sampleCount;

              regions.push(zoneRegion);
              hadSampleID = true;
            } else {
              applyGenerator(zoneRegion, igen);
            }
          }
          if (ibagIdx === pinst.instBagNdx && !hadSampleID) instRegion = zoneRegion;
        }
        hadGenInstrument = true;
      } else {
        applyGenerator(presetRegion, pgen);
      }
    }
    if (pbagIdx === pphdr.presetBagNdx && !hadGenInstrument) globalRegion = presetRegion;
  }
  return regions;
}




export function parseSf2(buffer) {
  const view = new DataView(buffer);
  if (fourCCAt(view, 0) !== "RIFF") throw new Error("Not a RIFF file");
  const riffSize = view.getUint32(4, true);
  if (fourCCAt(view, 8) !== "sfbk") throw new Error("Not a SoundFont2 (sfbk) file");
  const fileEnd = Math.min(buffer.byteLength, 8 + riffSize);

  let sampleData = new Float32Array(0);
  let hydra = null;
  walkChunks(view, 12, fileEnd).forEach((chunk) => {
    if (chunk.id !== "LIST") return;
    const listType = fourCCAt(view, chunk.dataStart);
    const listStart = chunk.dataStart + 4;
    const listEnd = chunk.dataStart + chunk.size;
    if (listType === "sdta") sampleData = parseSdta(view, listStart, listEnd);
    else if (listType === "pdta") hydra = parsePdta(view, listStart, listEnd);
  });

  if (!hydra || !hydra.phdr || hydra.phdr.length < 2) throw new Error("SoundFont has no presets");

  const presets = [];
  for (let i = 0; i < hydra.phdr.length - 1; i += 1) {
    const phdr = hydra.phdr[i];
    presets.push({
      bank: phdr.bank,
      program: phdr.preset,
      name: phdr.presetName || `Preset ${phdr.bank}:${phdr.preset}`,
      regions: buildPresetRegions(hydra, i, sampleData.length),
    });
  }
  return { presets, sampleData };
}

export function findPreset(presets, bank, program) {
  return presets.find((p) => p.bank === bank && p.program === program) || null;
}




export function resolvePreset(presets, bank, program, isDrum) {
  let preset = null;
  if (isDrum) {
    preset = findPreset(presets, 128 | (bank & 0x7fff), program);
    if (!preset) preset = findPreset(presets, 128, program);
    if (!preset) preset = findPreset(presets, 128, 0);
    if (!preset) preset = findPreset(presets, bank & 0x7fff, program);
  } else {
    preset = findPreset(presets, bank & 0x7fff, program);
  }
  if (!preset) preset = findPreset(presets, 0, program);
  return preset;
}

export function getPresetName(presets, bank, program) {
  const preset = findPreset(presets, bank, program);
  return preset ? preset.name : null;
}

export function getMaxBank(presets) {
  return presets.reduce((max, p) => Math.max(max, p.bank), 0);
}
