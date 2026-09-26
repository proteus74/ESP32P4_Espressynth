export const COMB_FILTER_PORTS = ["time", "feedback", "mix", "polarity", "damping", "gain"];
export const COMB_FILTER_DEFAULTS = { time: 0.5, feedback: 5, mix: 5, polarity: 0, damping: 10, gain: 10 };

export function combFilterCv(value, fallback = 0) {
  return typeof value === "number" && Number.isFinite(value) ? Math.min(10, Math.max(0, value)) : fallback;
}

// Matches nodes/CombFilterNode.h, including the one-sample minimum and 5ms smoothing.
export function createCombFilterDsp(entry, sampleRate) {
  const buffer = new Float32Array(Math.floor(sampleRate / 10) + 2);
  const smoothing = 1 - Math.exp(-1 / (0.005 * sampleRate));
  let writeIndex = 0;
  let initialized = false;
  let delaySamples = 1, feedback = 0, mix = 0.5, gain = 1, dampingCoeff = 0, dampingState = 0;
  let lastDamping = -1, targetDampingCoeff = 0;
  const flush = (v) => Math.abs(v) < 1e-15 ? 0 : v;
  const delayFor = (v) => Math.max(1, combFilterCv(v) * 0.01 * sampleRate);
  const feedbackFor = (v, polarity) => combFilterCv(v) * 0.095 * (combFilterCv(polarity) >= 5 ? -1 : 1);
  const dampingFor = (v) => v >= 10 ? 0 : Math.exp(-2 * Math.PI * 20 * (Math.min(20000, sampleRate * 0.45) / 20) ** (v / 10) / sampleRate);
  return {
    render(inputs, output) {
      const timeTarget = delayFor(inputs[1][0]);
      const feedbackTarget = feedbackFor(inputs[2][0], inputs[4][0]);
      const mixTarget = combFilterCv(inputs[3][0]) / 10;
      const damping = combFilterCv(inputs[5][0]);
      if (damping !== lastDamping) {
        lastDamping = damping;
        targetDampingCoeff = dampingFor(damping);
      }
      if (!initialized) {
        delaySamples = timeTarget;
        feedback = feedbackTarget;
        mix = mixTarget;
        gain = combFilterCv(inputs[6][0]) / 10;
        dampingCoeff = targetDampingCoeff;
        initialized = true;
      }
      for (let i = 0; i < output.length; i += 1) {
        delaySamples += smoothing * (timeTarget - delaySamples);
        feedback += smoothing * (feedbackTarget - feedback);
        mix += smoothing * (mixTarget - mix);
        dampingCoeff += smoothing * (targetDampingCoeff - dampingCoeff);
        gain += smoothing * (combFilterCv(inputs[6][i]) / 10 - gain);
        const delay = delaySamples - 1;
        const integer = Math.floor(delay);
        const fraction = delay - integer;
        const index = (writeIndex + buffer.length - 1 - integer) % buffer.length;
        const older = index === 0 ? buffer.length - 1 : index - 1;
        const wet = buffer[index] + (buffer[older] - buffer[index]) * fraction;
        dampingState = flush(wet * (1 - dampingCoeff) + dampingState * dampingCoeff);
        buffer[writeIndex] = flush(inputs[0][i] + feedback * dampingState);
        writeIndex = (writeIndex + 1) % buffer.length;
        output[i] = (inputs[0][i] * (1 - mix) + wet * mix) * gain;
      }
    },
    response(freqs, mag) {
      const n = initialized ? delaySamples : delayFor(entry.time);
      const f = initialized ? feedback : feedbackFor(entry.feedback, entry.polarity);
      const m = initialized ? mix : combFilterCv(entry.mix) / 10;
      const g = initialized ? gain : combFilterCv(entry.gain) / 10;
      const a = initialized ? dampingCoeff : dampingFor(combFilterCv(entry.damping));
      const integer = Math.floor(n), fraction = n - integer;
      for (let i = 0; i < freqs.length; i += 1) {
        const w = 2 * Math.PI * freqs[i] / sampleRate;
        // Fractional delay D and one-pole feedback lowpass L.
        const dr = (1 - fraction) * Math.cos(w * integer) + fraction * Math.cos(w * (integer + 1));
        const di = -(1 - fraction) * Math.sin(w * integer) - fraction * Math.sin(w * (integer + 1));
        const lrDen = 1 - a * Math.cos(w), liDen = a * Math.sin(w);
        const lNorm = lrDen * lrDen + liDen * liDen;
        const lr = (1 - a) * lrDen / lNorm, li = -(1 - a) * liDen / lNorm;
        const denR = 1 - f * (lr * dr - li * di), denI = -f * (lr * di + li * dr);
        const norm = denR * denR + denI * denI;
        const wetR = (dr * denR + di * denI) / norm, wetI = (di * denR - dr * denI) / norm;
        mag[i] = g * Math.hypot(1 - m + m * wetR, m * wetI);
      }
    },
  };
}
