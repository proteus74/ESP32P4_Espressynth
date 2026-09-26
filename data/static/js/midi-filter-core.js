export function midiFilterCriterion(value, maximum) {
  const number = Number(value);
  return value !== null && value !== "" && typeof value !== "boolean" && Number.isFinite(number) && number >= -1 && number <= maximum
    ? Math.round(number) : -1;
}

export function createMidiFilterState() {
  return { channel: -1, note: -1, voices: 1, overflowMode: "steal-oldest", activeNotes: new Set(), admittedChannels: new Set() };
}

export function acceptsMidiFilter(state, data) {
  if (!Number.isInteger(data.channel) || data.channel < 0 || data.channel > 15) return false;
  const noteEvent = ["note_on", "note_off", "polytouch"].includes(data.midiType);
  if (noteEvent && (!Number.isInteger(data.note) || data.note < 0 || data.note > 127)) return false;
  if (data.midiType === "note_on" && (!Number.isInteger(data.velocity) || data.velocity < 0 || data.velocity > 127)) return false;
  const key = data.channel * 128 + data.note;
  const noteOff = data.midiType === "note_off" || (data.midiType === "note_on" && data.velocity === 0);
  if (noteOff) {
    const admitted = state.activeNotes.delete(key);
    return admitted || ((state.channel < 0 || state.channel === data.channel) && (state.note < 0 || state.note === data.note));
  }
  const release = data.midiType === "control_change" &&
    ((data.control === 64 && data.value < 64) || data.control === 120 || data.control === 123);
  if (state.channel >= 0 && state.channel !== data.channel && !(release && state.admittedChannels.has(data.channel))) return false;
  if (noteEvent && state.note >= 0 && state.note !== data.note) return false;
  if (data.midiType === "note_on") {
    state.activeNotes.add(key);
    state.admittedChannels.add(data.channel);
  }
  if (data.midiType === "control_change" && (data.control === 120 || data.control === 123)) {
    for (const active of state.activeNotes) if (Math.floor(active / 128) === data.channel) state.activeNotes.delete(active);
  }
  return true;
}
