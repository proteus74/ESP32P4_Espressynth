export const OVERDRIVE_PORTS = ["mode", "drive", "tone", "bias", "mix", "gain"];
export const OVERDRIVE_DEFAULTS = { mode: 0, drive: 5, tone: 7, bias: 5, mix: 10, gain: 10 };
export const OVERDRIVE_MODE_NAMES = ["Soft", "Hard", "Tube", "Fuzz"];

const CV_MAX = 10;
const AUDIO_PEAK = 5;

export function overdriveCv(value, fallback = 0) {
  return typeof value === "number" && Number.isFinite(value) ? Math.min(CV_MAX, Math.max(0, value)) : fallback;
}

// 0-10V stepped into four 2.5V bands (nodes::overdrive::modeIndex).
export function overdriveModeIndex(cv) {
  return Math.min(3, Math.floor(overdriveCv(cv) * 0.4));
}

const logRange = (cv, lo, hi) => lo * (hi / lo) ** (overdriveCv(cv) / CV_MAX);
export const overdriveDriveFactor = (cv) => logRange(cv, 1, 100); // 0..40 dB
export const overdriveToneHz = (cv) => logRange(cv, 500, 12000);

function softClip(x) {
  if (x > 3) return 1;
  if (x < -3) return -1;
  const x2 = x * x;
  return x * (27 + x2) / (27 + 9 * x2);
}

export function overdriveShape(mode, x) {
  switch (mode) {
    case 1: return x > 1 ? 1 : (x < -1 ? -1 : x);
    case 2: return x >= 0 ? x / (1 + x) : 0.5 * softClip(2 * x);
    case 3: return x >= 0 ? 1 - Math.exp(-4 * x) : -(1 - Math.exp(4 * x));
    default: return softClip(x);
  }
}

// Mirrors nodes/OverdriveNode.h render(). inputs = [in, mode, drive, tone, bias, mix, gain].
export function createOverdriveDsp(sampleRate) {
  const onePole = (hz) => 1 - Math.exp(-2 * Math.PI * hz / sampleRate);
  const dcR = 1 - 2 * Math.PI * 10 / sampleRate;
  const preCoeff = onePole(720);
  const levelCoeff = 1 - Math.exp(-1 / (0.05 * sampleRate));
  const levelEps = 1e-6;
  const maxLevelGain = 4;
  let prevIn = 0, preLp = 0, dcX1 = 0, dcY1 = 0, toneZ1 = 0, inPow = 0, wetPow = 0;
  let cachedDrive = -1, cachedTone = -1, driveFactor = 1, toneCoeff = 1;
  return {
    render(inputs, output) {
      const [input, modeCv, driveCv, toneCv, biasCv, mixCv, gainCv] = inputs;
      for (let i = 0; i < output.length; i += 1) {
        const mode = overdriveModeIndex(modeCv[i]);
        const drive = overdriveCv(driveCv[i]);
        const tone = overdriveCv(toneCv[i]);
        if (drive !== cachedDrive) {
          cachedDrive = drive;
          driveFactor = overdriveDriveFactor(drive);
        }
        if (tone !== cachedTone) {
          cachedTone = tone;
          toneCoeff = onePole(overdriveToneHz(tone));
        }
        const dry = input[i];
        const xn = dry / AUDIO_PEAK;
        preLp += preCoeff * (xn - preLp);
        inPow += levelCoeff * (xn * xn - inPow);
        const src = mode === 0 ? xn - preLp : xn;
        const modeBias = mode === 2 ? 0.3 : (mode === 3 ? 0.6 : 0);
        const bias = ((overdriveCv(biasCv[i]) - CV_MAX / 2) * 0.1 + modeBias) * Math.sqrt(2 * inPow);
        const x = (src + bias) * driveFactor;
        let shaped = 0.5 * (overdriveShape(mode, 0.5 * (prevIn + x)) + overdriveShape(mode, x));
        prevIn = x;
        if (mode === 0) shaped += xn;
        const dc = shaped - dcX1 + dcR * dcY1;
        dcX1 = shaped;
        dcY1 = dc;
        toneZ1 += toneCoeff * (dc - toneZ1);
        wetPow += levelCoeff * (toneZ1 * toneZ1 - wetPow);
        const level = Math.min(maxLevelGain, Math.sqrt((inPow + levelEps) / (wetPow + levelEps)));
        const wet = toneZ1 * level * AUDIO_PEAK;
        const mix = overdriveCv(mixCv[i]) / CV_MAX;
        output[i] = (dry + (wet - dry) * mix) * (overdriveCv(gainCv[i]) / CV_MAX);
      }
    },
  };
}
