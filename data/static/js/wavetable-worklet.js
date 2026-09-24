// AudioWorkletProcessor for the Wavetable widget (see audio-engine.js,
// case "lfo"/"wavetable"). Emits the raw -1..+1 wavetable signal; the LFO chain
// (gain/one-shot/global) in audio-engine.js scales it into
// the 0-10 V CV window exactly as it does for the classic waveforms.
//
// AudioParams:
//   frequency   - LFO rate in Hz (a-rate)
//   morph       - table position 0-10 V (a-rate, modulatable: LFOs/
//                 envelopes can be connected straight to this param)
//   phaseOffset - phase offset in cycles (k-rate)
// Messages (port.postMessage):
//   { type: "tables", tables: Float32Array[] }  - replace the bank
//   { type: "reset" }                           - restart at phase 0
import { WavetableLfoCore } from "./wavetable-core.js";

class WavetableLfoProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [
      { name: "frequency", defaultValue: 2, minValue: 0, maxValue: 1000, automationRate: "a-rate" },
      { name: "morph", defaultValue: 0, minValue: 0, maxValue: 10, automationRate: "a-rate" },
      { name: "phaseOffset", defaultValue: 0, minValue: -1, maxValue: 1, automationRate: "k-rate" },
    ];
  }

  constructor() {
    super();
    this.core = new WavetableLfoCore(sampleRate);
    this.port.onmessage = (event) => {
      const msg = event.data || {};
      if (msg.type === "tables") this.core.setTables(msg.tables);
      else if (msg.type === "reset") this.core.reset();
    };
  }

  process(inputs, outputs, parameters) {
    const out = outputs[0] && outputs[0][0];
    if (!out) return true;
    this.core.process(out, parameters.frequency, parameters.morph, parameters.phaseOffset[0]);
    for (let ch = 1; ch < outputs[0].length; ch += 1) outputs[0][ch].set(out);
    return true;
  }
}

registerProcessor("wavetable-lfo", WavetableLfoProcessor);
