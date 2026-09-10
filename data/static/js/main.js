import { AudioEngine } from "./audio-engine.js?v=20260909-link-output-levels-8";
import { getPresetName, getMaxBank } from "./sf2-engine.js";

const GRID = 20;
const LINK_BUS_COUNT = 8;
const SOUNDFONT_DEFAULT_MAX_BANK = 128;
const audioEngine = new AudioEngine();
window.audioEngine = audioEngine; // debug: inspect from DevTools console (module scope isn't global)



const menuItems = document.querySelectorAll(".menu-item:not(.menu-item-static)");

menuItems.forEach((item) => {
  item.addEventListener("click", (e) => {
    e.stopPropagation();
    const wasOpen = item.classList.contains("open");
    menuItems.forEach((m) => m.classList.remove("open"));
    if (!wasOpen) item.classList.add("open");
  });
});

document.addEventListener("click", () => {
  menuItems.forEach((m) => m.classList.remove("open"));
});

document.addEventListener("click", (e) => {
  if (e.target.classList?.contains("modal-overlay")) {
    e.stopPropagation();
  }
}, true);

document.addEventListener("keydown", (e) => {
  if (e.key !== "Escape") return;
  const openOverlays = Array.from(document.querySelectorAll(".modal-overlay:not(.hidden)"));
  const overlay = openOverlays.at(-1);
  if (!overlay) return;
  const closeButton = overlay.querySelector('[id$="Cancel"], [id$="Close"]');
  if (closeButton) closeButton.click();
  e.preventDefault();
  e.stopPropagation();
}, true);



const infoOverlay = document.getElementById("infoOverlay");
const infoVersionValue = document.getElementById("infoVersionValue");

async function openInfo() {
  infoVersionValue.textContent = "–";
  infoOverlay.classList.remove("hidden");
  try {
    const res = await fetch("/api/version");
    const data = await res.json();
    infoVersionValue.textContent = data.version || "unknown";
  } catch (err) {
    infoVersionValue.textContent = "unknown";
  }
}

function closeInfo() {
  infoOverlay.classList.add("hidden");
}



async function applyWm8960Availability() {
  let wm8960 = false;
  try {
    const res = await fetch("/api/version");
    const data = await res.json();
    wm8960 = data.wm8960 === true;
  } catch (err) {
    wm8960 = false;
  }
  if (!wm8960) {
    const cat = WIDGET_CATEGORIES.find((c) => c.label === "Mixing/Output");
    if (cat) {
      const idx = cat.types.indexOf("wm8960-out");
      if (idx >= 0) cat.types.splice(idx, 1);
    }
    globalThis.__wm8960Enabled = false;
  } else {
    globalThis.__wm8960Enabled = true;
  }
}

applyWm8960Availability();

document.getElementById("menubarLogo").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  openInfo();
});

document.getElementById("infoClose").addEventListener("click", closeInfo);



const toast = document.getElementById("toast");
let toastTimer = null;

function showToast(message) {
  toast.textContent = message;
  toast.classList.remove("hidden");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.add("hidden"), 2000);
}



const presetLoadProgress = document.getElementById("presetLoadProgress");
const presetLoadProgressLabel = document.getElementById("presetLoadProgressLabel");
const presetLoadProgressFill = document.getElementById("presetLoadProgressFill");



function showPresetLoadProgressIndeterminate(label) {
  presetLoadProgressLabel.textContent = label;
  presetLoadProgress.classList.remove("hidden");
  presetLoadProgress.classList.add("indeterminate");
}

function updatePresetLoadProgressDeterminate(done, total) {
  presetLoadProgress.classList.remove("hidden", "indeterminate");
  presetLoadProgressLabel.textContent = `Loading samples… ${done}/${total}`;
  presetLoadProgressFill.style.width = `${Math.round((done / total) * 100)}%`;
}

function hidePresetLoadProgress() {
  presetLoadProgress.classList.add("hidden");
  presetLoadProgress.classList.remove("indeterminate");
  presetLoadProgressFill.style.width = "0%";
}





function pollAssetLoadProgress() {
  const { done, total } = audioEngine.getLoadProgress();
  if (total === 0) {
    hidePresetLoadProgress();
    return;
  }
  updatePresetLoadProgressDeterminate(done, total);
  if (done >= total) {
    setTimeout(hidePresetLoadProgress, 300); 
    return;
  }
  requestAnimationFrame(pollAssetLoadProgress);
}

const presetSelect = document.getElementById("presetSelect");
presetSelect.addEventListener("change", () => {
  loadPresetByName(presetSelect.value, { activate: true });
});

document.getElementById("menuNew").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  handleNewPreset();
});

document.getElementById("menuSave").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  handleExplicitSave();
});

document.getElementById("menuSaveAs").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  handleSaveAs();
});

document.getElementById("menuExport").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  exportPresetToFile();
});

const presetImportInput = document.getElementById("presetImportInput");

document.getElementById("menuImport").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  presetImportInput.value = ""; 
  presetImportInput.click();
});

presetImportInput.addEventListener("change", () => {
  const file = presetImportInput.files[0];
  if (file) importPresetFromFile(file);
});

document.getElementById("menuFileExplorer").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  openFileExplorer();
});

const bleConnectOverlay = document.getElementById("bleConnectOverlay");
const bleConnectStatus = document.getElementById("bleConnectStatus");
const bleDeviceList = document.getElementById("bleDeviceList");
const bleScanButton = document.getElementById("bleScan");

let bleActionBusy = false;

function renderBleDevices(data) {
  if (!data.available) bleConnectStatus.textContent = data.error || "BLE unavailable";
  else if (data.connected) bleConnectStatus.textContent = `Connected: ${data.name}`;
  else if (data.connecting) bleConnectStatus.textContent = "Connecting…";
  else if (data.scanning) bleConnectStatus.textContent = "Scanning Bluetooth LE devices…";
  else bleConnectStatus.textContent = data.error || "Select a BLE-MIDI keyboard to connect";
  bleDeviceList.replaceChildren();
  for (const device of (data.devices || [])) {
    const row = document.createElement("div");
    row.className = "ble-device-row";
    const isTarget = data.connectedId && device.id === data.connectedId;
    if (isTarget && data.connected) row.classList.add("ble-device-connected");
    else if (isTarget && data.connecting) row.classList.add("ble-device-connecting");
    if (bleActionBusy) row.classList.add("ble-device-busy");
    const label = document.createElement("span");
    
    
    let suffix = " · BLE-MIDI";
    if (isTarget && data.connected) suffix = " · connected";
    else if (isTarget && data.connecting) suffix = " · connecting…";
    label.textContent = `${device.name} · ${device.id} · ${device.rssi} dBm${suffix}`;
    row.appendChild(label);
    if (!bleActionBusy && !(isTarget && (data.connected || data.connecting))) {
      row.addEventListener("click", () => connectBleDevice(device.id));
    }
    bleDeviceList.appendChild(row);
  }
}

async function refreshBleDevices() {
  const res = await fetch("/api/ble/devices");
  const data = await res.json();
  renderBleDevices(data);
  return data;
}

async function connectBleDevice(id) {
  bleActionBusy = true;
  try {
    const res = await fetch("/api/ble/connect", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id }),
    });
    const data = await res.json();
    if (data.error) throw new Error(data.error);
    for (let i = 0; i < 20; ++i) {
      await new Promise((resolve) => setTimeout(resolve, 500));
      const state = await refreshBleDevices();
      if (!state.connecting) break;
    }
  } catch (err) { bleConnectStatus.textContent = `Error: ${err.message}`; }
  finally { bleActionBusy = false; await refreshBleDevices(); }
}

document.getElementById("menuBleConnect").addEventListener("click", async (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  bleConnectOverlay.classList.remove("hidden");
  try { await refreshBleDevices(); } catch (err) { bleConnectStatus.textContent = `Error: ${err.message}`; }
});

document.getElementById("bleConnectClose").addEventListener("click", () => bleConnectOverlay.classList.add("hidden"));
bleConnectOverlay.addEventListener("click", (e) => { if (e.target === bleConnectOverlay) bleConnectOverlay.classList.add("hidden"); });
bleScanButton.addEventListener("click", async () => {
  bleScanButton.disabled = true;
  try {
    const res = await fetch("/api/ble/scan", { method: "POST" });
    const data = await res.json();
    if (data.error) throw new Error(data.error);
    for (let i = 0; i < 9; ++i) {
      await new Promise((resolve) => setTimeout(resolve, 750));
      const scan = await refreshBleDevices();
      if (!scan.scanning) break;
    }
  } catch (err) { bleConnectStatus.textContent = `Error: ${err.message}`; }
  finally { bleScanButton.disabled = false; }
});



const audioStartBtn = document.getElementById("audioStartBtn");
audioStartBtn.addEventListener("click", () => {
  
  
  
  
  
  if (!audioEngine.isRunning()) audioEngine.resume();
  const muted = !audioEngine.isMasterMuted();
  audioEngine.setMasterMuted(muted);
  audioStartBtn.textContent = muted ? "🔇" : "🔊";
  audioStartBtn.classList.toggle("active", !muted);
  audioStartBtn.title = muted ? "Start audio" : "Mute audio";
});



const settingsOverlay = document.getElementById("settingsOverlay");
const ethDhcpInput = document.getElementById("ethDhcpInput");
const ethStaticFields = document.getElementById("ethStaticFields");
const ethIpInput = document.getElementById("ethIpInput");
const ethMaskInput = document.getElementById("ethMaskInput");
const ethGwInput = document.getElementById("ethGwInput");
const ethDnsInput = document.getElementById("ethDnsInput");
const wifiEnabledInput = document.getElementById("wifiEnabledInput");
const wifiSsidInput = document.getElementById("wifiSsidInput");
const wifiPasswordInput = document.getElementById("wifiPasswordInput");
const wifiScanButton = document.getElementById("wifiScan");
const wifiStatus = document.getElementById("wifiStatus");
const wifiNetworks = document.getElementById("wifiNetworks");

function updateStaticFieldsVisibility() {
  ethStaticFields.hidden = ethDhcpInput.checked;
}

function renderWifiStatus(data) {
  if (!data.wifiAvailable && !data.available) wifiStatus.textContent = "Wi-Fi unavailable on this board";
  else if (data.wifiConnected || data.connected) wifiStatus.textContent = `Connected${data.wifiIp || data.ip ? ` — ${data.wifiIp || data.ip}` : ""}`;
  else if (data.wifiScanning || data.scanning) wifiStatus.textContent = "Scanning…";
  else if (data.wifiError || data.error) wifiStatus.textContent = `Error: ${data.wifiError || data.error}`;
  else wifiStatus.textContent = data.wifiEnabled || data.enabled ? "Connecting…" : "Wi-Fi disabled";
}

ethDhcpInput.addEventListener("change", updateStaticFieldsVisibility);

async function openSettings() {
  settingsOverlay.classList.remove("hidden");
  try {
    const res = await fetch("/api/settings/network");
    const data = await res.json();
    ethDhcpInput.checked = data.ethDhcp !== false;
    ethIpInput.value = data.ethIp || "";
    ethMaskInput.value = data.ethMask || "";
    ethGwInput.value = data.ethGw || "";
    ethDnsInput.value = data.ethDns || "";
    wifiEnabledInput.checked = data.wifiEnabled === true;
    wifiSsidInput.value = data.wifiSsid || "";
    wifiPasswordInput.value = "";
    renderWifiStatus(data);
  } catch (err) {
    appendLog(`Could not load network settings: ${err.message}`);
  }
  updateStaticFieldsVisibility();
  ethDhcpInput.focus();
}

function closeSettings() {
  settingsOverlay.classList.add("hidden");
}

document.getElementById("menuSettings").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  openSettings();
});

document.getElementById("settingsCancel").addEventListener("click", closeSettings);

wifiScanButton.addEventListener("click", async () => {
  wifiScanButton.disabled = true;
  wifiNetworks.hidden = true;
  renderWifiStatus({ scanning: true });
  try {
    const start = await fetch("/api/wifi/scan", { method: "POST" });
    const started = await start.json();
    if (!start.ok || started.error) throw new Error(started.error || `HTTP ${start.status}`);
    let scan;
    for (let attempt = 0; attempt < 8; ++attempt) {
      await new Promise((resolve) => setTimeout(resolve, 750));
      const res = await fetch("/api/wifi/scan");
      scan = await res.json();
      if (!scan.scanning) break;
    }
    renderWifiStatus(scan || {});
    wifiNetworks.replaceChildren();
    for (const network of (scan?.networks || [])) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "wifi-network-row";
      button.textContent = `${network.ssid} (${network.rssi} dBm)${network.auth ? " 🔒" : ""}`;
      button.addEventListener("click", () => {
        wifiSsidInput.value = network.ssid;
        wifiPasswordInput.focus();
      });
      wifiNetworks.appendChild(button);
    }
    wifiNetworks.hidden = (scan?.networks || []).length === 0;
  } catch (err) {
    appendLog(`Could not scan Wi-Fi networks: ${err.message}`);
    renderWifiStatus({ error: err.message });
  } finally {
    wifiScanButton.disabled = false;
  }
});

document.getElementById("settingsSave").addEventListener("click", async () => {
  const body = {
    ethDhcp: ethDhcpInput.checked,
    ethIp: ethIpInput.value.trim(),
    ethMask: ethMaskInput.value.trim(),
    ethGw: ethGwInput.value.trim(),
    ethDns: ethDnsInput.value.trim(),
    wifiEnabled: wifiEnabledInput.checked,
    wifiSsid: wifiSsidInput.value.trim(),
    wifiPassword: wifiPasswordInput.value,
  };
  try {
    const res = await fetch("/api/settings/network", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const result = await res.json();
    if (result.error) throw new Error(result.error);
    closeSettings();
    showToast(result.applied === false ? "Saved; Wi-Fi could not be applied" : "Saved — network changes applied");
  } catch (err) {
    appendLog(`Could not save network settings: ${err.message}`);
    showToast("Could not save network settings");
  }
});

settingsOverlay.addEventListener("click", (e) => {
  if (e.target === settingsOverlay) closeSettings();
});



const saveAsOverlay = document.getElementById("saveAsOverlay");
const saveAsNameInput = document.getElementById("saveAsNameInput");
let saveAsPending = false;

function openSaveAsModal() {
  saveAsPending = true;
  saveAsNameInput.value = currentPresetName || "";
  saveAsOverlay.classList.remove("hidden");
  saveAsNameInput.focus();
  saveAsNameInput.select();
}

function closeSaveAsModal() {
  saveAsOverlay.classList.add("hidden");
  saveAsPending = false;
}

function handleSaveAs() {
  openSaveAsModal();
}

saveAsOverlay.addEventListener("click", (e) => {
  if (e.target === saveAsOverlay) closeSaveAsModal();
});

document.getElementById("saveAsCancel").addEventListener("click", closeSaveAsModal);

document.getElementById("saveAsSave").addEventListener("click", async () => {
  if (!saveAsPending) return;
  const name = saveAsNameInput.value.trim();
  if (!name) return;
  const existing = await fetchPresetList();
  if (existing.includes(name) && !confirm(`Preset "${name}" already exists. Overwrite?`)) {
    return;
  }
  const ok = await saveToServer(name);
  closeSaveAsModal();
  if (ok) {
    currentPresetName = name;
    await refreshPresetMenu();
    showToast("Preset saved");
  } else {
    appendLog("Preset could not be saved.");
  }
});



function exportPresetToFile() {
  const json = JSON.stringify(serializePreset(), null, 2);
  const blob = new Blob([json], { type: "application/json" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = `${currentPresetName || "preset"}.json`;
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
  showToast("Preset exported");
}

async function importPresetFromFile(file) {
  let data;
  try {
    data = JSON.parse(await file.text());
  } catch (err) {
    appendLog(`Preset file is not valid JSON: ${err.message}`);
    showToast("Invalid preset file");
    return;
  }
  if (!data || typeof data !== "object") {
    appendLog("Preset file has an unexpected format.");
    showToast("Invalid preset file");
    return;
  }

  
  
  
  
  
  
  
  
  currentPresetName = null;
  await applyPresetData(data);
  await savePresetNow();
  await refreshPresetMenu();
  showToast("Preset imported");
}



const WIDGET_TYPES = {
  annotation: {
    label: "Annotation",
    icon: "📝",
    inputs: [],
    outputs: [],
  },
  debug: {
    label: "Debug",
    icon: "🐞",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
  "midi-in": {
    label: "Midi-In",
    icon: "🎹",
    inputs: [
      { id: "voices", label: "Voices" },
      { id: "glide", label: "Glide" },
      { id: "glideTime", label: "Glide Time" },
    ],
    outputs: [
      { id: "midi", label: "MIDI" },
      { id: "pitchCv", label: "Pitch" },
      { id: "note", label: "Note (0–127)" },
      { id: "velocity", label: "Velocity" },
      { id: "gate", label: "Gate" },
    ],
  },
  "midi-split": {
    label: "Midi-Split",
    icon: "✂️",
    
    
    
    
    
    inputs: [{ id: "midi", label: "MIDI" }],
    
    
    
    outputs: [],
  },
  knob: {
    label: "Knob",
    icon: "🎛️",
    inputs: [],
    outputs: [{ id: "voltage", label: "" }],
  },
  button: {
    label: "Button",
    icon: "🔘",
    inputs: [],
    outputs: [{ id: "value", label: "" }],
  },
  "up-down-button": {
    label: "Up/Down Button",
    icon: "🔼",
    inputs: [],
    outputs: [{ id: "value", label: "" }],
  },
  switch: {
    label: "Switch",
    icon: "🔀",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  multiplexer: {
    label: "Multiplexer",
    icon: "🎯",
    // Static/default shape (2 inputs) - live input count (2-16) is handled by
    // renderMultiplexerPorts() below, same pattern as Mixer.
    inputs: [
      { id: "select", label: "Select" },
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  multiplier: {
    label: "Multiplier",
    icon: "✖️",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  adder: {
    label: "Adder",
    icon: "➕",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  divide: {
    label: "Divide",
    icon: "➗",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  "frequency-divider": {
    label: "Frequency Divider",
    icon: "➗",
    inputs: [
      { id: "cv", label: "In" },
      { id: "divider", label: "Divider" },
    ],
    outputs: [{ id: "cv", label: "Out" }],
  },
  abs: {
    label: "Abs",
    icon: "|x|",
    inputs: [{ id: "in", label: "In" }],
    outputs: [{ id: "value", label: "" }],
  },
  "value-mapper": {
    label: "Value Mapper",
    icon: "🗺️",
    inputs: [{ id: "in", label: "In" }],
    outputs: [{ id: "out", label: "Out" }],
  },
  clock: {
    label: "Clock",
    icon: "⏱️",
    inputs: [{ id: "tempo", label: "CV" }],
    outputs: [{ id: "pulse", label: "Pulse" }],
  },
  oscillator: {
    label: "WaveGen",
    icon: "〰️",
    inputs: [
      { id: "pitchCv", label: "Pitch" },
      { id: "tune", label: "Tune" },
      { id: "amplitude", label: "Amplitude" },
      { id: "waveform", label: "Waveform" },
      { id: "gate", label: "Gate" },
      { id: "gain", label: "Gain" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  hammond: {
    label: "Hammond",
    icon: "🎹",
    // No "waveform" and no "gate": the nine drawbars replace the waveform
    // selector, and the tonewheels are free-running - there is nothing for
    // a gate edge to retrigger (see the C++ nodes/HammondNode.h comment).
    // Note on/off is expressed exactly like WaveGen's: MIDI velocity ->
    // Amplitude for level, a downstream ADSR/Gain for the actual key
    // shaping. The nine "drawbarN" rows ARE real audio-rate ports here
    // (unlike Mixer's "gainN") - renderHammondPorts() below draws them.
    inputs: [
      { id: "pitchCv", label: "Pitch" },
      { id: "tune", label: "Tune" },
      { id: "amplitude", label: "Amplitude" },
      { id: "gain", label: "Gain" },
      { id: "drawbar1", label: "16′" },
      { id: "drawbar2", label: "5⅓′" },
      { id: "drawbar3", label: "8′" },
      { id: "drawbar4", label: "4′" },
      { id: "drawbar5", label: "2⅔′" },
      { id: "drawbar6", label: "2′" },
      { id: "drawbar7", label: "1⅗′" },
      { id: "drawbar8", label: "1⅓′" },
      { id: "drawbar9", label: "1′" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  "wave-player": {
    label: "Wave Player",
    icon: "🎵",
    inputs: [
      { id: "amplitude", label: "Amplitude" },
      { id: "note", label: "Note (0–127)" },
      { id: "gate", label: "Gate" },
    ],
    outputs: [
      { id: "left", label: "Out L" },
      { id: "right", label: "Out R" },
    ],
  },
  "soundfont-player": {
    label: "SoundFont Player",
    icon: "SF2",
    inputs: [
      { id: "midi", label: "MIDI" },
      { id: "bank", label: "Bank" },
      { id: "program", label: "Program" },
      { id: "enableCc", label: "Enable CC Messages" },
      { id: "enableModWheel", label: "Enable Mod Wheel (Ch 1, CC 1)" },
      { id: "enablePitchWheel", label: "Enable Pitch Wheel" },
      { id: "gain", label: "Gain" },
    ],
    outputs: [
      { id: "left", label: "Out L" },
      { id: "right", label: "Out R" },
    ],
  },
  detune: {
    label: "DeTune",
    icon: "↔️",
    inputs: [
      { id: "cv", label: "In" },
      { id: "amount", label: "Amount" },
    ],
    outputs: [
      { id: "f1", label: "Tune Up" },
      { id: "f2", label: "Tune Down" },
    ],
  },
  "audio-out": {
    label: "JS Audio-Out",
    icon: "🔊",
    inputs: [
      { id: "left", label: "In L" },
      { id: "right", label: "In R" },
      { id: "gain", label: "Volume" },
      { id: "muted", label: "Mute (Gate)" },
    ],
    outputs: [],
  },
  "usb-out": {
    label: "USB-Out",
    icon: "🖥️",
    inputs: [
      { id: "left", label: "In L" },
      { id: "right", label: "In R" },
      { id: "gain", label: "Volume" },
      { id: "muted", label: "Mute (Gate)" },
    ],
    outputs: [],
  },
  "es8311-out": {
    label: "ES8311-Out",
    icon: "🔊",
    inputs: [
      { id: "left", label: "In L" },
      { id: "right", label: "In R" },
      { id: "gain", label: "Volume" },
      { id: "muted", label: "Mute (Gate)" },
    ],
    outputs: [],
  },
  "wm8960-out": {
    label: "WM8960-Out",
    icon: "🔊",
    inputs: [
      { id: "left", label: "In L" },
      { id: "right", label: "In R" },
      { id: "gain", label: "Volume" },
      { id: "muted", label: "Mute (Gate)" },
    ],
    outputs: [],
  },
  gain: {
    label: "Gain/VCA",
    icon: "🔉",
    inputs: [
      { id: "in", label: "In" },
      { id: "amount", label: "Gain" },
      { id: "global", label: "Global" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  filter: {
    label: "Filter",
    icon: "🎚️",
    inputs: [
      { id: "in", label: "In" },
      { id: "bypass", label: "Bypass" },
      { id: "cutoff", label: "Cutoff" },
      { id: "resonance", label: "Resonance" },
      { id: "global", label: "Global" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "curtis-filter": {
    label: "CEM3320 Filter",
    icon: "🎚️",
    inputs: [
      { id: "in", label: "In" },
      { id: "bypass", label: "Bypass" },
      { id: "cutoff", label: "Cutoff" },
      { id: "resonance", label: "Resonance" },
      { id: "global", label: "Global" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "esp-eq": { label: "EQ", icon: "📶", inputs: [{ id: "inL", label: "In L" }, { id: "inR", label: "In R" }, { id: "bypass", label: "Bypass" }, { id: "frequency", label: "Frequency" }, { id: "q", label: "Q" }, { id: "gainDb", label: "Gain dB" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  "esp-alc": { label: "ESP ALC", icon: "ALC", inputs: [{ id: "inL", label: "In L" }, { id: "inR", label: "In R" }, { id: "bypass", label: "Bypass" }, { id: "gainL", label: "Gain L dB" }, { id: "gainR", label: "Gain R dB" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  "esp-channel-convert": { label: "ESP Channel Convert", icon: "CH", inputs: [{ id: "inL", label: "In L" }, { id: "inR", label: "In R" }, { id: "bypass", label: "Bypass" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  "esp-mixer": { label: "ESP Crossfade Mixer", icon: "MX", inputs: [{ id: "in1L", label: "A In L" }, { id: "in1R", label: "A In R" }, { id: "in2L", label: "B In L" }, { id: "in2R", label: "B In R" }, { id: "mode", label: "Scene" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  "esp-sonic": { label: "Sonic", icon: "Sonic", inputs: [{ id: "inL", label: "In L" }, { id: "inR", label: "In R" }, { id: "bypass", label: "Bypass" }, { id: "speed", label: "Speed" }, { id: "pitch", label: "Pitch" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  "esp-drc": { label: "Dynamic Range Control", icon: "DRC", inputs: [{ id: "inL", label: "In L" }, { id: "inR", label: "In R" }, { id: "bypass", label: "Bypass" }, { id: "attackMs", label: "Attack ms" }, { id: "releaseMs", label: "Release ms" }, { id: "makeupDb", label: "Makeup dB" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  "esp-mbc": { label: "ESP MBC", icon: "MBC", inputs: [{ id: "inL", label: "In L" }, { id: "inR", label: "In R" }, { id: "bypass", label: "Bypass" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  mixer: {
    label: "Mixer",
    icon: "🎧",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  adsr: {
    label: "ADSR",
    icon: "⛰️",
    inputs: [
      { id: "gate", label: "Gate" },
      { id: "velocity", label: "Velocity" },
      { id: "att", label: "Attack" },
      { id: "dec", label: "Decay" },
      { id: "sus", label: "Sustain" },
      { id: "rel", label: "Release" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  const: {
    label: "Const",
    icon: "🔢",
    inputs: [],
    outputs: [{ id: "value", label: "" }],
  },
  chorus: {
    label: "Chorus",
    icon: "🌀",
    inputs: [
      { id: "in", label: "In" },
      { id: "bypass", label: "Bypass" },
      { id: "rate", label: "Rate" },
      { id: "depth", label: "Depth" },
      { id: "offset", label: "Offset" },
    ],
    outputs: [
      { id: "left", label: "Out L" },
      { id: "right", label: "Out R" },
    ],
  },
  delay: {
    label: "Delay",
    icon: "🔁",
    inputs: [
      { id: "inL", label: "In L" },
      { id: "inR", label: "In R" },
      { id: "bypass", label: "Bypass" },
      { id: "time", label: "Time" },
      { id: "feedback", label: "Feedback" },
      { id: "mix", label: "Mix" },
    ],
    outputs: [
      { id: "outL", label: "Out L" },
      { id: "outR", label: "Out R" },
    ],
  },
  reverb: {
    label: "Reverb",
    icon: "🏛️",
    inputs: [
      { id: "inL", label: "In L" },
      { id: "inR", label: "In R" },
      { id: "bypass", label: "Bypass" },
      { id: "roomSize", label: "Room Size" },
      { id: "damping", label: "Damping" },
      { id: "wetLevel", label: "Wet Level" },
      { id: "dryLevel", label: "Dry Level" },
      { id: "preDelay", label: "Pre-Delay" },
    ],
    outputs: [
      { id: "outL", label: "Out L" },
      { id: "outR", label: "Out R" },
    ],
  },
  "reverb-light": {
    label: "Reverb Light",
    icon: "🪶",
    inputs: [
      { id: "inL", label: "In L" },
      { id: "inR", label: "In R" },
      { id: "bypass", label: "Bypass" },
      { id: "roomSize", label: "Room Size" },
      { id: "damping", label: "Damping" },
      { id: "wetLevel", label: "Wet Level" },
      { id: "dryLevel", label: "Dry Level" },
    ],
    outputs: [
      { id: "outL", label: "Out L" },
      { id: "outR", label: "Out R" },
    ],
  },
  "audio-output-link": {
    label: "To Master",
    icon: "🔗",
    inputs: Array.from({ length: LINK_BUS_COUNT }, (_, i) => ({ id: `in${i + 1}`, label: String(i + 1) })),
    outputs: [],
  },
  "audio-input-link": {
    label: "Audio Outputs",
    icon: "🔗",
    inputs: Array.from({ length: LINK_BUS_COUNT }, (_, i) => ({ id: `gain${i + 1}`, label: String(i + 1) })),
    outputs: Array.from({ length: LINK_BUS_COUNT }, (_, i) => ({ id: `out${i + 1}`, label: "" })),
  },
  lfo: {
    label: "LFO",
    icon: "🔄",
    inputs: [
      { id: "rate", label: "Rate" },
      { id: "amplitude", label: "Amplitude" },
      { id: "phase", label: "Phase" },
      { id: "sync", label: "Sync" },
      { id: "oneShot", label: "One Shot" },
      { id: "global", label: "Global" },
      { id: "zeroPoint", label: "Zero Point" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  "vu-meter": {
    label: "VU-Meter",
    icon: "📊",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
  "vu-meter-horizontal": {
    label: "VU-Meter Horizontal",
    icon: "📶",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
  oscilloscope: {
    label: "Oscilloscope",
    icon: "🖥️",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [],
  },
  spectrum: {
    label: "Spectrum Analyzer",
    icon: "📈",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
  voltmeter: {
    label: "Voltmeter",
    icon: "🌡️",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
};






const WIDGET_CATEGORIES = [
  { label: "Midi", types: ["midi-in", "midi-split"] },
  { label: "Generator", types: ["oscillator", "hammond", "wave-player", "soundfont-player"] },
  { label: "Modulator", types: ["lfo", "adsr", "detune", "clock"] },
  { label: "Control", types: ["knob", "button", "up-down-button", "switch", "multiplexer", "const"] },
  { label: "Math", types: ["adder", "divide", "frequency-divider", "multiplier", "abs", "value-mapper"] },
  { label: "Filter", types: ["filter", "curtis-filter", "esp-eq"] },
  { label: "Effects", types: ["chorus", "delay", "reverb", "reverb-light", "esp-alc", "esp-channel-convert", "esp-mixer", "esp-sonic", "esp-drc", "esp-mbc"] },
  { label: "Mixing/Output", types: ["gain", "mixer", "audio-out", "usb-out", "es8311-out", "wm8960-out"] },
  { label: "Misc", types: ["annotation", "debug", "voltmeter", "vu-meter", "vu-meter-horizontal", "oscilloscope", "spectrum"] },
];



const AUDIO_PORTS = {
  
  
  
  
  
  
  oscillator: { in: ["amplitude", "gain", "pitchCv", "tune"], out: ["value"] },
  // All 13 are real audio-rate ports here (unlike Mixer's control-only
  // "gainN") - each of the 9 drawbar levels is independently CV-connectable.
  hammond: {
    in: ["amplitude", "pitchCv", "tune", "gain",
         "drawbar1", "drawbar2", "drawbar3", "drawbar4", "drawbar5", "drawbar6", "drawbar7", "drawbar8", "drawbar9"],
    out: ["value"],
  },
  switch: { in: ["in1", "in2"], out: ["value"] },
  // "select" (CV) is deliberately absent here - control-rate, same reasoning
  // as C++ PortTables' isControlOnlyInput (see nodes/MultiplexerNode.h).
  multiplexer: { in: ["in1", "in2", "in3", "in4", "in5", "in6", "in7", "in8", "in9", "in10", "in11", "in12", "in13", "in14", "in15", "in16"], out: ["value"] },
  
  
  
  "wave-player": { in: ["amplitude"], out: ["left", "right"] },
  "soundfont-player": { out: ["left", "right"] },
  
  
  
  lfo: { out: ["value"] },
  // Const is a scalar control source. It is not an audio-rate output.
  const: { out: [] },
  
  
  
  
  "audio-out": { in: ["left", "right"] },
  
  
  
  
  
  
  gain: { in: ["in", "amount"], out: ["out"] },
  
  
  
  
  filter: { in: ["in", "cutoff", "resonance"], out: ["out"] },
  "curtis-filter": { in: ["in", "cutoff", "resonance"], out: ["out"] },
  "esp-eq": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "esp-alc": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "esp-channel-convert": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "esp-mixer": { in: ["in1L", "in1R", "in2L", "in2R"], out: ["outL", "outR"] },
  "esp-sonic": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "esp-drc": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "esp-mbc": { in: ["inL", "inR"], out: ["outL", "outR"] },
  
  
  
  mixer: { in: ["in1", "in2", "in3", "in4", "in5", "in6", "in7", "in8"], out: ["out"] },
  
  
  
  
  
  
  
  
  
  
  
  
  
  adsr: { out: ["out"] },
  chorus: { in: ["in"], out: ["left", "right"] },
  
  
  
  delay: { in: ["inL", "inR"], out: ["outL", "outR"] },
  reverb: { in: ["inL", "inR"], out: ["outL", "outR"] },
  "reverb-light": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "audio-output-link": { in: Array.from({ length: LINK_BUS_COUNT }, (_, i) => `in${i + 1}`) },
  "audio-input-link": {
    in: Array.from({ length: LINK_BUS_COUNT }, (_, i) => `gain${i + 1}`),
    out: Array.from({ length: LINK_BUS_COUNT }, (_, i) => `out${i + 1}`),
  },
  "audio-out": { in: ["left", "right"] },
  "usb-out": { in: ["left", "right"] },
  "es8311-out": { in: ["left", "right"] },
  "wm8960-out": { in: ["left", "right"] },
  "vu-meter": { in: ["in"] },
  "vu-meter-horizontal": { in: ["in"] },
  oscilloscope: { in: ["in1", "in2"] },
  spectrum: { in: ["in"] },
};




function audioPortsLookup(type, portId, direction) {
  const spec = AUDIO_PORTS[type];
  if (!spec) return "control";
  const list = direction === "in" ? spec.in : spec.out;
  if (!list) return "control";
  if (list === "all") return "audio";
  return list.includes(portId) ? "audio" : "control";
}







const DUAL_MODE_TYPES = {
  multiplier: { states: () => multiplierStates, audioPorts: "all" },
  adder: { states: () => adderStates, audioPorts: "all" },
  divide: { states: () => divideStates, audioPorts: "all" },
  "frequency-divider": { states: () => frequencyDividerStates, audioPorts: "all" },
  abs: { states: () => absStates, audioPorts: "all" },
  "value-mapper": { states: () => valueMapperStates, audioPorts: "all" },
  detune: { states: () => detuneStates, audioPorts: { in: ["cv"], out: ["f1", "f2"] } },
};

function getPortKind(widgetId, portId, direction) {
  const el = document.getElementById(widgetId);
  if (!el) return "control";
  const type = el.dataset.widgetType;

  const dual = DUAL_MODE_TYPES[type];
  if (dual) {
    const state = dual.states().get(widgetId);
    if (!state || !state.audioMode) return "control";
    if (dual.audioPorts === "all") return "audio";
    const list = direction === "in" ? dual.audioPorts.in : dual.audioPorts.out;
    return list && list.includes(portId) ? "audio" : "control";
  }

  return audioPortsLookup(type, portId, direction);
}








function portKindFromType(type, portId, direction) {
  return audioPortsLookup(type, portId, direction);
}

function getVisualPortKind(widgetId, portId, direction) {
  return getPortKind(widgetId, portId, direction);
}

const WAVE_TYPE_LABELS = {
  sine: "Sine",
  square: "Square",
  triangle: "Triangle",
  sawtooth: "Sawtooth",
  noise: "Noise",
  "pink-noise": "Pink Noise",
};

const WAVE_ICON_ATTRS = 'viewBox="0 0 18 12" width="18" height="12" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"';

const WAVE_TYPE_BUTTONS = [
  {
    id: "sine",
    icon: `<svg ${WAVE_ICON_ATTRS}><path d="M1,6 C2,0 4,0 5,6 C6,12 8,12 9,6 C10,0 12,0 13,6 C14,12 16,12 17,6"/></svg>`,
  },
  {
    id: "square",
    icon: `<svg ${WAVE_ICON_ATTRS}><path d="M1,9 L1,3 L5,3 L5,9 L9,9 L9,3 L13,3 L13,9 L17,9 L17,3"/></svg>`,
  },
  {
    id: "triangle",
    icon: `<svg ${WAVE_ICON_ATTRS}><path d="M1,9 L5,3 L9,9 L13,3 L17,9"/></svg>`,
  },
  {
    id: "sawtooth",
    icon: `<svg ${WAVE_ICON_ATTRS}><path d="M1,9 L5,3 L5,9 L9,3 L9,9 L13,3 L13,9 L17,3"/></svg>`,
  },
  {
    id: "noise",
    icon: `<svg ${WAVE_ICON_ATTRS}><path d="M1,6 L3,2 L4,9 L6,4 L7,10 L9,3 L10,7 L12,1 L13,8 L15,5 L17,9"/></svg>`,
  },
  {
    id: "pink-noise",
    className: "osc-wave-btn-pink-noise",
    icon: `<svg ${WAVE_ICON_ATTRS}><path d="M1,6 L3,2 L4,9 L6,4 L7,10 L9,3 L10,7 L12,1 L13,8 L15,5 L17,9"/></svg>`,
  },
];



const LFO_TYPE_BUTTONS = WAVE_TYPE_BUTTONS.filter((w) => w.id !== "noise" && w.id !== "pink-noise");

const LFO_ZERO_MODE_BUTTONS = [
  { id: "auto", label: "Auto" },
  { id: "fixed", label: "Fixed" },
];

const LFO_POLARITY_BUTTONS = [
  { id: "unipolar", label: "Unipolar" },
  { id: "bipolar", label: "Bipolar" },
];





const GENERATOR_PAGE_TYPES = [
  "midi-in", "midi-split", "knob", "button", "multiplier", "adder", "divide", "frequency-divider", "abs", "value-mapper", "clock", "oscillator", "hammond", "wave-player", "soundfont-player", "detune", "adsr", "const",
];
const FX_PAGE_TYPES = [
  "midi-in", "adsr",
  "chorus", "delay", "reverb", "reverb-light", "esp-alc", "esp-channel-convert",
  "esp-mixer", "esp-sonic", "esp-drc", "esp-mbc", "audio-out",
  "usb-out", "es8311-out", "wm8960-out",
];







const DUAL_PAGE_TYPES = ["annotation", "knob", "button", "up-down-button", "switch", "multiplexer", "debug", "voltmeter", "lfo", "vu-meter", "vu-meter-horizontal", "oscilloscope", "spectrum", "gain", "filter", "curtis-filter", "mixer", "esp-eq"];



const LINK_WIDGET_TYPES = ["audio-output-link", "audio-input-link"];
const OUTPUT_SINGLETON_TYPES = ["audio-out", "usb-out", "es8311-out", "wm8960-out"];

const pages = {
  generator: {
    canvasEl: document.getElementById("canvas-generator"),
    canvasContentEl: document.getElementById("canvasContent-generator"),
    connectionsLayerEl: document.getElementById("connectionsLayer-generator"),
    lassoBoxEl: document.getElementById("lassoBox-generator"),
    zoom: 1,
    panX: 0,
    panY: 0,
    selectedWidgetIds: new Set(),
    clipboard: { widgets: [], connections: [] },
    allowedTypes: [...GENERATOR_PAGE_TYPES, ...DUAL_PAGE_TYPES],
  },
  fx: {
    canvasEl: document.getElementById("canvas-fx"),
    canvasContentEl: document.getElementById("canvasContent-fx"),
    connectionsLayerEl: document.getElementById("connectionsLayer-fx"),
    lassoBoxEl: document.getElementById("lassoBox-fx"),
    zoom: 1,
    panX: 0,
    panY: 0,
    selectedWidgetIds: new Set(),
    clipboard: { widgets: [], connections: [] },
    allowedTypes: [...FX_PAGE_TYPES, ...DUAL_PAGE_TYPES],
  },
};
let currentPageName = "generator";

let canvas = pages.generator.canvasEl;
let canvasContent = pages.generator.canvasContentEl;
let lassoBox = pages.generator.lassoBoxEl;
const zoomIndicator = document.getElementById("zoomIndicator");

const ZOOM_MIN = 0.5;
const ZOOM_MAX = 1.5;
const ZOOM_STEP = 0.05;

let zoom = pages.generator.zoom;
let panX = pages.generator.panX;
let panY = pages.generator.panY;
let widgetCounter = 0;
let selectedWidgetIds = pages.generator.selectedWidgetIds;
let clipboard = pages.generator.clipboard;

function switchPage(name) {
  if (name === currentPageName || !pages[name]) return;

  
  
  
  cancelPendingPaste();

  const prev = pages[currentPageName];
  prev.zoom = zoom;
  prev.panX = panX;
  prev.panY = panY;
  prev.selectedWidgetIds = selectedWidgetIds;
  prev.clipboard = clipboard;
  prev.canvasEl.classList.add("hidden");

  currentPageName = name;
  const next = pages[name];
  canvas = next.canvasEl;
  canvasContent = next.canvasContentEl;
  connectionsLayer = next.connectionsLayerEl;
  lassoBox = next.lassoBoxEl;
  zoom = next.zoom;
  panX = next.panX;
  panY = next.panY;
  selectedWidgetIds = next.selectedWidgetIds;
  clipboard = next.clipboard;
  next.canvasEl.classList.remove("hidden");

  document.querySelectorAll(".page-tab").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.page === name);
  });

  applyTransform();
  updateSelectionClasses();
  drawConnections();
}

document.querySelectorAll(".page-tab").forEach((btn) => {
  btn.addEventListener("click", () => switchPage(btn.dataset.page));
});

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function parseFloatFlexible(str) {
  if (str === null || str === undefined) return NaN;
  return parseFloat(String(str).trim().replace(",", "."));
}





function parseValueList(str) {
  if (typeof str !== "string") return null;
  const parts = str
    .split(",")
    .map((s) => s.trim())
    .filter((s) => s.length > 0);
  if (parts.length < 2) return null;
  const values = parts.map((s) => parseFloatFlexible(s));
  if (values.some((v) => Number.isNaN(v))) return null;
  return values;
}

function parseVoltageValueList(str) {
  const values = parseValueList(str);
  return values ? values.map((value) => clamp(value, 0, CV_MAX_VOLTS)) : null;
}

function clampVoltage(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? clamp(number, 0, CV_MAX_VOLTS) : fallback;
}

function roundFloat(value, decimals = 4) {
  const factor = 10 ** decimals;
  return Math.round(value * factor) / factor;
}






function formatNumber(value) {
  const num = Number(value);
  return Number.isFinite(num) ? num.toFixed(2) : String(value);
}

const GRID_SNAP_KEY = "espressynth.gridSnap";
let gridSnapEnabled = localStorage.getItem(GRID_SNAP_KEY) !== "0";

function snap(value) {
  return gridSnapEnabled ? Math.round(value / GRID) * GRID : value;
}

function applyTransform() {
  canvasContent.style.transform = `translate(${panX}px, ${panY}px) scale(${zoom})`;
  zoomIndicator.textContent = `${Math.round(zoom * 100)}%`;
}

function clientToContent(clientX, clientY) {
  const rect = canvas.getBoundingClientRect();
  return {
    x: (clientX - rect.left - panX) / zoom,
    y: (clientY - rect.top - panY) / zoom,
  };
}

function updateSelectionClasses() {
  canvasContent.querySelectorAll(".widget-instance").forEach((w) => {
    w.classList.toggle("selected", selectedWidgetIds.has(w.id));
  });
}

function selectOnlyWidget(id) {
  selectedWidgetIds = new Set([id]);
  updateSelectionClasses();
}

function toggleWidgetSelection(id) {
  if (selectedWidgetIds.has(id)) selectedWidgetIds.delete(id);
  else selectedWidgetIds.add(id);
  updateSelectionClasses();
}

function clearSelection() {
  selectedWidgetIds.clear();
  updateSelectionClasses();
  if (selectedConnectionId) {
    const prev = connectionsLayer.querySelector(`[data-conn-id="${selectedConnectionId}"]`);
    if (prev) prev.classList.remove("selected");
    selectedConnectionId = null;
  }
}

function openLfoSettings(widgetId) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  pendingLfoId = widgetId;
  lfoMaxRateInput.value = String(state.maxRate);
  lfoSettingsOverlay.classList.remove("hidden");
}

function createWidget(type, label, x, y, forcedId) {
  if (!forcedId && OUTPUT_SINGLETON_TYPES.includes(type)) {
    const existing = pages.fx.canvasContentEl.querySelector(`.widget-instance[data-widget-type="${type}"]`);
    if (existing) {
      showToast(`Only one ${WIDGET_TYPES[type]?.label || label} is allowed`);
      return existing;
    }
  }
  let id;
  if (forcedId) {
    id = forcedId;
    const num = parseInt(forcedId.replace("widget-", ""), 10);
    if (!Number.isNaN(num)) widgetCounter = Math.max(widgetCounter, num);
  } else {
    widgetCounter += 1;
    id = `widget-${widgetCounter}`;
  }
  const def = WIDGET_TYPES[type] || { label, inputs: [], outputs: [] };

  const el = document.createElement("div");
  el.className = "widget-instance";
  el.id = id;
  el.style.left = `${x}px`;
  el.style.top = `${y}px`;
  el.dataset.widgetType = type;

  const header = document.createElement("div");
  header.className = "widget-instance-header";

  const titleWrap = document.createElement("span");
  titleWrap.className = "widget-instance-title";

  if (def.icon) {
    const iconSpan = document.createElement("span");
    iconSpan.textContent = def.icon;
    titleWrap.appendChild(iconSpan);
  }

  const labelSpan = document.createElement("span");
  labelSpan.className = "widget-instance-label";
  labelSpan.textContent = label;
  titleWrap.appendChild(labelSpan);

  header.appendChild(titleWrap);

  if (!LINK_WIDGET_TYPES.includes(type)) {
    const closeBtn = document.createElement("button");
    closeBtn.className = "widget-instance-close";
    closeBtn.textContent = "×";
    closeBtn.addEventListener("click", () => {
      pushHistorySnapshot();
      removeWidget(id);
      drawConnections();
      savePresetNow(); 
    });
    header.appendChild(closeBtn);
  }

  header.addEventListener("mousedown", (e) => startWidgetDrag(e, el));

  header.addEventListener("dblclick", (e) => {
    if (e.target.closest("button")) return;
    e.stopPropagation();
    startWidgetRename(labelSpan);
  });

  el.addEventListener("click", (e) => {
    if (e.target.closest("button") || e.target.closest(".port-dot")) return;
    if (el.dataset.justDragged === "1") return;
    e.stopPropagation();
    if (e.shiftKey || e.ctrlKey || e.metaKey) {
      toggleWidgetSelection(id);
    } else {
      selectOnlyWidget(id);
    }
  });

  if (type === "knob") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openKnobSettings(id);
    });
  }

  if (type === "button") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openButtonSettings(id);
    });
  }

  if (type === "up-down-button") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openUpDownButtonSettings(id);
    });
  }

  if (type === "switch") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button") || e.target.closest("label")) return;
      e.stopPropagation();
      openSwitchSettings(id);
    });
  }

  if (type === "midi-in") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openMidiInSettings(id);
    });
  }

  if (type === "midi-split") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openMidiSplitSettings(id);
    });
  }

  if (type === "multiplier") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openMultiplierSettings(id);
    });
  }

  if (type === "adder") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openAdderSettings(id);
    });
  }

  if (type === "clock") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openClockSettings(id);
    });
  }

  if (type === "mixer") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openMixerSettings(id);
    });
  }

  let multiplexerSwitchBody = null;

  if (type === "multiplexer") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button") || e.target.closest("input")) return;
      e.stopPropagation();
      openMultiplexerSettings(id);
    });
  }

  if (type === "wave-player") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openWavePlayerSettings(id);
    });
  }

  if (type === "soundfont-player") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openSoundFontPlayerSettings(id);
    });
  }

  if (type === "adsr") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openAdsrSettings(id);
    });
  }

  if (type === "filter" || type === "curtis-filter") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openFilterSettings(id);
    });
  }

  if (type === "value-mapper") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openValueMapperSettings(id);
    });
  }

  if (type === "delay" || type === "reverb" || type === "reverb-light") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openEffectCvSettings(id);
    });
  }

  el.appendChild(header);

  if (type === "annotation") {
    createAnnotationBody(id, el);
  }

  if (type === "midi-in") {
    midiInStates.set(id, { channel: null, voices: 1, overflowMode: "steal-oldest", glide: false, glideTime: 0 });
    midiVoiceSlotStates.set(id, {
      slots: [{ note: null, heldSince: 0, held: false }],
      holdCounter: 0,
      pedalDown: false,
      pendingRelease: false,
      monoHeldNotes: new Map(),
      monoCurrentNote: null,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body midi-in-body";
    const voiceSlotsLabel = document.createElement("span");
    voiceSlotsLabel.className = "midi-in-voice-slots hidden";
    voiceSlotsLabel.dataset.role = "midi-in-voice-slots";
    body.appendChild(voiceSlotsLabel);
    el.appendChild(body);
  }

  if (type === "midi-split") {
    midiSplitStates.set(id, {
      zoneCount: 0, 
      zones: [
        { from: 0, to: 127, octave: 0 },
        { from: 0, to: 127, octave: 0 },
        { from: 0, to: 127, octave: 0 },
        { from: 0, to: 127, octave: 0 },
      ],
      zoneActiveCount: [0, 0, 0, 0], 
    });
  }

  if (type === "multiplier") {
    multiplierStates.set(id, {
      inputCount: 2,
      values: [1, 1, 1, 1, 1, 1, 1, 1],
      audioMode: false,
    });
  }

  if (type === "divide") {
    divideStates.set(id, {
      values: [0, 1],
      audioMode: false,
    });
  }

  if (type === "frequency-divider") {
    frequencyDividerStates.set(id, { cv: 0, divider: 2, audioMode: false });
  }

  if (type === "abs") {
    absStates.set(id, { value: 0, audioMode: false });
  }

  if (type === "value-mapper") {
    valueMapperStates.set(id, {
      value: 0,
      point1In: 0,
      point1Out: 0,
      point2In: 127,
      point2Out: CV_MAX_VOLTS,
      constraintMin: null,
      constraintMax: null,
      listText: "",
      audioMode: false,
    });
  }

  if (type === "adder") {
    adderStates.set(id, {
      inputCount: 2,
      values: [0, 0, 0, 0, 0, 0, 0, 0],
      audioMode: false,
    });
  }

  if (type === "clock") {
    clockStates.set(id, {
      cvVolts: 0,
      bpm: 0,
      minBpm: 0,
      maxBpm: 240,
      pulseMs: 100,
      tickTimer: null,
      offTimer: null,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body clock-body";

    const pulseRow = document.createElement("div");
    pulseRow.className = "clock-pulse-row";

    const dot = document.createElement("div");
    dot.className = "clock-pulse-dot";
    dot.dataset.role = "clock-pulse-dot";

    const bpmLabel = document.createElement("div");
    bpmLabel.className = "clock-bpm-label";
    bpmLabel.dataset.role = "clock-bpm-label";
    bpmLabel.textContent = "0 BPM";

    pulseRow.appendChild(dot);
    body.appendChild(pulseRow);
    body.appendChild(bpmLabel);
    el.appendChild(body);
  }

  if (type === "oscillator") {
    const center = CV_MAX_VOLTS / 2;
    const oscState = {
      
      
      
      
      
      
      pitchCv: center,
      tuneVolts: center,
      freq: hzFromPitchCv(center),
      waveType: "sine",
      amplitude: 1,
      
      
      gain: CV_MAX_VOLTS,
    };
    oscillatorStates.set(id, oscState);
    audioEngine.createNode(id, "oscillator", {
      waveType: oscState.waveType,
      frequency: oscState.freq,
      amplitude: 0,
      gain: oscState.gain,
      tuneVolts: oscState.tuneVolts,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body oscillator-body";

    const waveButtons = document.createElement("div");
    waveButtons.className = "osc-wave-buttons";
    WAVE_TYPE_BUTTONS.forEach((w) => {
      const btn = document.createElement("button");
      btn.className = "osc-wave-btn";
      if (w.className) btn.classList.add(w.className);
      btn.innerHTML = w.icon;
      btn.dataset.wave = w.id;
      btn.title = WAVE_TYPE_LABELS[w.id];
      if (w.id === oscState.waveType) btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setOscillatorWaveType(id, w.id);
      });
      waveButtons.appendChild(btn);
    });

    const freqLabel = document.createElement("div");
    freqLabel.className = "osc-freq-label";
    freqLabel.dataset.role = "osc-freq-label";
    freqLabel.textContent = `${formatNumber(oscState.freq)} Hz`;

    body.appendChild(waveButtons);
    body.appendChild(freqLabel);
    el.appendChild(body);
  }

  if (type === "hammond") {
    const center = CV_MAX_VOLTS / 2;
    const hammondState = {
      pitchCv: center,
      tuneVolts: center,
      freq: hzFromPitchCv(center),
      amplitude: 1,
      gain: CV_MAX_VOLTS,
      drawbars: [...HAMMOND_DEFAULT_DRAWBARS],
      ratios: [...HAMMOND_DEFAULT_RATIOS],
      modulationStride: 4,
    };
    hammondStates.set(id, hammondState);
    audioEngine.createNode(id, "hammond", {
      frequency: hammondState.freq,
      amplitude: 0,
      gain: hammondState.gain,
      tuneVolts: hammondState.tuneVolts,
      drawbars: [...hammondState.drawbars],
      ratios: [...hammondState.ratios],
      modulationStride: hammondState.modulationStride,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body oscillator-body";
    const freqLabel = document.createElement("div");
    freqLabel.className = "osc-freq-label";
    freqLabel.dataset.role = "hammond-freq-label";
    freqLabel.textContent = `${formatNumber(hammondState.freq)} Hz`;
    body.appendChild(freqLabel);
    el.appendChild(body);

    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openHammondSettings(id);
    });
  }

  if (type === "wave-player") {
    const wpState = { sampleFile: "", rootNoteMidi: 60 };
    wavePlayerStates.set(id, wpState);
    audioEngine.createNode(id, "wave-player", { sampleFile: wpState.sampleFile, rootNoteMidi: wpState.rootNoteMidi });

    const body = document.createElement("div");
    body.className = "widget-instance-body wave-player-body";

    const sampleLabel = document.createElement("div");
    sampleLabel.className = "wave-player-sample-label";
    sampleLabel.dataset.role = "wave-player-sample-label";
    sampleLabel.textContent = "No sample";

    body.appendChild(sampleLabel);
    el.appendChild(body);
  }

  if (type === "soundfont-player") {
    const state = { volume: "flash", path: "", bank: 0, program: 0, maxVoices: 32,
      enableCc: false, enableModWheel: false, enablePitchWheel: false, gain: CV_MAX_VOLTS,
      maxBank: SOUNDFONT_DEFAULT_MAX_BANK, presetName: null };
    soundFontPlayerStates.set(id, state);
    audioEngine.createNode(id, "soundfont-player", state);
    const body = document.createElement("div");
    body.className = "widget-instance-body wave-player-body";
    const fileLabel = document.createElement("div");
    fileLabel.className = "wave-player-sample-label";
    fileLabel.dataset.role = "soundfont-player-file-label";
    fileLabel.textContent = "No SoundFont";
    const presetLabel = document.createElement("div");
    presetLabel.className = "wave-player-sample-label soundfont-preset-label";
    presetLabel.dataset.role = "soundfont-player-preset-label";
    body.appendChild(fileLabel);
    body.appendChild(presetLabel);
    el.appendChild(body);
  }

  if (type === "audio-out" || type === "usb-out" || type === "es8311-out" || type === "wm8960-out") {
    const state = { muted: false, gain: CV_MAX_VOLTS };
    audioEngine.createNode(id, type, state);
    audioOutStates.set(id, state);

    const body = document.createElement("div");
    body.className = "widget-instance-body audio-out-body";

    const muteRow = document.createElement("div");
    muteRow.className = "audio-out-mute-row";
    const muteSwitch = document.createElement("label");
    muteSwitch.className = "switch";
    const muteInput = document.createElement("input");
    muteInput.type = "checkbox";
    muteInput.dataset.role = "audio-out-mute";
    const muteTrack = document.createElement("span");
    muteTrack.className = "switch-track";
    muteSwitch.appendChild(muteInput);
    muteSwitch.appendChild(muteTrack);
    const muteText = document.createElement("span");
    muteText.textContent = "Mute";
    muteRow.appendChild(muteSwitch);
    muteRow.appendChild(muteText);

    const volumeRow = document.createElement("label");
    volumeRow.className = "audio-out-volume-row";
    const volumeText = document.createElement("span");
    volumeText.textContent = "VOL";
    const volumeSlider = document.createElement("input");
    volumeSlider.type = "range";
    volumeSlider.className = "port-slider";
    volumeSlider.min = "0";
    volumeSlider.max = String(CV_MAX_VOLTS);
    volumeSlider.step = "0.1";
    volumeSlider.value = String(CV_MAX_VOLTS);
    volumeSlider.dataset.role = "audio-out-volume";
    const volumeValue = document.createElement("span");
    volumeValue.className = "port-value";
    volumeValue.dataset.role = "audio-out-volume-value";
    volumeValue.textContent = "10.0 V";
    volumeRow.appendChild(volumeText);
    volumeRow.appendChild(volumeSlider);
    volumeRow.appendChild(volumeValue);

    [muteInput, volumeSlider].forEach((control) => {
      control.addEventListener("mousedown", (e) => e.stopPropagation());
      control.addEventListener("click", (e) => e.stopPropagation());
    });
    muteInput.addEventListener("change", () => setAudioOutParams(id, { muted: muteInput.checked }));
    volumeSlider.addEventListener("input", () => setAudioOutParams(id, { gain: Number(volumeSlider.value) }));

    body.appendChild(muteRow);
    body.appendChild(volumeRow);
    el.appendChild(body);
  }

  if (type === "gain") {
    gainStates.set(id, { amount: CV_MAX_VOLTS, curve: "linear", global: false });
    audioEngine.createNode(id, "gain", { amount: CV_MAX_VOLTS, curve: "linear" });

    const body = document.createElement("div");
    body.className = "widget-instance-body gain-body";

    const curveButtons = document.createElement("div");
    curveButtons.className = "filter-type-buttons";
    GAIN_CURVE_BUTTONS.forEach((c) => {
      const btn = document.createElement("button");
      btn.className = "filter-type-btn";
      btn.textContent = c.label;
      btn.dataset.curve = c.id;
      btn.title = c.title;
      if (c.id === "linear") btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setGainCurve(id, c.id);
      });
      curveButtons.appendChild(btn);
    });

    body.appendChild(curveButtons);
    el.appendChild(body);
  }

  if (type === "filter" || type === "curtis-filter") {
    const filterDefaults = {
      filterType: type === "curtis-filter" ? "lp24" : "lowpass",
      cutoff: 5,
      cutoffMinHz: 20,
      cutoffMaxHz: 20000,
      cutoffCurve: "log",
      resonance: 5,
      resonanceMinQ: 0.1,
      resonanceMaxQ: 20,
      resonanceCurve: "log",
      
      
      
      
      
      
      coeffStride: 4,
      global: false,
      bypass: false,
    };
    filterStates.set(id, { ...filterDefaults });
    audioEngine.createNode(id, type, { ...filterDefaults });

    const body = document.createElement("div");
    body.className = "widget-instance-body filter-body";

    const typeButtons = document.createElement("div");
    typeButtons.className = "filter-type-buttons";
    const typeButtonsDefinition = type === "curtis-filter" ? CURTIS_FILTER_TYPE_BUTTONS : FILTER_TYPE_BUTTONS;
    const typeLabels = type === "curtis-filter" ? CURTIS_FILTER_TYPE_LABELS : FILTER_TYPE_LABELS;
    typeButtonsDefinition.forEach((f) => {
      const btn = document.createElement("button");
      btn.className = "filter-type-btn";
      btn.textContent = f.label;
      btn.dataset.filterType = f.id;
      btn.title = typeLabels[f.id];
      if (f.id === filterDefaults.filterType) btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setFilterType(id, f.id);
      });
      typeButtons.appendChild(btn);
    });

    body.appendChild(typeButtons);
    el.appendChild(body);
  }

  if (ESP_EFFECT_DEFAULTS[type]) {
    const state = { ...ESP_EFFECT_DEFAULTS[type] };
    espEffectStates.set(id, state);
    audioEngine.createNode(id, type, state);
  }

  if (type === "esp-eq") {
    const body = document.createElement("div");
    body.className = "widget-instance-body esp-eq-body";

    const typeButtons = document.createElement("div");
    typeButtons.className = "filter-type-buttons";
    EQ_FILTER_TYPE_BUTTONS.forEach((f) => {
      const btn = document.createElement("button");
      btn.className = "filter-type-btn eq-filter-type-btn";
      btn.textContent = f.label;
      btn.dataset.filterType = f.id;
      btn.title = EQ_FILTER_TYPE_LABELS[f.id];
      if (f.id === "peak") btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setEspEqFilterType(id, f.id);
      });
      typeButtons.appendChild(btn);
    });

    body.appendChild(typeButtons);
    el.appendChild(body);
  }

  if (type === "mixer") {
    mixerStates.set(id, { inputCount: 2, gains: new Array(MIXER_MAX_INPUTS).fill(1) });
    audioEngine.createNode(id, "mixer", { inputCount: 2 });
  }

  if (type === "multiplexer") {
    multiplexerStates.set(id, {
      inputCount: 2,
      manualSelect: 0,
      activeIndex: 0,
      cvVolts: null,
      softFadeMs: 5,
      values: new Array(MULTIPLEXER_MAX_INPUTS).fill(0),
    });
    audioEngine.createNode(id, "multiplexer", { select: 0, softFadeMs: 5 });

    const body = document.createElement("div");
    body.className = "widget-instance-body switch-body multiplexer-switch-body";

    const selector = document.createElement("div");
    selector.className = "switch multiplexer-switch";
    selector.setAttribute("role", "img");
    selector.setAttribute("aria-label", "Aktiver Multiplexer-Eingang");
    const track = document.createElement("span");
    track.className = "switch-track";
    const thumb = document.createElement("span");
    thumb.className = "multiplexer-switch-thumb";
    thumb.dataset.role = "multiplexer-switch-thumb";
    selector.append(track, thumb);
    let dragging = false;
    selector.addEventListener("pointerdown", (e) => {
      if (e.button !== 0 || multiplexerHasCvConnection(id)) return;
      e.preventDefault();
      e.stopPropagation();
      dragging = true;
      selector.setPointerCapture?.(e.pointerId);
      setMultiplexerManualIndex(id, e.clientY);
    });
    selector.addEventListener("pointermove", (e) => {
      if (!dragging) return;
      e.preventDefault();
      setMultiplexerManualIndex(id, e.clientY);
    });
    const stopDragging = (e) => {
      dragging = false;
      if (e.pointerId !== undefined && selector.hasPointerCapture?.(e.pointerId)) {
        selector.releasePointerCapture(e.pointerId);
      }
    };
    selector.addEventListener("pointerup", stopDragging);
    selector.addEventListener("pointercancel", stopDragging);
    body.appendChild(selector);
    multiplexerSwitchBody = body;
  }

  if (type === "lfo") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button, input, .port-dot")) return;
      e.stopPropagation();
      openLfoSettings(id);
    });
  }

  if (type === "adsr") {
    const adsrDefaults = {
      attack: 0.4,
      decay: 0.4,
      sustain: 0.7,
      release: 0.6,
      attackMaxSeconds: MAX_ENVELOPE_SECONDS,
      attackCurve: "linear",
      decayMaxSeconds: MAX_ENVELOPE_SECONDS,
      decayCurve: "linear",
      releaseMaxSeconds: MAX_ENVELOPE_SECONDS,
      releaseCurve: "linear",
    };
    adsrStates.set(id, { ...adsrDefaults });
    audioEngine.createNode(id, "adsr", { ...adsrDefaults });
  }

  if (type === "chorus") {
    const chorusDefaults = { rate: 1.5, depth: 0.003, offset: 0.02, bypass: false };
    chorusStates.set(id, { ...chorusDefaults });
    audioEngine.createNode(id, "chorus", { ...chorusDefaults });
  }

  if (type === "delay" || type === "reverb" || type === "reverb-light") {
    const state = defaultEffectCvState(type);
    effectCvStates.set(id, state);
    audioEngine.createNode(id, type, effectCvParamsToValues(type, state));
  }

  if (type === "audio-output-link" || type === "audio-input-link") {
    const state = type === "audio-input-link" ? { gains: Array(LINK_BUS_COUNT).fill(CV_MAX_VOLTS) } : {};
    if (type === "audio-input-link") audioInputLinkStates.set(id, state);
    audioEngine.createNode(id, type, state);
  }

  if (type === "lfo") {
    lfoStates.set(id, {
      waveType: "sine",
      rate: 2,
      maxRate: LFO_MAX_RATE_HZ,
      amplitude: 10,
      phase: 0, 
      zeroPointFixed: false,
      zeroPoint: 5,
      bipolar: false,
      oneShot: false,
      oneShotDone: true,
      oneShotAccum: 0,
      global: false,
      cyclePos: 0,
      lastTick: null,
      timer: null,
      syncPrevByVoice: new Map(),
    });
    audioEngine.createNode(id, "lfo", {
      waveType: "sine",
      rate: 2,
      maxRate: LFO_MAX_RATE_HZ,
      amplitude: 10,
      phase: 0,
      zeroPointFixed: false,
      zeroPoint: 5,
      bipolar: false,
      oneShot: false,
      global: false,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body lfo-body";

    const waveButtons = document.createElement("div");
    waveButtons.className = "osc-wave-buttons";
    LFO_TYPE_BUTTONS.forEach((w) => {
      const btn = document.createElement("button");
      btn.className = "osc-wave-btn";
      btn.innerHTML = w.icon;
      btn.dataset.wave = w.id;
      btn.title = WAVE_TYPE_LABELS[w.id];
      if (w.id === "sine") btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setLfoWaveType(id, w.id);
      });
      waveButtons.appendChild(btn);
    });
    body.appendChild(waveButtons);

    
    
    
    
    
    
    const zeroModeButtons = document.createElement("div");
    zeroModeButtons.className = "filter-type-buttons lfo-zero-mode-buttons";
    LFO_ZERO_MODE_BUTTONS.forEach((m) => {
      const btn = document.createElement("button");
      btn.className = "filter-type-btn";
      btn.textContent = m.label;
      btn.dataset.zeroMode = m.id;
      btn.title = m.id === "fixed" ? "Zero point fixed adjustable" : "Zero point = half amplitude";
      if (m.id === "auto") btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setLfoZeroPointMode(id, m.id === "fixed");
      });
      zeroModeButtons.appendChild(btn);
    });
    body.appendChild(zeroModeButtons);

    const polarityButtons = document.createElement("div");
    polarityButtons.className = "filter-type-buttons lfo-polarity-buttons";
    LFO_POLARITY_BUTTONS.forEach((m) => {
      const btn = document.createElement("button");
      btn.className = "filter-type-btn";
      btn.textContent = m.label;
      btn.dataset.polarity = m.id;
      btn.title = m.id === "bipolar" ? "Audio-style +/-5V swing, centered 0V" : "CV-style 0-10V swing (default)";
      if (m.id === "unipolar") btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setLfoPolarity(id, m.id === "bipolar");
      });
      polarityButtons.appendChild(btn);
    });
    body.appendChild(polarityButtons);

    el.appendChild(body);

    startLfoTimer(id);
  }

  if (type === "vu-meter") {
    audioEngine.createNode(id, "vu-meter", {});
    audioMonitorStates.set(id, createAudioMonitorState());

    const body = document.createElement("div");
    body.className = "widget-instance-body vu-meter-body vu-meter-analog-body";

    const gauge = document.createElement("div");
    gauge.className = "vu-analog-gauge";
    for (let tick = 0; tick <= 10; tick += 1) {
      const mark = document.createElement("span");
      mark.className = `vu-analog-tick vu-zone-${tick >= 9 ? "red" : tick >= 7 ? "amber" : "green"}`;
      mark.style.setProperty("--vu-angle", `${-52 + tick * 10.4}deg`);
      gauge.appendChild(mark);
    }
    [0, 0.5, 1].forEach((fraction) => {
      const angle = -52 + fraction * 104;
      const radians = angle * Math.PI / 180;
      const scaleLabel = document.createElement("span");
      scaleLabel.className = "vu-analog-label";
      scaleLabel.dataset.rangeFraction = String(fraction);
      scaleLabel.style.left = `${70 + Math.sin(radians) * 45}px`;
      scaleLabel.style.top = `${66 - Math.cos(radians) * 41}px`;
      gauge.appendChild(scaleLabel);
    });
    const unit = document.createElement("span");
    unit.className = "vu-analog-unit";
    unit.textContent = "V RMS";
    gauge.appendChild(unit);
    const needle = document.createElement("span");
    needle.className = "vu-analog-needle";
    needle.dataset.role = "vu-needle";
    gauge.appendChild(needle);
    const pivot = document.createElement("span");
    pivot.className = "vu-analog-pivot";
    gauge.appendChild(pivot);
    const readout = document.createElement("span");
    readout.className = "voltmeter-readout vu-meter-readout";
    readout.dataset.role = "vu-readout";
    readout.textContent = "0.00 V";
    body.appendChild(gauge);
    body.appendChild(readout);
    el.appendChild(body);
  }

  if (type === "vu-meter-horizontal") {
    audioEngine.createNode(id, "vu-meter", {});
    audioMonitorStates.set(id, createAudioMonitorState());

    const body = document.createElement("div");
    body.className = "widget-instance-body vu-meter-body vu-meter-horizontal-body";
    const scale = document.createElement("div");
    scale.className = "vu-horizontal-scale";
    [0, 0.25, 0.5, 0.75, 1].forEach((fraction) => {
      const scaleLabel = document.createElement("span");
      scaleLabel.dataset.rangeFraction = String(fraction);
      scale.appendChild(scaleLabel);
    });
    const track = document.createElement("div");
    track.className = "vu-meter-track";
    const fill = document.createElement("div");
    fill.className = "vu-meter-fill";
    fill.dataset.role = "vu-fill";
    const peak = document.createElement("span");
    peak.className = "vu-meter-peak";
    peak.dataset.role = "vu-peak";
    track.appendChild(fill);
    track.appendChild(peak);
    const horizontalReadout = document.createElement("span");
    horizontalReadout.className = "voltmeter-readout vu-meter-readout";
    horizontalReadout.dataset.role = "vu-readout";
    horizontalReadout.textContent = "0.00 V";
    body.appendChild(scale);
    body.appendChild(track);
    body.appendChild(horizontalReadout);
    el.appendChild(body);
  }

  if (type === "oscilloscope") {
    audioEngine.createNode(id, "oscilloscope", {});

    const body = document.createElement("div");
    body.className = "widget-instance-body oscilloscope-body";

    const canvas = document.createElement("canvas");
    canvas.className = "scope-canvas";
    canvas.dataset.role = "scope-canvas";
    canvas.width = 220;
    canvas.height = 80;

    body.appendChild(canvas);
    el.appendChild(body);
  }

  if (type === "spectrum") {
    audioEngine.createNode(id, "spectrum", {});
    audioMonitorStates.set(id, createAudioMonitorState());

    const body = document.createElement("div");
    body.className = "widget-instance-body spectrum-body";

    const canvas = document.createElement("canvas");
    canvas.className = "spectrum-canvas";
    canvas.dataset.role = "spectrum-canvas";
    canvas.width = 220;
    canvas.height = 80;

    const axisCanvas = document.createElement("canvas");
    axisCanvas.className = "spectrum-axis-canvas";
    axisCanvas.dataset.role = "spectrum-axis-canvas";
    axisCanvas.width = 220;
    axisCanvas.height = 14;

    body.appendChild(canvas);
    body.appendChild(axisCanvas);
    body.appendChild(createMonitorRangeControl(id));
    el.appendChild(body);
  }

  if (type === "voltmeter") {
    voltmeterStates.set(id, { value: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body voltmeter-body";

    const dial = document.createElement("div");
    dial.className = "voltmeter-dial";

    const scale = document.createElement("div");
    scale.className = "voltmeter-scale";
    for (let tick = 0; tick <= 20; tick += 1) {
      const mark = document.createElement("span");
      mark.className = `voltmeter-tick${tick % 5 === 0 ? " voltmeter-tick-major" : ""}`;
      mark.style.setProperty("--tick-angle", `${-60 + tick * 6}deg`);
      scale.appendChild(mark);
    }
    dial.appendChild(scale);

    const labels = [
      ["-10", -60],
      ["-5", -30],
      ["0", 0],
      ["+5", 30],
      ["+10", 60],
    ];
    labels.forEach(([text, angle]) => {
      const radians = angle * Math.PI / 180;
      const label = document.createElement("span");
      label.className = "voltmeter-scale-label";
      label.textContent = text;
      label.style.left = `${63 + Math.sin(radians) * 36}px`;
      label.style.top = `${70 - Math.cos(radians) * 36}px`;
      dial.appendChild(label);
    });

    const unit = document.createElement("span");
    unit.className = "voltmeter-unit";
    unit.textContent = "VOLTS";
    dial.appendChild(unit);

    const needle = document.createElement("div");
    needle.className = "voltmeter-needle";
    needle.dataset.role = "voltmeter-needle";
    dial.appendChild(needle);

    const pivot = document.createElement("div");
    pivot.className = "voltmeter-pivot";
    dial.appendChild(pivot);

    const readout = document.createElement("div");
    readout.className = "voltmeter-readout";
    readout.dataset.role = "voltmeter-readout";
    readout.textContent = "+0.00 V";

    body.appendChild(dial);
    body.appendChild(readout);
    el.appendChild(body);
  }

  if (type === "knob") {
    knobStates.set(id, {
      cc: null,
      channel: null,
      raw: 0,
      point1In: 0,
      point1Out: 0,
      point2In: 127,
      point2Out: CV_MAX_VOLTS,
      constraintMin: null,
      constraintMax: null,
      listText: "",
    });
    audioEngine.createNode(id, "knob", { value: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body knob-body";

    const dial = document.createElement("div");
    dial.className = "knob-dial";

    const scale = document.createElement("div");
    scale.className = "knob-scale";
    const tickCount = 19;
    const scaleCenter = 46;
    const tickRadius = 41;
    for (let tickIndex = 0; tickIndex < tickCount; tickIndex += 1) {
      const angle = -135 + (270 * tickIndex) / (tickCount - 1);
      const radians = angle * Math.PI / 180;
      const tick = document.createElement("span");
      tick.className = "knob-tick";
      tick.style.left = `${scaleCenter + Math.sin(radians) * tickRadius}px`;
      tick.style.top = `${scaleCenter - Math.cos(radians) * tickRadius}px`;
      tick.style.transform = `translate(-50%, -50%) rotate(${angle}deg)`;
      scale.appendChild(tick);
    }
    dial.appendChild(scale);

    const indicator = document.createElement("div");
    indicator.className = "knob-indicator";
    indicator.dataset.role = "knob-indicator";
    dial.appendChild(indicator);

    const centerCap = document.createElement("div");
    centerCap.className = "knob-center-cap";
    dial.appendChild(centerCap);

    const readout = document.createElement("div");
    readout.className = "knob-readout";
    readout.dataset.role = "knob-readout";
    readout.textContent = "0.00";

    const ccLabel = document.createElement("div");
    ccLabel.className = "knob-cc-label";
    ccLabel.dataset.role = "knob-cc-label";

    const valueRow = document.createElement("div");
    valueRow.className = "knob-value-row";
    valueRow.appendChild(readout);

    dial.addEventListener("mousedown", (e) => startKnobDrag(e, id));

    body.appendChild(dial);
    body.appendChild(valueRow);
    body.appendChild(ccLabel);
    el.appendChild(body);

    updateKnobVisual(id);
  }

  if (type === "button") {
    buttonStates.set(id, {
      cc: null,
      channel: null,
      value0: 0,
      value127: CV_MAX_VOLTS,
      listText: "",
      listIndex: 0,
      wasAbove: false,
    });
    audioEngine.createNode(id, "button", { value: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body button-body";

    const controlRow = document.createElement("div");
    controlRow.className = "button-control-row";

    const pressButton = document.createElement("button");
    pressButton.className = "button-trigger";
    pressButton.type = "button";
    pressButton.dataset.role = "button-readout";
    pressButton.title = "Wert weiterschalten";
    pressButton.addEventListener("click", (e) => {
      e.stopPropagation();
      advanceButtonValue(id);
    });

    const ccLabel = document.createElement("div");
    ccLabel.className = "button-cc-label";
    ccLabel.dataset.role = "button-cc-label";

    controlRow.appendChild(pressButton);
    body.appendChild(controlRow);
    body.appendChild(ccLabel);
    el.appendChild(body);

    updateButtonVisual(id);
  }

  if (type === "up-down-button") {
    upDownButtonStates.set(id, {
      ccUp: null,
      channelUp: null,
      ccDown: null,
      channelDown: null,
      min: 0,
      max: CV_MAX_VOLTS,
      step: 1,
      listText: "",
      listIndex: 0,
      currentValue: 0,
    });
    audioEngine.createNode(id, "up-down-button", { value: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body up-down-button-body";

    const pressRow = document.createElement("div");
    pressRow.className = "up-down-button-press-row";

    const downButton = document.createElement("button");
    downButton.className = "button-trigger up-down-button-down";
    downButton.type = "button";
    downButton.title = "Wert verringern";
    downButton.textContent = "▼";
    downButton.addEventListener("click", (e) => {
      e.stopPropagation();
      advanceUpDownButtonValue(id, "down");
    });

    const readout = document.createElement("span");
    readout.className = "up-down-button-readout";
    readout.dataset.role = "up-down-button-readout";

    const upButton = document.createElement("button");
    upButton.className = "button-trigger up-down-button-up";
    upButton.type = "button";
    upButton.title = "Wert erhoehen";
    upButton.textContent = "▲";
    upButton.addEventListener("click", (e) => {
      e.stopPropagation();
      advanceUpDownButtonValue(id, "up");
    });

    pressRow.appendChild(downButton);
    pressRow.appendChild(readout);
    pressRow.appendChild(upButton);

    const ccLabel = document.createElement("div");
    ccLabel.className = "button-cc-label";
    ccLabel.dataset.role = "up-down-button-cc-label";

    body.appendChild(pressRow);
    body.appendChild(ccLabel);
    el.appendChild(body);

    updateUpDownButtonVisual(id);
  }

  let switchBody = null;

  if (type === "switch") {
    switchStates.set(id, { cc: null, channel: null, selectIn2: false, in1: 0, in2: 0, in1Default: 0, in2Default: 0 });
    audioEngine.createNode(id, "switch", { selectIn2: false, in1Default: 0, in2Default: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body switch-body";

    const toggleLabel = document.createElement("label");
    toggleLabel.className = "switch";
    const toggleInput = document.createElement("input");
    toggleInput.type = "checkbox";
    toggleInput.dataset.role = "switch-toggle";
    toggleInput.addEventListener("mousedown", (e) => e.stopPropagation());
    toggleInput.addEventListener("click", (e) => e.stopPropagation());
    toggleInput.addEventListener("change", () => setSwitchSelectIn2(id, toggleInput.checked));
    const toggleTrack = document.createElement("span");
    toggleTrack.className = "switch-track";
    toggleLabel.appendChild(toggleInput);
    toggleLabel.appendChild(toggleTrack);

    const toggleText = document.createElement("span");
    toggleText.dataset.role = "switch-select-label";
    toggleText.textContent = "In 1";

    const ccLabel = document.createElement("div");
    ccLabel.className = "button-cc-label";
    ccLabel.dataset.role = "switch-cc-label";

    body.appendChild(toggleLabel);
    body.appendChild(toggleText);
    body.appendChild(ccLabel);
    switchBody = body;

  }

  if (type === "const") {
    constStates.set(id, { value: 0 });
    audioEngine.createNode(id, "const", { value: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body const-body";

    const input = document.createElement("input");
    input.type = "text";
    input.inputMode = "decimal";
    input.className = "const-input";
    input.dataset.role = "const-input";
    input.value = "0.00";
    input.addEventListener("mousedown", (e) => e.stopPropagation());
    input.addEventListener("click", (e) => e.stopPropagation());
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") input.blur();
    });
    input.addEventListener("change", () => {
      setConstValue(id, parseFloatFlexible(input.value));
    });

    body.appendChild(input);
    el.appendChild(body);
  }

  if (type === "detune") {
    detuneStates.set(id, { amount: 0, cv: CV_MAX_VOLTS / 2, audioMode: false });
  }

  const ports = document.createElement("div");
  ports.className = "widget-ports";

  if (type === "multiplier") {
    renderMultiplierPorts(id, ports);
  } else if (type === "adder") {
    renderAdderPorts(id, ports);
  } else if (type === "divide") {
    renderDividePorts(id, ports);
  } else if (type === "mixer") {
    renderMixerPorts(id, ports);
  } else if (type === "multiplexer") {
    renderMultiplexerPorts(id, ports);
  } else if (type === "hammond") {
    renderHammondPorts(id, ports);
  } else if (type === "midi-split") {
    renderMidiSplitPorts(id, ports);
  } else {
    def.inputs.forEach((port) => {
      const row = createPortRow(id, port, "in", type);
      if (type === "value-mapper" && port.id === "in") {
        const label = row.querySelector(".port-label");
        if (label) {
          const value = document.createElement("span");
          value.className = "value-mapper-port-value";
          value.dataset.role = "value-mapper-in-value";
          value.textContent = formatNumber(valueMapperStates.get(id)?.value ?? 0);
          label.appendChild(value);
        }
      }
      const isAudioOut = type === "audio-out" || type === "usb-out" ||
        type === "es8311-out" || type === "wm8960-out";
      if (type === "audio-input-link" && /^gain[1-8]$/.test(port.id)) {
        const channel = Number(port.id.slice(4)) - 1;
        const state = audioInputLinkStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state?.gains[channel] ?? CV_MAX_VOLTS);
        slider.dataset.role = `audio-input-link-gain-${channel}`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => setAudioInputLinkGain(id, channel, Number(slider.value)));
        const value = document.createElement("span");
        value.className = "port-value";
        value.dataset.role = `audio-input-link-gain-value-${channel}`;
        value.textContent = `${formatNumber(state?.gains[channel] ?? CV_MAX_VOLTS)} V`;
        row.append(slider, value);
      }
      if (isAudioOut && (port.id === "gain" || port.id === "muted")) {
        const controlRow = el.querySelector(port.id === "gain" ? ".audio-out-volume-row" : ".audio-out-mute-row");
        const dot = row.querySelector(".port-dot");
        if (controlRow && dot) controlRow.prepend(dot);
        return;
      }
      if (isAudioOut && (port.id === "left" || port.id === "right")) {
        row.classList.add("audio-out-port-row");
        row.appendChild(buildAudioMeterTrack(`audio-out-meter-${port.id}`));
      }
      if (type === "debug" && port.id === "in") {
        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "debug-value";
        valueSpan.textContent = "–";
        row.appendChild(valueSpan);
      }
      if (type === "voltmeter" && port.id === "in") {
        const readout = el.querySelector('[data-role="voltmeter-readout"]');
        if (readout) row.appendChild(readout);
      }
      if ((type === "vu-meter" || type === "vu-meter-horizontal") && port.id === "in") {
        const readout = el.querySelector('[data-role="vu-readout"]');
        if (readout) row.appendChild(readout);
      }
      if (type === "midi-in" && port.id === "voices") {
        const state = midiInStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = String(MIDI_IN_MIN_VOICES);
        slider.max = String(midiInVoiceLimit(id));
        slider.step = "1";
        slider.value = String(state.voices);
        slider.dataset.role = "midi-in-voices-slider";
        slider.addEventListener("input", () => setMidiInVoices(id, Number(slider.value)));
        row.appendChild(slider);
        const value = document.createElement("span");
        value.className = "port-value";
        value.dataset.role = "midi-in-voices-value";
        value.textContent = String(state.voices);
        row.appendChild(value);
      }
      if (type === "midi-in" && port.id === "glide") {
        const state = midiInStates.get(id);
        const switchLabel = document.createElement("label");
        switchLabel.className = "switch";
        const toggle = document.createElement("input");
        toggle.type = "checkbox";
        toggle.checked = state.glide;
        toggle.dataset.role = "midi-in-glide-switch";
        toggle.addEventListener("change", () => setMidiInGlide(id, toggle.checked));
        const track = document.createElement("span");
        track.className = "switch-track";
        switchLabel.append(toggle, track);
        row.insertBefore(switchLabel, row.querySelector(".port-label"));
      }
      if (type === "midi-in" && port.id === "glideTime") {
        const state = midiInStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = "2";
        slider.step = "0.01";
        slider.value = String(state.glideTime);
        slider.dataset.role = "midi-in-glide-time-slider";
        slider.addEventListener("input", () => setMidiInGlideTime(id, Number(slider.value)));
        row.appendChild(slider);
        const value = document.createElement("span");
        value.className = "port-value";
        value.dataset.role = "midi-in-glide-time-value";
        value.textContent = `${formatNumber(state.glideTime)} s`;
        row.appendChild(value);
      }
      if (type === "adsr" && ["att", "dec", "sus", "rel"].includes(port.id)) {
        const adsrState = adsrStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(adsrSliderVolts(adsrState, port.id));
        slider.dataset.role = `adsr-slider-${port.id}`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, port.id, parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `adsr-${port.id}-value`;
        valueSpan.textContent = formatAdsrValue(adsrState, port.id);
        row.appendChild(valueSpan);
      }
      if (type === "clock" && port.id === "tempo") {
        const state = clockStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.cvVolts);
        slider.dataset.role = "clock-cv-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, port.id, parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "clock-cv-value";
        valueSpan.textContent = `${formatNumber(state.cvVolts)} V`;
        row.appendChild(valueSpan);
      }
      if (type === "lfo" && port.id === "oneShot") {
        const state = lfoStates.get(id);
        const switchLabel = document.createElement("label");
        switchLabel.className = "switch";
        const toggle = document.createElement("input");
        toggle.type = "checkbox";
        toggle.checked = state.oneShot === true;
        toggle.dataset.role = "lfo-oneshot-switch";
        toggle.addEventListener("mousedown", (e) => e.stopPropagation());
        toggle.addEventListener("click", (e) => e.stopPropagation());
        toggle.addEventListener("change", () => setLfoOneShot(id, toggle.checked));
        const switchTrack = document.createElement("span");
        switchTrack.className = "switch-track";
        switchLabel.appendChild(toggle);
        switchLabel.appendChild(switchTrack);
        row.insertBefore(switchLabel, row.querySelector(".port-label"));
      }
      if ((type === "lfo" || type === "gain" || type === "filter" || type === "curtis-filter") && port.id === "global") {
        const stateMap = { lfo: lfoStates, gain: gainStates, filter: filterStates, "curtis-filter": filterStates }[type];
        const state = stateMap.get(id);
        const switchLabel = document.createElement("label");
        switchLabel.className = "switch";
        const toggle = document.createElement("input");
        toggle.type = "checkbox";
        toggle.checked = state.global === true;
        toggle.dataset.role = `${type}-global-switch`;
        toggle.addEventListener("mousedown", (e) => e.stopPropagation());
        toggle.addEventListener("click", (e) => e.stopPropagation());
        toggle.addEventListener("change", () => {
          if (type === "lfo") setLfoGlobal(id, toggle.checked);
          else if (type === "gain") setGainGlobal(id, toggle.checked);
          else setFilterGlobal(id, toggle.checked);
        });
        const switchTrack = document.createElement("span");
        switchTrack.className = "switch-track";
        switchLabel.appendChild(toggle);
        switchLabel.appendChild(switchTrack);
        row.insertBefore(switchLabel, row.querySelector(".port-label"));
      }
      if (type === "lfo" && port.id === "zeroPoint") {
        
        
        
        
        
        const state = lfoStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.zeroPoint);
        slider.dataset.role = "lfo-zeropoint-port-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "zeroPoint", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "lfo-zeropoint-port-value";
        valueSpan.textContent = `${formatNumber(state.zeroPoint)} V`;
        row.appendChild(valueSpan);
      }
      if ((type === "filter" || type === "curtis-filter") && (port.id === "cutoff" || port.id === "resonance")) {
        const filterState = filterStates.get(id);
        const isCutoff = port.id === "cutoff";

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(isCutoff ? filterState.cutoff : filterState.resonance);
        slider.dataset.role = `filter-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, port.id, parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `filter-${port.id}-value`;
        valueSpan.textContent = formatFilterCvValue(filterState, port.id);
        row.appendChild(valueSpan);
      }
      if (type === "soundfont-player" && (port.id === "bank" || port.id === "program")) {
        const state = soundFontPlayerStates.get(id);
        const isBank = port.id === "bank";
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = isBank ? String(state.maxBank ?? SOUNDFONT_DEFAULT_MAX_BANK) : "127";
        slider.step = "1";
        slider.value = String(state[port.id]);
        slider.dataset.role = `soundfont-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, port.id, Number(slider.value)));
        row.appendChild(slider);
        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `soundfont-${port.id}-value`;
        valueSpan.textContent = String(state[port.id]);
        row.appendChild(valueSpan);
        if (port.id === "program") {
          
          
          
          const label = row.querySelector(".port-label");
          if (label) {
            label.style.cursor = "pointer";
            label.title = "Double-click to browse presets";
            label.addEventListener("dblclick", (e) => {
              e.stopPropagation();
              openSoundFontPresetPicker(id);
            });
          }
        }
      }
      if (type === "soundfont-player" && ["enableCc", "enableModWheel", "enablePitchWheel"].includes(port.id)) {
        const state = soundFontPlayerStates.get(id);
        const switchLabel = document.createElement("label");
        switchLabel.className = "switch";
        const toggle = document.createElement("input");
        toggle.type = "checkbox";
        toggle.checked = state[port.id] === true;
        toggle.dataset.role = `soundfont-${port.id}-switch`;
        toggle.addEventListener("mousedown", (e) => e.stopPropagation());
        toggle.addEventListener("click", (e) => e.stopPropagation());
        toggle.addEventListener("change", () => deliverToWidget(id, port.id, toggle.checked ? 1 : 0));
        const switchTrack = document.createElement("span");
        switchTrack.className = "switch-track";
        switchLabel.appendChild(toggle);
        switchLabel.appendChild(switchTrack);
        
        
        
        row.insertBefore(switchLabel, row.querySelector(".port-label"));
      }
      if (type === "soundfont-player" && port.id === "gain") {
        const state = soundFontPlayerStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.gain ?? CV_MAX_VOLTS);
        slider.dataset.role = "soundfont-gain-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "gain", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "soundfont-gain-value";
        valueSpan.textContent = formatNumber(state.gain ?? CV_MAX_VOLTS);
        row.appendChild(valueSpan);
      }
      if (hasBypass(type) && port.id === "bypass") {
        const state = bypassStateMapFor(type).get(id);
        const switchLabel = document.createElement("label");
        switchLabel.className = "switch";
        const toggle = document.createElement("input");
        toggle.type = "checkbox";
        toggle.checked = state.bypass;
        toggle.dataset.role = `${type}-bypass-switch`;
        toggle.addEventListener("mousedown", (e) => e.stopPropagation());
        toggle.addEventListener("click", (e) => e.stopPropagation());
        toggle.addEventListener("change", () => deliverToWidget(id, "bypass", toggle.checked ? GATE_ON_VOLTS : GATE_OFF_VOLTS));
        const switchTrack = document.createElement("span");
        switchTrack.className = "switch-track";
        switchLabel.appendChild(toggle);
        switchLabel.appendChild(switchTrack);
        row.insertBefore(switchLabel, row.querySelector(".port-label"));
      }
      if (ESP_EFFECT_SLIDER_CONFIG[type]?.[port.id]) {
        const effectState = espEffectStates.get(id);
        const cfg = ESP_EFFECT_SLIDER_CONFIG[type][port.id];

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = String(cfg.min);
        slider.max = String(cfg.max);
        slider.step = String(cfg.step);
        slider.value = String(effectState[port.id]);
        slider.dataset.role = `esp-effect-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, port.id, parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `esp-effect-${port.id}-value`;
        valueSpan.textContent = formatEspEffectValue(cfg, effectState[port.id]);
        row.appendChild(valueSpan);
      }
      if (type === "chorus" && ["rate", "depth", "offset"].includes(port.id)) {
        const chorusState = chorusStates.get(id);
        const cfg = CHORUS_SLIDER_CONFIG[port.id];

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = String(cfg.min);
        slider.max = String(cfg.max);
        slider.step = String(cfg.step);
        slider.value = String(chorusState[port.id]);
        slider.dataset.role = `chorus-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, port.id, parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `chorus-${port.id}-value`;
        valueSpan.textContent = formatChorusValue(port.id, chorusState[port.id]);
        row.appendChild(valueSpan);
      }
      if (effectCvParamDef(type, port.id)) {
        const state = effectCvStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state[port.id].volts);
        slider.dataset.role = `effect-cv-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, port.id, parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `effect-cv-${port.id}-value`;
        valueSpan.textContent = formatEffectCvValue(type, port.id, state[port.id]);
        row.appendChild(valueSpan);
      }
      if (type === "oscillator" && port.id === "tune") {
        const oscState = oscillatorStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(oscState.tuneVolts);
        slider.dataset.role = "oscillator-tune-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "tune", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "oscillator-tune-value";
        valueSpan.textContent = formatNumber(oscState.tuneVolts);
        row.appendChild(valueSpan);

        
        
        
        
        const resetTune = () => {
          if (connections.some((c) => c.to.widgetId === id && c.to.portId === "tune")) return;
          deliverToWidget(id, "tune", CV_MAX_VOLTS / 2);
        };
        const tuneLabel = row.querySelector(".port-label");
        if (tuneLabel) {
          tuneLabel.addEventListener("dblclick", (e) => {
            e.stopPropagation();
            resetTune();
          });
        }
        valueSpan.addEventListener("dblclick", (e) => {
          e.stopPropagation();
          resetTune();
        });
      }
      if (type === "oscillator" && port.id === "gain") {
        const oscState = oscillatorStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(oscState.gain);
        slider.dataset.role = "oscillator-gain-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "gain", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "oscillator-gain-value";
        valueSpan.textContent = formatNumber(oscState.gain);
        row.appendChild(valueSpan);
      }
      if (type === "detune" && port.id === "amount") {
        const detuneState = detuneStates.get(id);
        const initialCents = Math.round(detuneAmountToCents(detuneState.amount) / DETUNE_CENTS_STEP) * DETUNE_CENTS_STEP;

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = String(-DETUNE_MAX_CENTS);
        slider.max = String(DETUNE_MAX_CENTS);
        slider.step = String(DETUNE_CENTS_STEP);
        slider.value = String(initialCents);
        slider.dataset.role = "detune-amount-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "amount", detuneCentsToAmount(parseFloat(slider.value)));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "detune-amount-value";
        valueSpan.textContent = `${initialCents > 0 ? "+" : ""}${initialCents} ct`;
        row.appendChild(valueSpan);
      }
      if (type === "frequency-divider" && port.id === "divider") {
        const state = frequencyDividerStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(FREQUENCY_DIVIDER_STEPS.length - 1);
        slider.step = "1";
        slider.value = String(frequencyDividerStepIndex(state.divider));
        slider.dataset.role = "frequency-divider-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "divider", FREQUENCY_DIVIDER_STEPS[Number(slider.value)]);
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "frequency-divider-value";
        valueSpan.textContent = String(state.divider);
        row.appendChild(valueSpan);
      }
      if (type === "gain" && port.id === "amount") {
        const gainState = gainStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(gainState.amount);
        slider.dataset.role = "gain-amount-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "amount", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "gain-amount-value";
        valueSpan.textContent = formatGainCvValue(gainState);
        row.appendChild(valueSpan);
      }
      if (type === "lfo" && (port.id === "rate" || port.id === "amplitude" || port.id === "phase")) {
        const lfoState = lfoStates.get(id);
        const cfg = LFO_SLIDER_CONFIG[port.id];

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = String(cfg.min);
        slider.max = String(port.id === "rate" ? lfoState.maxRate : cfg.max);
        slider.step = String(cfg.step);
        slider.value = String(lfoState[port.id]);
        slider.dataset.role = `lfo-${port.id}-slider`;
        if (port.id === "phase") {
          slider.setAttribute("list", "lfo-phase-detents");
          ensurePhaseDetentDatalist();
        }
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          let value = parseFloat(slider.value);
          if (port.id === "phase") {
            value = snapToDetent(value, LFO_PHASE_DETENTS, LFO_PHASE_SNAP_RANGE);
            slider.value = String(value);
          }
          if (port.id === "rate") setLfoManualRate(id, value);
          else deliverToWidget(id, port.id, value);
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `lfo-${port.id}-value`;
        valueSpan.textContent = formatLfoValue(port.id, lfoState[port.id]);
        row.appendChild(valueSpan);
      }
      ports.appendChild(row);
    });
    def.outputs.forEach((port) => {
      const row = createPortRow(id, port, "out", type);
      if (type === "clock" && port.id === "pulse") {
        const pulseRow = el.querySelector(".clock-pulse-row");
        if (pulseRow) pulseRow.appendChild(row);
        return;
      }
      if (type === "abs" && port.id === "value") {
        row.classList.add("abs-output-row");
        const readout = document.createElement("span");
        readout.className = "port-value";
        readout.dataset.role = "abs-readout";
        readout.textContent = formatNumber(Math.abs(absStates.get(id)?.value ?? 0));
        row.appendChild(readout);
      }
      if (type === "value-mapper" && port.id === "out") {
        const label = row.querySelector(".port-label");
        if (label) {
          const value = document.createElement("span");
          value.className = "value-mapper-port-value";
          value.dataset.role = "value-mapper-out-value";
          value.textContent = formatNumber(mapValueMapperValue(valueMapperStates.get(id)));
          label.appendChild(value);
        }
      }
      ports.appendChild(row);
    });
  }

  if (type === "switch") {
    const switchContent = document.createElement("div");
    switchContent.className = "switch-content";
    switchContent.appendChild(ports);
    switchContent.appendChild(switchBody);
    const switchCcLabel = switchBody.querySelector('[data-role="switch-cc-label"]');
    if (switchCcLabel) switchContent.appendChild(switchCcLabel);
    el.appendChild(switchContent);
  } else if (type === "multiplexer") {
    const multiplexerContent = document.createElement("div");
    multiplexerContent.className = "switch-content multiplexer-content";
    multiplexerContent.appendChild(ports);
    multiplexerContent.appendChild(multiplexerSwitchBody);
    const routing = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    routing.classList.add("multiplexer-routing");
    routing.setAttribute("aria-hidden", "true");
    routing.setAttribute("fill", "none");
    const routingPath = document.createElementNS("http://www.w3.org/2000/svg", "path");
    routingPath.dataset.role = "multiplexer-routing-path";
    routingPath.setAttribute("fill", "none");
    routing.appendChild(routingPath);
    multiplexerContent.appendChild(routing);
    el.appendChild(multiplexerContent);
  } else if (type === "knob") {
    ports.classList.add("knob-output-ports");
    const knobBody = el.querySelector(".knob-body");
    if (knobBody) knobBody.appendChild(ports);
  } else if (type === "button") {
    ports.classList.add("button-output-ports");
    const buttonControlRow = el.querySelector(".button-control-row");
    if (buttonControlRow) buttonControlRow.appendChild(ports);
    else el.appendChild(ports);
  } else if (type === "up-down-button") {
    ports.classList.add("up-down-button-output-ports");
    const pressRow = el.querySelector(".up-down-button-press-row");
    if (pressRow) pressRow.appendChild(ports);
    else el.appendChild(ports);
  } else if (type === "const") {
    ports.classList.add("const-output-ports");
    const constBody = el.querySelector(".const-body");
    if (constBody) constBody.appendChild(ports);
    else el.appendChild(ports);
  } else if (type !== "annotation") {
    el.appendChild(ports);
  }

  if (type === "esp-eq") {
    const scopeBody = document.createElement("div");
    scopeBody.className = "widget-instance-body spectrum-body";

    const canvas = document.createElement("canvas");
    canvas.className = "spectrum-canvas";
    canvas.dataset.role = "esp-eq-canvas";
    canvas.width = 220;
    canvas.height = 80;

    scopeBody.appendChild(canvas);
    el.appendChild(scopeBody);
  }

  canvasContent.appendChild(el);
  if (type === "switch") updateSwitchVisual(id);
  if (type === "button") updateButtonVisual(id);
  if (type === "up-down-button") updateUpDownButtonVisual(id);
  if (audioMonitorStates.has(id)) updateAudioMonitorRangeVisual(id);
  
  
  
  if (type !== "annotation") layoutPortsGrid(ports);
  if (type === "multiplexer") {
    updateMultiplexerVisual(id);
    requestAnimationFrame(() => updateMultiplexerRouting(id));
  }
  if (type === "midi-in") updateMidiInControls(id);
  savePresetNow(); 
  return el;
}

function removeWidget(id, force = false) {
  const el = document.getElementById(id);
  if (!force && el && LINK_WIDGET_TYPES.includes(el.dataset.widgetType)) return;
  selectedWidgetIds.delete(id);
  removeConnectionsForWidget(id);
  knobStates.delete(id);
  buttonStates.delete(id);
  upDownButtonStates.delete(id);
  switchStates.delete(id);
  midiInStates.delete(id);
  midiVoiceSlotStates.delete(id);
  midiSplitStates.delete(id);
  multiplierStates.delete(id);
  adderStates.delete(id);
  divideStates.delete(id);
  frequencyDividerStates.delete(id);
  absStates.delete(id);
  valueMapperStates.delete(id);
  stopClockTimer(id);
  clockStates.delete(id);
  stopLfoTimer(id);
  lfoStates.delete(id);
  oscillatorStates.delete(id);
  hammondStates.delete(id);
  wavePlayerStates.delete(id);
  soundFontPlayerStates.delete(id);
  detuneStates.delete(id);
  audioOutStates.delete(id);
  audioInputLinkStates.delete(id);
  gainStates.delete(id);
  filterStates.delete(id);
  espEffectStates.delete(id);
  mixerStates.delete(id);
  multiplexerStates.delete(id);
  adsrStates.delete(id);
  constStates.delete(id);
  voltmeterStates.delete(id);
  audioMonitorStates.delete(id);
  annotationStates.delete(id);
  annotationSelections.delete(id);
  clearTimeout(annotationSaveTimers.get(id));
  annotationSaveTimers.delete(id);
  chorusStates.delete(id);
  effectCvStates.delete(id);
  audioEngine.removeNode(id);
  refreshControlGraph();
  refreshVoiceGroups();
  if (midiLearnTargetId === id) midiLearnTargetId = null;
  if (buttonMidiLearnTargetId === id) buttonMidiLearnTargetId = null;
  if (upDownMidiLearnTargetIdUp === id) upDownMidiLearnTargetIdUp = null;
  if (upDownMidiLearnTargetIdDown === id) upDownMidiLearnTargetIdDown = null;
  if (switchMidiLearnTargetId === id) switchMidiLearnTargetId = null;
  if (pendingKnobId === id) pendingKnobId = null;
  if (pendingButtonId === id) pendingButtonId = null;
  if (pendingUpDownButtonId === id) pendingUpDownButtonId = null;
  if (pendingSwitchId === id) pendingSwitchId = null;
  if (pendingMidiInId === id) pendingMidiInId = null;
  if (pendingMultiplierId === id) pendingMultiplierId = null;
  if (pendingAdderId === id) pendingAdderId = null;
  if (pendingClockId === id) pendingClockId = null;
  if (pendingMixerId === id) pendingMixerId = null;
  if (pendingWavePlayerId === id) pendingWavePlayerId = null;
  if (el) el.remove();
}

function clearCanvas() {
  
  
  
  
  const ids = Array.from(canvasContent.querySelectorAll(".widget-instance")).map((el) => el.id);
  ids.forEach((id) => removeWidget(id, true));
  drawConnections();
}

function loadInitTemplate() {
  
  
  
  
  
  switchPage("generator");
  clearCanvas();
  const oscEl = createWidget("oscillator", "WaveGen", 100, 100);
  ensureSingletonLinkWidget("generator");
  const outputLinkEl = pages.generator.canvasContentEl.querySelector(
    '[data-widget-type="audio-output-link"]'
  );
  ["in1", "in2"].forEach((portId) => {
    connectionCounter += 1;
    connections.push({
      id: `conn-${connectionCounter}`,
      from: { widgetId: oscEl.id, portId: "value" },
      to: { widgetId: outputLinkEl.id, portId },
      color: getConnectionColorForOutput(oscEl.id, "value"),
    });
    audioEngine.connect(oscEl.id, "value", outputLinkEl.id, portId);
  });
  drawConnections();

  switchPage("fx");
  clearCanvas();
  ensureSingletonLinkWidget("fx");
  const inputLinkEl = pages.fx.canvasContentEl.querySelector('[data-widget-type="audio-input-link"]');

  
  
  const outDefs = [
    { type: "audio-out", label: "JS Audio-Out", x: 60, y: 100 },
    { type: "usb-out", label: "USB-Out", x: 320, y: 100 },
    { type: "es8311-out", label: "ES8311-Out", x: 580, y: 100 },
  ];
  if (globalThis.__wm8960Enabled) {
    outDefs.push({ type: "wm8960-out", label: "WM8960-Out", x: 840, y: 100 });
  }
  outDefs.forEach((def) => {
    const outEl = createWidget(def.type, def.label, def.x, def.y);
    [
      { portId: "out1", channel: "left" },
      { portId: "out2", channel: "right" },
    ].forEach(({ portId, channel }) => {
      connectionCounter += 1;
      connections.push({
        id: `conn-${connectionCounter}`,
        from: { widgetId: inputLinkEl.id, portId },
        to: { widgetId: outEl.id, portId: channel },
        color: getConnectionColorForOutput(inputLinkEl.id, portId),
      });
      audioEngine.connect(inputLinkEl.id, portId, outEl.id, channel);
    });
  });
  drawConnections();

  switchPage("generator");
  savePresetNow(); 
  showToast("Default template loaded");
}

function startWidgetRename(labelSpan) {
  const currentName = labelSpan.textContent;
  const input = document.createElement("input");
  input.type = "text";
  input.className = "widget-rename-input";
  input.value = currentName;
  labelSpan.replaceWith(input);
  input.focus();
  input.select();

  function commit() {
    input.removeEventListener("blur", commit);
    const newName = input.value.trim() || currentName;
    labelSpan.textContent = newName;
    input.replaceWith(labelSpan);
    markDirty();
  }

  input.addEventListener("blur", commit);
  input.addEventListener("mousedown", (ev) => ev.stopPropagation());
  input.addEventListener("keydown", (ev) => {
    ev.stopPropagation();
    if (ev.key === "Enter") {
      ev.preventDefault();
      input.blur();
    } else if (ev.key === "Escape") {
      input.value = currentName;
      input.blur();
    }
  });
}




















function layoutPortsGrid(portsEl) {
  const inputRows = Array.from(portsEl.querySelectorAll(".port-row-in"));
  const rowsByControlType = [
    (row) => !row.querySelector(".port-slider, .switch"),
    (row) => Boolean(row.querySelector(".port-slider")),
    (row) => Boolean(row.querySelector(".switch")),
  ];
  rowsByControlType.forEach((matches) => {
    inputRows.filter(matches).forEach((row) => portsEl.appendChild(row));
  });

  const inRows = Array.from(portsEl.querySelectorAll(".port-row-in"));
  const outRows = Array.from(portsEl.querySelectorAll(".port-row-out"));
  const isSoundFontPlayer = portsEl.closest('[data-widget-type="soundfont-player"]') !== null;
  const isMidiIn = portsEl.closest('[data-widget-type="midi-in"]') !== null;
  const isAudioInputLink = portsEl.closest('[data-widget-type="audio-input-link"]') !== null;
  const isDetune = portsEl.closest('[data-widget-type="detune"]') !== null;

  outRows.forEach((row) => {
    row.style.position = "";
    row.style.top = "";
    row.style.right = "";
  });
  portsEl.style.minHeight = "";
  if (inRows.length === 0 || outRows.length === 0) return;

  
  
  
  
  
  const rowHeight = inRows[0].offsetHeight || 26;
  const switchControl = portsEl.closest('[data-widget-type="switch"]')?.querySelector('.switch-body > .switch');
  if (switchControl && inRows.length >= 2) {
    const switchHeight = rowHeight * 2;
    switchControl.style.height = `${switchHeight}px`;
    switchControl.style.setProperty("--switch-offset", `${Math.max(0, (rowHeight - 12) / 2)}px`);
    switchControl.style.setProperty("--switch-travel", `${rowHeight}px`);
  }
  const multiplexerBody = portsEl.closest('[data-widget-type="multiplexer"]')?.querySelector('.multiplexer-switch-body');
  const multiplexerControl = multiplexerBody?.querySelector('.multiplexer-switch');
  const multiplexerRows = inRows.filter((row) => row.classList.contains("mux-in-row"));
  const paddingTop = parseFloat(getComputedStyle(portsEl).paddingTop) || 0;
  if (multiplexerBody && multiplexerControl && multiplexerRows.length > 0) {
    const firstMultiplexerRow = inRows.indexOf(multiplexerRows[0]);
    multiplexerBody.style.top = `${paddingTop + firstMultiplexerRow * rowHeight}px`;
    multiplexerControl.style.height = `${rowHeight * multiplexerRows.length}px`;
    multiplexerControl.style.setProperty("--switch-offset", `${Math.max(0, (rowHeight - 12) / 2)}px`);
  }
  const outputRowStart = isMidiIn ? 2 : 0;
  const outputTop = isSoundFontPlayer
    ? -(portsEl.previousElementSibling?.offsetHeight || 0)
    : paddingTop + outputRowStart * rowHeight;
  const outputCenterOffset = isAudioInputLink ? Math.max(0, (rowHeight - 14) / 2) : 0;
  const outputRowCount = outputRowStart + outRows.length + (isDetune ? 1 : 0);
  portsEl.style.minHeight = `${rowHeight * Math.max(inRows.length, outputRowCount)}px`;
  outRows.forEach((row, i) => {
    row.style.position = "absolute";
    // DeTune has a slider on its second input row. Keep second output label
    // below that row instead of painting "Tune Down" over the slider/value.
    const outputIndex = isDetune && i === 1 ? 2 : i;
    row.style.top = `${outputTop + outputCenterOffset + outputIndex * rowHeight}px`;
    row.style.right = "0";
  });
}

function createPortRow(widgetId, port, direction, widgetType) {
  const row = document.createElement("div");
  row.className = `port-row port-row-${direction}`;

  const dot = document.createElement("span");
  const kind = portKindFromType(widgetType, port.id, direction);
  dot.className = `port-dot port-${direction} port-kind-${kind}`;
  dot.dataset.widgetId = widgetId;
  dot.dataset.portId = port.id;
  dot.dataset.portDir = direction;

  const labelEl = document.createElement("span");
  labelEl.className = "port-label";
  labelEl.textContent = port.label;

  if (direction === "out") {
    row.appendChild(labelEl);
    row.appendChild(dot);
    dot.addEventListener("mousedown", (e) => startConnectionDrag(e, dot));
  } else {
    row.appendChild(dot);
    row.appendChild(labelEl);
  }

  return row;
}



const knobStates = new Map();
let midiLearnTargetId = null;

const constStates = new Map();

const voltmeterStates = new Map();
const audioMonitorStates = new Map();
const annotationStates = new Map();
const annotationSelections = new Map();
const annotationSaveTimers = new Map();

const ANNOTATION_DEFAULT_WIDTH = 300;
const ANNOTATION_DEFAULT_HEIGHT = 180;
const ANNOTATION_MIN_WIDTH = 180;
const ANNOTATION_MIN_HEIGHT = 100;
const ANNOTATION_MAX_WIDTH = 1200;
const ANNOTATION_MAX_HEIGHT = 900;
const ANNOTATION_DEFAULT_BACKGROUND = "default";
const ANNOTATION_CARD_BACKGROUND_FALLBACK = "#1e293b";

function sanitizeAnnotationBackground(value) {
  const color = String(value || "").trim();
  if (!color || color === ANNOTATION_DEFAULT_BACKGROUND) return ANNOTATION_DEFAULT_BACKGROUND;
  return /^#[0-9a-f]{6}$/i.test(color) ? color.toLowerCase() : ANNOTATION_DEFAULT_BACKGROUND;
}

function annotationBackgroundPickerColor(value) {
  if (/^#[0-9a-f]{6}$/i.test(value || "")) return value;
  const cardColor = getComputedStyle(document.documentElement).getPropertyValue("--card").trim();
  return /^#[0-9a-f]{6}$/i.test(cardColor) ? cardColor : ANNOTATION_CARD_BACKGROUND_FALLBACK;
}

function applyAnnotationAppearance(el, backgroundColor) {
  if (backgroundColor === ANNOTATION_DEFAULT_BACKGROUND) {
    el.style.removeProperty("background-color");
    el.style.removeProperty("--annotation-text-color");
    return;
  }
  el.style.backgroundColor = backgroundColor;
  const red = Number.parseInt(backgroundColor.slice(1, 3), 16);
  const green = Number.parseInt(backgroundColor.slice(3, 5), 16);
  const blue = Number.parseInt(backgroundColor.slice(5, 7), 16);
  const brightness = (red * 299 + green * 587 + blue * 114) / 1000;
  el.style.setProperty("--annotation-text-color", brightness >= 150 ? "#111827" : "#e2e8f0");
}

function sanitizeAnnotationHtml(html) {
  const source = document.createElement("template");
  source.innerHTML = String(html || "");
  const output = document.createElement("div");
  const allowedTags = new Set(["DIV", "P", "BR", "STRONG", "EM", "U", "UL", "OL", "LI", "SPAN", "FONT"]);
  const droppedTags = new Set(["SCRIPT", "STYLE", "IFRAME", "OBJECT", "EMBED", "IMG", "SVG", "MATH", "LINK", "META"]);
  const fontSizeMap = { "1": 10, "2": 12, "3": 14, "4": 18, "5": 24, "6": 32, "7": 40 };

  function safeColor(value) {
    const probe = document.createElement("span");
    probe.style.color = String(value || "").trim();
    return probe.style.color;
  }

  function copyChildren(from, to) {
    Array.from(from.childNodes).forEach((child) => copyNode(child, to));
  }

  function copyNode(node, parent) {
    if (node.nodeType === Node.TEXT_NODE) {
      parent.appendChild(document.createTextNode(node.textContent || ""));
      return;
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return;
    if (droppedTags.has(node.tagName)) return;
    if (!allowedTags.has(node.tagName) && node.tagName !== "B" && node.tagName !== "I") {
      copyChildren(node, parent);
      return;
    }

    let tag = node.tagName;
    if (tag === "B") tag = "STRONG";
    if (tag === "I") tag = "EM";
    if (tag === "FONT") tag = "SPAN";
    const clean = document.createElement(tag.toLowerCase());

    if (tag === "SPAN") {
      const color = safeColor(node.style.color || node.getAttribute("color"));
      if (color) clean.style.color = color;

      let size = 0;
      if (node.tagName === "FONT") size = fontSizeMap[node.getAttribute("size")] || 0;
      if (!size && /px$/i.test(node.style.fontSize || "")) size = Number.parseFloat(node.style.fontSize);
      if (Number.isFinite(size) && size > 0) clean.style.fontSize = `${clamp(size, 10, 48)}px`;

      const weight = node.style.fontWeight;
      if (weight === "bold" || Number.parseInt(weight, 10) >= 600) clean.style.fontWeight = "bold";
      if (node.style.fontStyle === "italic") clean.style.fontStyle = "italic";
      if ((node.style.textDecorationLine || node.style.textDecoration || "").includes("underline")) {
        clean.style.textDecoration = "underline";
      }
    }

    copyChildren(node, clean);
    parent.appendChild(clean);
  }

  copyChildren(source.content, output);
  return output.innerHTML;
}

function saveAnnotationSelection(widgetId) {
  const editor = document.getElementById(widgetId)?.querySelector('[data-role="annotation-editor"]');
  const selection = window.getSelection();
  if (!editor || !selection || selection.rangeCount === 0) return;
  const range = selection.getRangeAt(0);
  const container = range.commonAncestorContainer.nodeType === Node.TEXT_NODE
    ? range.commonAncestorContainer.parentNode
    : range.commonAncestorContainer;
  if (container === editor || editor.contains(container)) annotationSelections.set(widgetId, range.cloneRange());
}

function restoreAnnotationSelection(widgetId) {
  const editor = document.getElementById(widgetId)?.querySelector('[data-role="annotation-editor"]');
  if (!editor) return null;
  editor.focus({ preventScroll: true });
  const selection = window.getSelection();
  const saved = annotationSelections.get(widgetId);
  if (selection && saved && editor.contains(saved.commonAncestorContainer)) {
    selection.removeAllRanges();
    selection.addRange(saved);
  }
  return editor;
}

function beginAnnotationHistory(widgetId) {
  const state = annotationStates.get(widgetId);
  if (!state || state.historyOpen) return;
  pushHistorySnapshot();
  state.historyOpen = true;
}

function scheduleAnnotationSave(widgetId) {
  clearTimeout(annotationSaveTimers.get(widgetId));
  annotationSaveTimers.set(widgetId, setTimeout(() => {
    annotationSaveTimers.delete(widgetId);
    savePresetNow();
  }, 800));
}

function syncAnnotationState(widgetId, persist = true) {
  const state = annotationStates.get(widgetId);
  const el = document.getElementById(widgetId);
  const editor = el?.querySelector('[data-role="annotation-editor"]');
  if (!state || !el || !editor) return;
  state.html = sanitizeAnnotationHtml(editor.innerHTML);
  state.width = clamp(Math.round(el.offsetWidth), ANNOTATION_MIN_WIDTH, ANNOTATION_MAX_WIDTH);
  state.height = clamp(Math.round(el.offsetHeight), ANNOTATION_MIN_HEIGHT, ANNOTATION_MAX_HEIGHT);
  if (persist) {
    markDirty();
    scheduleAnnotationSave(widgetId);
  }
}

function applyAnnotationCommand(widgetId, command, value = null) {
  beginAnnotationHistory(widgetId);
  const editor = restoreAnnotationSelection(widgetId);
  if (!editor) return;
  document.execCommand(command, false, value);
  saveAnnotationSelection(widgetId);
  syncAnnotationState(widgetId);
}

function applyAnnotationFontSize(widgetId, size) {
  const px = clamp(Number(size) || 14, 10, 48);
  beginAnnotationHistory(widgetId);
  const editor = restoreAnnotationSelection(widgetId);
  if (!editor) return;
  document.execCommand("fontSize", false, "7");
  editor.querySelectorAll('font[size="7"]').forEach((font) => {
    const span = document.createElement("span");
    span.style.fontSize = `${px}px`;
    while (font.firstChild) span.appendChild(font.firstChild);
    font.replaceWith(span);
  });
  saveAnnotationSelection(widgetId);
  syncAnnotationState(widgetId);
}

function createAnnotationToolbarButton(widgetId, label, title, command) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `annotation-tool annotation-tool-${command}`;
  button.textContent = label;
  button.title = title;
  button.setAttribute("aria-label", title);
  button.addEventListener("mousedown", (event) => {
    event.preventDefault();
    event.stopPropagation();
    saveAnnotationSelection(widgetId);
  });
  button.addEventListener("click", (event) => {
    event.stopPropagation();
    applyAnnotationCommand(widgetId, command);
  });
  return button;
}

function startAnnotationResize(event, widgetId) {
  event.preventDefault();
  event.stopPropagation();
  const el = document.getElementById(widgetId);
  const state = annotationStates.get(widgetId);
  if (!el || !state) return;
  selectOnlyWidget(widgetId);

  const startX = event.clientX;
  const startY = event.clientY;
  const startWidth = el.offsetWidth;
  const startHeight = el.offsetHeight;
  let moved = false;

  function onMove(moveEvent) {
    if (!moved) {
      pushHistorySnapshot();
      moved = true;
    }
    const width = clamp(startWidth + (moveEvent.clientX - startX) / zoom, ANNOTATION_MIN_WIDTH, ANNOTATION_MAX_WIDTH);
    const height = clamp(startHeight + (moveEvent.clientY - startY) / zoom, ANNOTATION_MIN_HEIGHT, ANNOTATION_MAX_HEIGHT);
    el.style.width = `${Math.round(width)}px`;
    el.style.height = `${Math.round(height)}px`;
    state.width = Math.round(width);
    state.height = Math.round(height);
  }

  function onUp() {
    document.removeEventListener("mousemove", onMove);
    document.removeEventListener("mouseup", onUp);
    if (!moved) return;
    markDirty();
    savePresetNow();
  }

  document.addEventListener("mousemove", onMove);
  document.addEventListener("mouseup", onUp);
}

function createAnnotationBody(widgetId, el) {
  const state = {
    html: "",
    width: ANNOTATION_DEFAULT_WIDTH,
    height: ANNOTATION_DEFAULT_HEIGHT,
    backgroundColor: ANNOTATION_DEFAULT_BACKGROUND,
    historyOpen: false,
  };
  annotationStates.set(widgetId, state);
  el.style.width = `${state.width}px`;
  el.style.height = `${state.height}px`;
  applyAnnotationAppearance(el, state.backgroundColor);

  const body = document.createElement("div");
  body.className = "annotation-body";
  const toolbar = document.createElement("div");
  toolbar.className = "annotation-toolbar";
  toolbar.setAttribute("role", "toolbar");
  toolbar.setAttribute("aria-label", "Annotation formatting");
  toolbar.appendChild(createAnnotationToolbarButton(widgetId, "B", "Bold", "bold"));
  toolbar.appendChild(createAnnotationToolbarButton(widgetId, "I", "Italic", "italic"));
  toolbar.appendChild(createAnnotationToolbarButton(widgetId, "U", "Underline", "underline"));
  toolbar.appendChild(createAnnotationToolbarButton(widgetId, "•", "Bulleted list", "insertUnorderedList"));
  toolbar.appendChild(createAnnotationToolbarButton(widgetId, "1.", "Numbered list", "insertOrderedList"));

  const fontSize = document.createElement("select");
  fontSize.className = "annotation-font-size";
  fontSize.title = "Font size";
  fontSize.setAttribute("aria-label", "Font size");
  [10, 12, 14, 16, 18, 20, 24, 28, 32, 40, 48].forEach((size) => {
    const option = document.createElement("option");
    option.value = String(size);
    option.textContent = String(size);
    if (size === 14) option.selected = true;
    fontSize.appendChild(option);
  });
  fontSize.addEventListener("mousedown", (event) => {
    event.stopPropagation();
    saveAnnotationSelection(widgetId);
  });
  fontSize.addEventListener("click", (event) => event.stopPropagation());
  fontSize.addEventListener("change", () => applyAnnotationFontSize(widgetId, fontSize.value));
  toolbar.appendChild(fontSize);

  const textColorLabel = document.createElement("label");
  textColorLabel.className = "annotation-color-control";
  textColorLabel.title = "Text color";
  textColorLabel.textContent = "A";
  const textColor = document.createElement("input");
  textColor.type = "color";
  textColor.value = "#e2e8f0";
  textColor.setAttribute("aria-label", "Text color");
  textColor.addEventListener("mousedown", (event) => {
    event.stopPropagation();
    saveAnnotationSelection(widgetId);
  });
  textColor.addEventListener("click", (event) => event.stopPropagation());
  textColor.addEventListener("change", () => applyAnnotationCommand(widgetId, "foreColor", textColor.value));
  textColorLabel.appendChild(textColor);
  toolbar.appendChild(textColorLabel);

  const backgroundLabel = document.createElement("label");
  backgroundLabel.className = "annotation-color-control annotation-background-control";
  backgroundLabel.title = "Background color";
  backgroundLabel.textContent = "BG";
  const background = document.createElement("input");
  background.type = "color";
  background.value = annotationBackgroundPickerColor(state.backgroundColor);
  background.dataset.role = "annotation-background";
  background.setAttribute("aria-label", "Background color");
  background.addEventListener("mousedown", (event) => event.stopPropagation());
  background.addEventListener("click", (event) => event.stopPropagation());
  background.addEventListener("change", () => {
    pushHistorySnapshot();
    state.backgroundColor = sanitizeAnnotationBackground(background.value);
    applyAnnotationAppearance(el, state.backgroundColor);
    markDirty();
    savePresetNow();
  });
  backgroundLabel.appendChild(background);
  toolbar.appendChild(backgroundLabel);

  const editor = document.createElement("div");
  editor.className = "annotation-editor";
  editor.dataset.role = "annotation-editor";
  editor.dataset.placeholder = "Write a note…";
  editor.contentEditable = "true";
  editor.spellcheck = true;
  editor.setAttribute("role", "textbox");
  editor.setAttribute("aria-multiline", "true");
  editor.setAttribute("aria-label", "Annotation text");
  editor.addEventListener("mousedown", (event) => event.stopPropagation());
  editor.addEventListener("click", (event) => event.stopPropagation());
  editor.addEventListener("dblclick", (event) => event.stopPropagation());
  editor.addEventListener("focus", () => selectOnlyWidget(widgetId));
  editor.addEventListener("beforeinput", () => beginAnnotationHistory(widgetId));
  editor.addEventListener("input", () => {
    saveAnnotationSelection(widgetId);
    syncAnnotationState(widgetId);
  });
  editor.addEventListener("mouseup", () => saveAnnotationSelection(widgetId));
  editor.addEventListener("keyup", () => saveAnnotationSelection(widgetId));
  editor.addEventListener("blur", () => {
    state.historyOpen = false;
    syncAnnotationState(widgetId, false);
    clearTimeout(annotationSaveTimers.get(widgetId));
    annotationSaveTimers.delete(widgetId);
    savePresetNow();
  });
  editor.addEventListener("paste", (event) => {
    event.preventDefault();
    beginAnnotationHistory(widgetId);
    document.execCommand("insertText", false, event.clipboardData?.getData("text/plain") || "");
  });
  editor.addEventListener("drop", (event) => {
    event.preventDefault();
    beginAnnotationHistory(widgetId);
    restoreAnnotationSelection(widgetId);
    document.execCommand("insertText", false, event.dataTransfer?.getData("text/plain") || "");
  });

  const resizeHandle = document.createElement("div");
  resizeHandle.className = "annotation-resize-handle";
  resizeHandle.title = "Resize annotation";
  resizeHandle.setAttribute("aria-hidden", "true");
  resizeHandle.addEventListener("mousedown", (event) => startAnnotationResize(event, widgetId));

  body.appendChild(toolbar);
  body.appendChild(editor);
  el.appendChild(body);
  el.appendChild(resizeHandle);
}

function getAnnotationParams(widgetId) {
  syncAnnotationState(widgetId, false);
  const state = annotationStates.get(widgetId);
  if (!state) return {};
  return {
    html: state.html,
    width: state.width,
    height: state.height,
    backgroundColor: state.backgroundColor,
  };
}

function applyAnnotationParams(widgetId, params) {
  const state = annotationStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const width = Number(params?.width);
  const height = Number(params?.height);
  state.html = sanitizeAnnotationHtml(params?.html);
  state.width = clamp(Number.isFinite(width) ? Math.round(width) : ANNOTATION_DEFAULT_WIDTH, ANNOTATION_MIN_WIDTH, ANNOTATION_MAX_WIDTH);
  state.height = clamp(Number.isFinite(height) ? Math.round(height) : ANNOTATION_DEFAULT_HEIGHT, ANNOTATION_MIN_HEIGHT, ANNOTATION_MAX_HEIGHT);
  state.backgroundColor = sanitizeAnnotationBackground(params?.backgroundColor);
  state.historyOpen = false;
  el.style.width = `${state.width}px`;
  el.style.height = `${state.height}px`;
  applyAnnotationAppearance(el, state.backgroundColor);
  const editor = el.querySelector('[data-role="annotation-editor"]');
  const background = el.querySelector('[data-role="annotation-background"]');
  if (editor) editor.innerHTML = state.html;
  if (background) background.value = annotationBackgroundPickerColor(state.backgroundColor);
  annotationSelections.delete(widgetId);
}

function createAudioMonitorState() {
  return { rangeVolts: AUDIO_PEAK_VOLTS, peakVolts: 0, holdUntil: 0, lastFrameMs: performance.now() };
}

function createMonitorRangeControl(widgetId) {
  const control = document.createElement("div");
  control.className = "monitor-range-control";
  control.setAttribute("role", "group");
  control.setAttribute("aria-label", "Voltage range");
  [AUDIO_PEAK_VOLTS, CV_MAX_VOLTS].forEach((rangeVolts) => {
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.rangeVolts = String(rangeVolts);
    button.textContent = `${rangeVolts} V`;
    button.title = `Display range 0-${rangeVolts} V`;
    button.addEventListener("click", (event) => {
      event.stopPropagation();
      setAudioMonitorRange(widgetId, rangeVolts);
    });
    control.appendChild(button);
  });
  return control;
}

function setAudioMonitorRange(widgetId, rangeVolts, persist = true) {
  const state = audioMonitorStates.get(widgetId);
  if (!state) return;
  state.rangeVolts = Number(rangeVolts) === CV_MAX_VOLTS ? CV_MAX_VOLTS : AUDIO_PEAK_VOLTS;
  state.peakVolts = Math.min(state.peakVolts, state.rangeVolts);
  updateAudioMonitorRangeVisual(widgetId);
  if (persist) {
    markDirty();
    savePresetNow();
  }
}

function updateAudioMonitorRangeVisual(widgetId) {
  const state = audioMonitorStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll("[data-range-volts]").forEach((button) => {
    button.classList.toggle("active", Number(button.dataset.rangeVolts) === state.rangeVolts);
  });
  el.querySelectorAll("[data-range-fraction]").forEach((label) => {
    const volts = state.rangeVolts * Number(label.dataset.rangeFraction);
    label.textContent = String(Number(volts.toFixed(2)));
  });
}

function updateConstInput(widgetId) {
  const state = constStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const input = el.querySelector('[data-role="const-input"]');
  if (input) input.value = formatNumber(state.value);
}

function setConstValue(widgetId, value) {
  const state = constStates.get(widgetId);
  if (!state) return;
  state.value = Number.isNaN(value) ? 0 : value;
  audioEngine.setParam(widgetId, "value", state.value);
  updateConstInput(widgetId);
  propagate(widgetId, "value", state.value);
  sendLiveParamUpdate(widgetId, { value: state.value });
}

function knobAngle(raw) {
  return -135 + (raw / 127) * 270;
}

function mapKnobValue(state) {
  const list = parseValueList(state.listText);
  if (list && list.length >= 2) {
    const idx = clamp(Math.floor((state.raw * list.length) / 128), 0, list.length - 1);
    return list[idx];
  }

  const { point1In, point1Out, point2In, point2Out, constraintMin, constraintMax } = state;

  let value;
  if (point2In === point1In) {
    value = point1Out;
  } else {
    const slope = (point2Out - point1Out) / (point2In - point1In);
    value = point1Out + (state.raw - point1In) * slope;
  }

  if (constraintMin !== null && value < constraintMin) value = constraintMin;
  if (constraintMax !== null && value > constraintMax) value = constraintMax;

  return value;
}

function updateKnobVisual(widgetId) {
  const state = knobStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const indicator = el.querySelector('[data-role="knob-indicator"]');
  if (indicator) indicator.style.transform = `rotate(${knobAngle(state.raw)}deg)`;

  const readout = el.querySelector('[data-role="knob-readout"]');
  if (readout) readout.textContent = formatNumber(mapKnobValue(state));

  const ccLabel = el.querySelector('[data-role="knob-cc-label"]');
  if (ccLabel) {
    ccLabel.textContent =
      state.cc === null ? "CC: –" : `CC ${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  }
}

function setKnobRaw(widgetId, raw) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  state.raw = clamp(Math.round(raw), 0, 127);
  const mapped = roundFloat(mapKnobValue(state));
  audioEngine.setParam(widgetId, "value", mapped);
  updateKnobVisual(widgetId);
  propagate(widgetId, "voltage", mapped);
  sendLiveParamUpdateThrottled(widgetId, { raw: state.raw });
}

function startKnobDrag(e, widgetId) {
  e.preventDefault();
  e.stopPropagation();
  const state = knobStates.get(widgetId);
  if (!state) return;

  const startY = e.clientY;
  const startRaw = state.raw;
  let moved = false;

  function onMove(ev) {
    const dy = startY - ev.clientY;
    if (Math.abs(dy) > 2) moved = true;
    setKnobRaw(widgetId, startRaw + dy / 2);
  }

  function onUp() {
    document.removeEventListener("mousemove", onMove);
    document.removeEventListener("mouseup", onUp);
    if (moved) markDirty();
  }

  document.addEventListener("mousemove", onMove);
  document.addEventListener("mouseup", onUp);
}

function routeMidiToKnobs(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;

  if (midiLearnTargetId) {
    const state = knobStates.get(midiLearnTargetId);
    if (state) {
      
      
      
      
      state.cc = data.control;
      state.channel = data.channel;
      knobCcDisplay.value = `${data.control} (Ch ${data.channel + 1})`;
      knobLearnBtn.textContent = "Learn";
      updateKnobVisual(midiLearnTargetId);
      sendLiveParamUpdate(midiLearnTargetId, { cc: data.control, channel: data.channel });
      
      
      
      
      
      savePresetNow();
    }
    midiLearnTargetId = null;
    return;
  }

  knobStates.forEach((state, widgetId) => {
    if (state.cc === data.control && state.channel === data.channel) {
      setKnobRaw(widgetId, data.value);
      markDirty();
    }
  });
}

const knobSettingsOverlay = document.getElementById("knobSettingsOverlay");
const knobCcDisplay = document.getElementById("knobCcDisplay");
const knobLearnBtn = document.getElementById("knobLearnBtn");
const knobPoint1InInput = document.getElementById("knobPoint1InInput");
const knobPoint1OutInput = document.getElementById("knobPoint1OutInput");
const knobPoint2InInput = document.getElementById("knobPoint2InInput");
const knobPoint2OutInput = document.getElementById("knobPoint2OutInput");
const knobConstraintMinInput = document.getElementById("knobConstraintMinInput");
const knobConstraintMaxInput = document.getElementById("knobConstraintMaxInput");
const knobValueListInput = document.getElementById("knobValueListInput");
const knobLinearInputs = [
  knobPoint1InInput,
  knobPoint1OutInput,
  knobPoint2InInput,
  knobPoint2OutInput,
  knobConstraintMinInput,
  knobConstraintMaxInput,
];
let pendingKnobId = null;




function updateKnobFieldsDisabledState() {
  const list = parseValueList(knobValueListInput.value);
  const listValid = !!(list && list.length >= 2);
  knobLinearInputs.forEach((input) => {
    input.disabled = listValid;
  });
}

knobValueListInput.addEventListener("input", updateKnobFieldsDisabledState);

function openKnobSettings(widgetId) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  pendingKnobId = widgetId;
  knobCcDisplay.value =
    state.cc === null ? "" : `${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  knobPoint1InInput.value = state.point1In;
  knobPoint1OutInput.value = state.point1Out;
  knobPoint2InInput.value = state.point2In;
  knobPoint2OutInput.value = state.point2Out;
  knobConstraintMinInput.value = state.constraintMin === null ? "" : state.constraintMin;
  knobConstraintMaxInput.value = state.constraintMax === null ? "" : state.constraintMax;
  knobValueListInput.value = state.listText || "";
  updateKnobFieldsDisabledState();
  knobLearnBtn.textContent = "Learn";
  midiLearnTargetId = null;
  knobSettingsOverlay.classList.remove("hidden");
}

function closeKnobSettings() {
  knobSettingsOverlay.classList.add("hidden");
  pendingKnobId = null;
  midiLearnTargetId = null;
  knobLearnBtn.textContent = "Learn";
}

knobSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === knobSettingsOverlay) closeKnobSettings();
});

document.getElementById("knobSettingsCancel").addEventListener("click", closeKnobSettings);

knobLearnBtn.addEventListener("click", () => {
  if (!pendingKnobId) return;
  midiLearnTargetId = pendingKnobId;
  knobLearnBtn.textContent = "Waiting for CC…";
});

document.getElementById("knobSettingsSave").addEventListener("click", () => {
  if (!pendingKnobId) return;
  const state = knobStates.get(pendingKnobId);
  if (state) {
    const p1In = parseFloatFlexible(knobPoint1InInput.value);
    const p1Out = parseFloatFlexible(knobPoint1OutInput.value);
    const p2In = parseFloatFlexible(knobPoint2InInput.value);
    const p2Out = parseFloatFlexible(knobPoint2OutInput.value);

    state.point1In = Number.isNaN(p1In) ? 0 : p1In;
    state.point1Out = Number.isNaN(p1Out) ? 0 : p1Out;
    state.point2In = Number.isNaN(p2In) ? 127 : p2In;
    state.point2Out = Number.isNaN(p2Out) ? CV_MAX_VOLTS : p2Out;

    let cMin = knobConstraintMinInput.value.trim() === "" ? null : parseFloatFlexible(knobConstraintMinInput.value);
    if (cMin !== null && Number.isNaN(cMin)) cMin = null;
    let cMax = knobConstraintMaxInput.value.trim() === "" ? null : parseFloatFlexible(knobConstraintMaxInput.value);
    if (cMax !== null && Number.isNaN(cMax)) cMax = null;
    if (cMin !== null && cMax !== null && cMin > cMax) {
      const swap = cMin;
      cMin = cMax;
      cMax = swap;
    }

    state.constraintMin = cMin;
    state.constraintMax = cMax;
    state.listText = knobValueListInput.value;

    const mapped = roundFloat(mapKnobValue(state));
    audioEngine.setParam(pendingKnobId, "value", mapped);
    propagate(pendingKnobId, "voltage", mapped);
    updateKnobVisual(pendingKnobId);
    sendLiveParamUpdate(pendingKnobId, {
      point1In: state.point1In,
      point1Out: state.point1Out,
      point2In: state.point2In,
      point2Out: state.point2Out,
      constraintMin: cMin,
      constraintMax: cMax,
      valueList: parseValueList(state.listText) || [],
    });
    savePresetNow(); 
  }
  closeKnobSettings();
});



const buttonStates = new Map();
let buttonMidiLearnTargetId = null;

function buttonCurrentOutput(state) {
  const list = parseVoltageValueList(state.listText);
  if (list && list.length >= 2) {
    const idx = clamp(state.listIndex, 0, list.length - 1);
    return list[idx];
  }
  return clampVoltage(state.wasAbove ? state.value127 : state.value0);
}

function publishButtonValue(widgetId) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  const output = buttonCurrentOutput(state);
  audioEngine.setParam(widgetId, "value", output);
  updateButtonVisual(widgetId);
  propagate(widgetId, "value", output);
  sendLiveParamUpdate(widgetId, {
    listIndex: state.listIndex,
    wasAbove: state.wasAbove,
  });
}

function advanceButtonValue(widgetId) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  const list = parseVoltageValueList(state.listText);
  if (list && list.length >= 2) {
    state.listIndex = (state.listIndex + 1) % list.length;
  } else {
    state.wasAbove = !state.wasAbove;
  }
  publishButtonValue(widgetId);
}

function updateButtonVisual(widgetId) {
  const state = buttonStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const readout = el.querySelector('[data-role="button-readout"]');
  const list = parseVoltageValueList(state.listText);
  const active = list && list.length >= 2 ? state.listIndex > 0 : state.wasAbove;
  if (readout) {
    readout.textContent = formatNumber(buttonCurrentOutput(state));
    readout.classList.toggle("is-active", active);
    readout.setAttribute("aria-pressed", String(active));
  }

  const ccLabel = el.querySelector('[data-role="button-cc-label"]');
  if (ccLabel) {
    ccLabel.textContent =
      state.cc === null ? "CC: –" : `CC ${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  }
}

function routeMidiToButtons(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;

  if (buttonMidiLearnTargetId) {
    const state = buttonStates.get(buttonMidiLearnTargetId);
    if (state) {
      state.cc = data.control;
      state.channel = data.channel;
      buttonCcDisplay.value = `${data.control} (Ch ${data.channel + 1})`;
      buttonLearnBtn.textContent = "Learn";
      updateButtonVisual(buttonMidiLearnTargetId);
      sendLiveParamUpdate(buttonMidiLearnTargetId, { cc: data.control, channel: data.channel });
      savePresetNow(); 
    }
    buttonMidiLearnTargetId = null;
    return;
  }

  buttonStates.forEach((state, widgetId) => {
    if (state.cc !== data.control || state.channel !== data.channel) return;

    const above = data.value >= 64;
    const list = parseVoltageValueList(state.listText);
    if (list && list.length >= 2) {
      if (above && !state.wasAbove) {
        state.listIndex = (state.listIndex + 1) % list.length;
      }
    }
    state.wasAbove = above;

    publishButtonValue(widgetId);
    markDirty();
  });
}

const buttonSettingsOverlay = document.getElementById("buttonSettingsOverlay");
const buttonCcDisplay = document.getElementById("buttonCcDisplay");
const buttonLearnBtn = document.getElementById("buttonLearnBtn");
const buttonValue0Input = document.getElementById("buttonValue0Input");
const buttonValue127Input = document.getElementById("buttonValue127Input");
const buttonValueListInput = document.getElementById("buttonValueListInput");
const buttonFixedInputs = [buttonValue0Input, buttonValue127Input];
let pendingButtonId = null;



function updateButtonFieldsDisabledState() {
  const list = parseVoltageValueList(buttonValueListInput.value);
  const listValid = !!(list && list.length >= 2);
  buttonFixedInputs.forEach((input) => {
    input.disabled = listValid;
  });
}

buttonValueListInput.addEventListener("input", updateButtonFieldsDisabledState);

function openButtonSettings(widgetId) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  pendingButtonId = widgetId;
  buttonCcDisplay.value =
    state.cc === null ? "" : `${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  buttonValue0Input.value = state.value0;
  buttonValue127Input.value = state.value127;
  const buttonList = parseVoltageValueList(state.listText);
  buttonValueListInput.value = buttonList ? buttonList.join(", ") : (state.listText || "");
  updateButtonFieldsDisabledState();
  buttonLearnBtn.textContent = "Learn";
  buttonMidiLearnTargetId = null;
  buttonSettingsOverlay.classList.remove("hidden");
}

function closeButtonSettings() {
  buttonSettingsOverlay.classList.add("hidden");
  pendingButtonId = null;
  buttonMidiLearnTargetId = null;
  buttonLearnBtn.textContent = "Learn";
}

buttonSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === buttonSettingsOverlay) closeButtonSettings();
});

document.getElementById("buttonSettingsCancel").addEventListener("click", closeButtonSettings);

buttonLearnBtn.addEventListener("click", () => {
  if (!pendingButtonId) return;
  buttonMidiLearnTargetId = pendingButtonId;
  buttonLearnBtn.textContent = "Waiting for CC…";
});

document.getElementById("buttonSettingsSave").addEventListener("click", () => {
  if (!pendingButtonId) return;
  const state = buttonStates.get(pendingButtonId);
  if (state) {
    const v0 = parseFloatFlexible(buttonValue0Input.value);
    const v127 = parseFloatFlexible(buttonValue127Input.value);
    state.value0 = clampVoltage(Number.isNaN(v0) ? 0 : v0);
    state.value127 = clampVoltage(Number.isNaN(v127) ? CV_MAX_VOLTS : v127, CV_MAX_VOLTS);
    const list = parseVoltageValueList(buttonValueListInput.value);
    state.listText = list ? list.join(", ") : buttonValueListInput.value;
    if (list && list.length >= 2) {
      state.listIndex = clamp(state.listIndex, 0, list.length - 1);
    }

    const output = buttonCurrentOutput(state);
    audioEngine.setParam(pendingButtonId, "value", output);
    propagate(pendingButtonId, "value", output);
    updateButtonVisual(pendingButtonId);
    sendLiveParamUpdate(pendingButtonId, {
      value0: state.value0,
      value127: state.value127,
      valueList: list || [],
    });
    savePresetNow(); 
  }
  closeButtonSettings();
});



const upDownButtonStates = new Map();
let upDownMidiLearnTargetIdUp = null;
let upDownMidiLearnTargetIdDown = null;

function upDownButtonCurrentOutput(state) {
  const list = parseVoltageValueList(state.listText);
  if (list && list.length >= 2) {
    const idx = clamp(state.listIndex, 0, list.length - 1);
    return list[idx];
  }
  return clampVoltage(state.currentValue);
}

function publishUpDownButtonValue(widgetId) {
  const state = upDownButtonStates.get(widgetId);
  if (!state) return;
  const output = upDownButtonCurrentOutput(state);
  audioEngine.setParam(widgetId, "value", output);
  updateUpDownButtonVisual(widgetId);
  propagate(widgetId, "value", output);
  sendLiveParamUpdate(widgetId, {
    listIndex: state.listIndex,
    currentValue: state.currentValue,
  });
}

function advanceUpDownButtonValue(widgetId, direction) {
  const state = upDownButtonStates.get(widgetId);
  if (!state) return;
  const list = parseVoltageValueList(state.listText);
  if (list && list.length >= 2) {
    if (direction === "up") {
      if (state.listIndex + 1 < list.length) state.listIndex += 1;
    } else if (state.listIndex > 0) {
      state.listIndex -= 1;
    }
  } else if (direction === "up") {
    state.currentValue = clamp(state.currentValue + state.step, state.min, state.max);
  } else {
    state.currentValue = clamp(state.currentValue - state.step, state.min, state.max);
  }
  publishUpDownButtonValue(widgetId);
}

function updateUpDownButtonVisual(widgetId) {
  const state = upDownButtonStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const readout = el.querySelector('[data-role="up-down-button-readout"]');
  if (readout) readout.textContent = formatNumber(upDownButtonCurrentOutput(state));

  const ccLabel = el.querySelector('[data-role="up-down-button-cc-label"]');
  if (ccLabel) {
    const upTxt =
      state.ccUp === null ? "–" : `${state.ccUp}${state.channelUp === null ? "" : ` (Ch ${state.channelUp + 1})`}`;
    const downTxt =
      state.ccDown === null
        ? "–"
        : `${state.ccDown}${state.channelDown === null ? "" : ` (Ch ${state.channelDown + 1})`}`;
    ccLabel.textContent = `▲ ${upTxt}  ▼ ${downTxt}`;
  }

}

function routeMidiToUpDownButtons(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;

  if (upDownMidiLearnTargetIdUp) {
    const state = upDownButtonStates.get(upDownMidiLearnTargetIdUp);
    if (state) {
      state.ccUp = data.control;
      state.channelUp = data.channel;
      upDownButtonCcUpDisplay.value = `${data.control} (Ch ${data.channel + 1})`;
      upDownButtonLearnUpBtn.textContent = "Learn";
      updateUpDownButtonVisual(upDownMidiLearnTargetIdUp);
      sendLiveParamUpdate(upDownMidiLearnTargetIdUp, { ccUp: data.control, channelUp: data.channel });
      savePresetNow(); 
    }
    upDownMidiLearnTargetIdUp = null;
    return;
  }
  if (upDownMidiLearnTargetIdDown) {
    const state = upDownButtonStates.get(upDownMidiLearnTargetIdDown);
    if (state) {
      state.ccDown = data.control;
      state.channelDown = data.channel;
      upDownButtonCcDownDisplay.value = `${data.control} (Ch ${data.channel + 1})`;
      upDownButtonLearnDownBtn.textContent = "Learn";
      updateUpDownButtonVisual(upDownMidiLearnTargetIdDown);
      sendLiveParamUpdate(upDownMidiLearnTargetIdDown, { ccDown: data.control, channelDown: data.channel });
      savePresetNow(); 
    }
    upDownMidiLearnTargetIdDown = null;
    return;
  }

  
  
  
  
  upDownButtonStates.forEach((state, widgetId) => {
    const matchedUp = state.ccUp !== null && state.ccUp === data.control && state.channelUp === data.channel;
    const matchedDown = state.ccDown !== null && state.ccDown === data.control && state.channelDown === data.channel;
    if (!matchedUp && !matchedDown) return;

    const list = parseVoltageValueList(state.listText);
    if (matchedUp) {
      if (list && list.length >= 2) {
        if (state.listIndex + 1 < list.length) state.listIndex += 1;
      } else {
        state.currentValue = clamp(state.currentValue + state.step, state.min, state.max);
      }
    }
    if (matchedDown) {
      if (list && list.length >= 2) {
        if (state.listIndex > 0) state.listIndex -= 1;
      } else {
        state.currentValue = clamp(state.currentValue - state.step, state.min, state.max);
      }
    }

    publishUpDownButtonValue(widgetId);
    markDirty();
  });
}

const upDownButtonSettingsOverlay = document.getElementById("upDownButtonSettingsOverlay");
const upDownButtonCcUpDisplay = document.getElementById("upDownButtonCcUpDisplay");
const upDownButtonLearnUpBtn = document.getElementById("upDownButtonLearnUpBtn");
const upDownButtonCcDownDisplay = document.getElementById("upDownButtonCcDownDisplay");
const upDownButtonLearnDownBtn = document.getElementById("upDownButtonLearnDownBtn");
const upDownButtonValueListInput = document.getElementById("upDownButtonValueListInput");
const upDownButtonMinInput = document.getElementById("upDownButtonMinInput");
const upDownButtonMaxInput = document.getElementById("upDownButtonMaxInput");
const upDownButtonStepInput = document.getElementById("upDownButtonStepInput");
let pendingUpDownButtonId = null;

function openUpDownButtonSettings(widgetId) {
  const state = upDownButtonStates.get(widgetId);
  if (!state) return;
  pendingUpDownButtonId = widgetId;
  upDownButtonCcUpDisplay.value =
    state.ccUp === null ? "" : `${state.ccUp}${state.channelUp === null ? "" : ` (Ch ${state.channelUp + 1})`}`;
  upDownButtonCcDownDisplay.value =
    state.ccDown === null ? "" : `${state.ccDown}${state.channelDown === null ? "" : ` (Ch ${state.channelDown + 1})`}`;
  upDownButtonMinInput.value = state.min;
  upDownButtonMaxInput.value = state.max;
  upDownButtonStepInput.value = state.step;
  const upDownList = parseVoltageValueList(state.listText);
  upDownButtonValueListInput.value = upDownList ? upDownList.join(", ") : (state.listText || "");
  upDownButtonLearnUpBtn.textContent = "Learn";
  upDownButtonLearnDownBtn.textContent = "Learn";
  upDownMidiLearnTargetIdUp = null;
  upDownMidiLearnTargetIdDown = null;
  upDownButtonSettingsOverlay.classList.remove("hidden");
}

function closeUpDownButtonSettings() {
  upDownButtonSettingsOverlay.classList.add("hidden");
  pendingUpDownButtonId = null;
  upDownMidiLearnTargetIdUp = null;
  upDownMidiLearnTargetIdDown = null;
  upDownButtonLearnUpBtn.textContent = "Learn";
  upDownButtonLearnDownBtn.textContent = "Learn";
}

upDownButtonSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === upDownButtonSettingsOverlay) closeUpDownButtonSettings();
});

document.getElementById("upDownButtonSettingsCancel").addEventListener("click", closeUpDownButtonSettings);

upDownButtonLearnUpBtn.addEventListener("click", () => {
  if (!pendingUpDownButtonId) return;
  upDownMidiLearnTargetIdUp = pendingUpDownButtonId;
  upDownMidiLearnTargetIdDown = null;
  upDownButtonLearnUpBtn.textContent = "Waiting for CC…";
  upDownButtonLearnDownBtn.textContent = "Learn";
});

upDownButtonLearnDownBtn.addEventListener("click", () => {
  if (!pendingUpDownButtonId) return;
  upDownMidiLearnTargetIdDown = pendingUpDownButtonId;
  upDownMidiLearnTargetIdUp = null;
  upDownButtonLearnDownBtn.textContent = "Waiting for CC…";
  upDownButtonLearnUpBtn.textContent = "Learn";
});

document.getElementById("upDownButtonSettingsSave").addEventListener("click", () => {
  if (!pendingUpDownButtonId) return;
  const state = upDownButtonStates.get(pendingUpDownButtonId);
  if (state) {
    const min = parseFloatFlexible(upDownButtonMinInput.value);
    const max = parseFloatFlexible(upDownButtonMaxInput.value);
    const step = parseFloatFlexible(upDownButtonStepInput.value);
    state.min = clampVoltage(Number.isNaN(min) ? 0 : min);
    state.max = clampVoltage(Number.isNaN(max) ? CV_MAX_VOLTS : max, CV_MAX_VOLTS);
    if (state.max < state.min) [state.min, state.max] = [state.max, state.min];
    state.step = Number.isNaN(step) || step <= 0 ? 1 : clampVoltage(step, 1);
    const list = parseVoltageValueList(upDownButtonValueListInput.value);
    state.listText = list ? list.join(", ") : upDownButtonValueListInput.value;
    if (list && list.length >= 2) {
      state.listIndex = clamp(state.listIndex, 0, list.length - 1);
    }
    state.currentValue = clamp(state.currentValue, state.min, state.max);

    const output = upDownButtonCurrentOutput(state);
    audioEngine.setParam(pendingUpDownButtonId, "value", output);
    propagate(pendingUpDownButtonId, "value", output);
    updateUpDownButtonVisual(pendingUpDownButtonId);
    sendLiveParamUpdate(pendingUpDownButtonId, {
      min: state.min,
      max: state.max,
      step: state.step,
      valueList: list || [],
    });
    savePresetNow(); 
  }
  closeUpDownButtonSettings();
});



const switchStates = new Map();
let switchMidiLearnTargetId = null;

function switchCurrentOutput(state) {
  return state.selectIn2 ? state.in2 : state.in1;
}

function updateSwitchVisual(widgetId) {
  const state = switchStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const toggle = el.querySelector('[data-role="switch-toggle"]');
  if (toggle) toggle.checked = state.selectIn2;
  const label = el.querySelector('[data-role="switch-select-label"]');
  if (label) label.textContent = state.selectIn2 ? "In 2" : "In 1";
  ["in1", "in2"].forEach((portId) => {
    const row = el.querySelector(`.port-row-in:has(.port-dot[data-port-id="${portId}"])`);
    if (!row) return;
    row.classList.toggle("switch-selected", (portId === "in2") === state.selectIn2);
    // No cable on this input - show the settings-dialog fallback that the
    // engine actually outputs in that case, right next to the port label
    // (e.g. "In 1 (52)"), so it's visible without opening Settings.
    const labelEl = row.querySelector(".port-label");
    if (labelEl) {
      const baseLabel = portId === "in2" ? "In 2" : "In 1";
      const connected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === portId);
      const fallback = portId === "in2" ? state.in2Default : state.in1Default;
      labelEl.textContent = connected ? baseLabel : `${baseLabel} (${formatNumber(fallback ?? 0)})`;
    }
  });
  const ccLabel = el.querySelector('[data-role="switch-cc-label"]');
  if (ccLabel) {
    ccLabel.textContent =
      state.cc === null ? "CC: –" : `CC ${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  }
}

function publishSwitchValue(widgetId) {
  const state = switchStates.get(widgetId);
  if (!state) return;
  propagate(widgetId, "value", switchCurrentOutput(state));
}

function setSwitchSelectIn2(widgetId, selectIn2) {
  const state = switchStates.get(widgetId);
  if (!state) return;
  state.selectIn2 = !!selectIn2;
  audioEngine.setParam(widgetId, "selectIn2", state.selectIn2);
  updateSwitchVisual(widgetId);
  sendLiveParamUpdate(widgetId, { selectIn2: state.selectIn2 });
  publishSwitchValue(widgetId);
}

function routeMidiToSwitches(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;

  if (switchMidiLearnTargetId) {
    const state = switchStates.get(switchMidiLearnTargetId);
    if (state) {
      state.cc = data.control;
      state.channel = data.channel;
      switchCcDisplay.value = `${data.control} (Ch ${data.channel + 1})`;
      switchLearnBtn.textContent = "Learn";
      updateSwitchVisual(switchMidiLearnTargetId);
      sendLiveParamUpdate(switchMidiLearnTargetId, { cc: data.control, channel: data.channel });
      savePresetNow(); 
    }
    switchMidiLearnTargetId = null;
    return;
  }

  
  
  
  switchStates.forEach((state, widgetId) => {
    if (state.cc === null || state.cc !== data.control || state.channel !== data.channel) return;
    setSwitchSelectIn2(widgetId, data.value >= 64);
    markDirty();
  });
}

const switchSettingsOverlay = document.getElementById("switchSettingsOverlay");
const switchCcDisplay = document.getElementById("switchCcDisplay");
const switchLearnBtn = document.getElementById("switchLearnBtn");
const switchIn1DefaultInput = document.getElementById("switchIn1DefaultInput");
const switchIn2DefaultInput = document.getElementById("switchIn2DefaultInput");
let pendingSwitchId = null;

function openSwitchSettings(widgetId) {
  const state = switchStates.get(widgetId);
  if (!state) return;
  pendingSwitchId = widgetId;
  switchCcDisplay.value = state.cc === null ? "" : `${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  switchIn1DefaultInput.value = state.in1Default ?? 0;
  switchIn2DefaultInput.value = state.in2Default ?? 0;
  switchLearnBtn.textContent = "Learn";
  switchMidiLearnTargetId = null;
  switchSettingsOverlay.classList.remove("hidden");
}

function closeSwitchSettings() {
  switchSettingsOverlay.classList.add("hidden");
  pendingSwitchId = null;
  switchMidiLearnTargetId = null;
  switchLearnBtn.textContent = "Learn";
}

switchSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === switchSettingsOverlay) closeSwitchSettings();
});

document.getElementById("switchSettingsCancel").addEventListener("click", closeSwitchSettings);

switchLearnBtn.addEventListener("click", () => {
  if (!pendingSwitchId) return;
  switchMidiLearnTargetId = pendingSwitchId;
  switchLearnBtn.textContent = "Waiting for CC…";
});

document.getElementById("switchSettingsSave").addEventListener("click", () => {
  if (pendingSwitchId) {
    const state = switchStates.get(pendingSwitchId);
    if (state) {
      const in1Default = parseFloatFlexible(switchIn1DefaultInput.value);
      const in2Default = parseFloatFlexible(switchIn2DefaultInput.value);
      state.in1Default = Number.isNaN(in1Default) ? 0 : in1Default;
      state.in2Default = Number.isNaN(in2Default) ? 0 : in2Default;
      audioEngine.setParam(pendingSwitchId, "in1Default", state.in1Default);
      audioEngine.setParam(pendingSwitchId, "in2Default", state.in2Default);
      updateSwitchVisual(pendingSwitchId);
      sendLiveParamUpdate(pendingSwitchId, { in1Default: state.in1Default, in2Default: state.in2Default });
    }
  }
  closeSwitchSettings();
});



const midiInStates = new Map();



const midiVoiceSlotStates = new Map();
const MIDI_IN_MIN_VOICES = 1;
const MIDI_IN_MAX_VOICES = 16;
const midiInSettingsOverlay = document.getElementById("midiInSettingsOverlay");
const midiInChannelSelect = document.getElementById("midiInChannelSelect");
const midiInOverflowModeSelect = document.getElementById("midiInOverflowModeSelect");
let pendingMidiInId = null;

function midiInVoiceLimit(widgetId) {
  return isFxMidiIn(widgetId) ? 1 : MIDI_IN_MAX_VOICES;
}

function isFxMidiIn(widgetId) {
  const el = document.getElementById(widgetId);
  return el
    ? pages.fx.canvasContentEl.contains(el)
    : canvasContent === pages.fx.canvasContentEl;
}

const omniOption = document.createElement("option");
omniOption.value = "omni";
omniOption.textContent = "Omni (all channels)";
midiInChannelSelect.appendChild(omniOption);
for (let ch = 0; ch < 16; ch += 1) {
  const opt = document.createElement("option");
  opt.value = String(ch);
  opt.textContent = String(ch + 1);
  midiInChannelSelect.appendChild(opt);
}





function resizeVoiceSlots(midiInId, voiceCount) {
  const slotState = midiVoiceSlotStates.get(midiInId);
  if (!slotState) return;
  if (voiceCount === slotState.slots.length) return;
  if (voiceCount < slotState.slots.length) {
    slotState.slots.length = voiceCount;
  } else {
    while (slotState.slots.length < voiceCount) {
      slotState.slots.push({ note: null, heldSince: 0 });
    }
  }
}

function updateMidiInControls(widgetId) {
  const state = midiInStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const voiceLimit = midiInVoiceLimit(widgetId);
  state.voices = clamp(Math.round(state.voices), MIDI_IN_MIN_VOICES, voiceLimit);
  if (isFxMidiIn(widgetId)) state.overflowMode = "ignore";
  const mono = state.voices === 1;
  const voicesSlider = el.querySelector('[data-role="midi-in-voices-slider"]');
  const voicesValue = el.querySelector('[data-role="midi-in-voices-value"]');
  if (voicesSlider) {
    voicesSlider.max = String(voiceLimit);
    voicesSlider.value = String(state.voices);
  }
  if (voicesValue) voicesValue.textContent = String(state.voices);
  const glideToggle = el.querySelector('[data-role="midi-in-glide-switch"]');
  const glideTimeSlider = el.querySelector('[data-role="midi-in-glide-time-slider"]');
  const glideTimeValue = el.querySelector('[data-role="midi-in-glide-time-value"]');
  if (glideToggle) {
    glideToggle.checked = state.glide;
    glideToggle.disabled = !mono;
  }
  if (glideTimeSlider) {
    glideTimeSlider.value = String(state.glideTime);
    glideTimeSlider.disabled = !mono;
  }
  if (glideTimeValue) glideTimeValue.textContent = `${formatNumber(state.glideTime)} s`;
  [glideToggle?.closest(".port-row"), glideTimeSlider?.closest(".port-row")].forEach((row) => {
    if (row) row.classList.toggle("disabled", !mono);
  });
}

function setMidiInVoices(widgetId, voices) {
  const state = midiInStates.get(widgetId);
  if (!state) return;
  state.voices = clamp(Math.round(voices), MIDI_IN_MIN_VOICES, midiInVoiceLimit(widgetId));
  resizeVoiceSlots(widgetId, state.voices);
  updateMidiInControls(widgetId);
  refreshVoiceGroups();
  savePresetNow();
}

function setMidiInGlide(widgetId, glide) {
  const state = midiInStates.get(widgetId);
  if (!state) return;
  state.glide = glide;
  updateMidiInControls(widgetId);
  sendLiveParamUpdate(widgetId, { glide: state.glide });
}

function setMidiInGlideTime(widgetId, seconds) {
  const state = midiInStates.get(widgetId);
  if (!state) return;
  state.glideTime = clamp(seconds, 0, 2);
  updateMidiInControls(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { glideTime: state.glideTime });
}

function openMidiInSettings(widgetId) {
  const state = midiInStates.get(widgetId);
  if (!state) return;
  pendingMidiInId = widgetId;
  midiInChannelSelect.value = state.channel === null ? "omni" : String(state.channel);
  const fxMidiIn = isFxMidiIn(widgetId);
  if (fxMidiIn) state.overflowMode = "ignore";
  midiInOverflowModeSelect.value = state.overflowMode;
  midiInOverflowModeSelect.disabled = fxMidiIn;
  midiInOverflowModeSelect.closest(".modal-field")?.classList.toggle("disabled", fxMidiIn);
  midiInSettingsOverlay.classList.remove("hidden");
}

function closeMidiInSettings() {
  midiInSettingsOverlay.classList.add("hidden");
  pendingMidiInId = null;
}

midiInSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === midiInSettingsOverlay) closeMidiInSettings();
});

document.getElementById("midiInSettingsCancel").addEventListener("click", closeMidiInSettings);

document.getElementById("midiInSettingsSave").addEventListener("click", () => {
  if (!pendingMidiInId) return;
  const state = midiInStates.get(pendingMidiInId);
  if (state) {
    const value = midiInChannelSelect.value;
    state.channel = value === "omni" ? null : parseInt(value, 10);
    state.overflowMode = isFxMidiIn(pendingMidiInId) || midiInOverflowModeSelect.value === "ignore"
      ? "ignore"
      : "steal-oldest";
    savePresetNow();
  }
  closeMidiInSettings();
});












function handleVoicesCapped() {}



function createPianoKeyboard({ minNote = 21, maxNote = 108, zones = [], onRangeSelect }) {
  const el = document.createElement("div");
  el.className = "piano-keyboard";

  const isBlackKey = (note) => [1, 3, 6, 8, 10].includes(note % 12);
  const whiteNotes = [];
  for (let n = minNote; n <= maxNote; n += 1) {
    if (!isBlackKey(n)) whiteNotes.push(n);
  }
  const keyWidthPct = 100 / whiteNotes.length;
  const keyElements = new Map(); 

  function zoneFor(note) {
    return zones.find((z) => note >= z.from && note <= z.to) || null;
  }

  function clearPreview() {
    keyElements.forEach((key) => key.classList.remove("piano-key-preview"));
  }

  function showPreview(startNote, endNote) {
    clearPreview();
    const lo = Math.min(startNote, endNote);
    const hi = Math.max(startNote, endNote);
    keyElements.forEach((key, note) => {
      if (note >= lo && note <= hi) key.classList.add("piano-key-preview");
    });
  }

  function makeKey(note, black) {
    const key = document.createElement("div");
    key.className = black ? "piano-key piano-key-black" : "piano-key piano-key-white";
    key.dataset.note = String(note);
    const zone = zoneFor(note);
    if (zone) {
      key.style.backgroundColor = zone.color;
      
      
      key.style.opacity = zone.active === false ? "0.35" : "1";
    }

    if (!black && note % 12 === 0) {
      const label = document.createElement("span");
      label.className = "piano-key-label";
      label.textContent = `C${Math.floor(note / 12) - 1}`;
      key.appendChild(label);
    }

    key.addEventListener("mousedown", (e) => {
      e.preventDefault();
      const startNote = note;
      showPreview(startNote, startNote);

      const onMove = (moveEvent) => {
        const target = moveEvent.target.closest(".piano-key");
        if (!target) return;
        showPreview(startNote, parseInt(target.dataset.note, 10));
      };
      const onUp = (upEvent) => {
        document.removeEventListener("mousemove", onMove);
        document.removeEventListener("mouseup", onUp);
        clearPreview();
        const target = upEvent.target.closest(".piano-key");
        const endNote = target ? parseInt(target.dataset.note, 10) : startNote;
        if (onRangeSelect) onRangeSelect(Math.min(startNote, endNote), Math.max(startNote, endNote));
      };
      document.addEventListener("mousemove", onMove);
      document.addEventListener("mouseup", onUp);
    });

    keyElements.set(note, key);
    return key;
  }

  let whiteIdx = 0;
  for (let n = minNote; n <= maxNote; n += 1) {
    if (isBlackKey(n)) continue;
    const key = makeKey(n, false);
    key.style.left = `${whiteIdx * keyWidthPct}%`;
    key.style.width = `${keyWidthPct}%`;
    el.appendChild(key);
    whiteIdx += 1;
  }
  whiteIdx = 0;
  for (let n = minNote; n <= maxNote; n += 1) {
    if (isBlackKey(n)) {
      const key = makeKey(n, true);
      key.style.left = `${whiteIdx * keyWidthPct - keyWidthPct * 0.3}%`;
      key.style.width = `${keyWidthPct * 0.6}%`;
      el.appendChild(key);
    } else {
      whiteIdx += 1;
    }
  }

  return el;
}



const MIDI_SPLIT_ZONE_COLORS = ["#f87171", "#60a5fa", "#4ade80", "#facc15"];

const midiSplitSettingsOverlay = document.getElementById("midiSplitSettingsOverlay");
const midiSplitZoneChips = document.getElementById("midiSplitZoneChips");
const midiSplitKeyboardContainer = document.getElementById("midiSplitKeyboardContainer");
const midiSplitAddZoneButton = document.getElementById("midiSplitAddZoneButton");
let pendingMidiSplitId = null;
let midiSplitEditState = null;
let midiSplitActiveZone = 0;

function renderMidiSplitEditor() {
  midiSplitAddZoneButton.disabled = midiSplitEditState.zoneCount >= MIDI_SPLIT_MAX_ZONES;

  midiSplitZoneChips.innerHTML = "";
  for (let z = 0; z < midiSplitEditState.zoneCount; z += 1) {
    const chip = document.createElement("div");
    chip.className = "midi-split-zone-chip" + (z === midiSplitActiveZone ? " active" : "");
    chip.style.borderColor = MIDI_SPLIT_ZONE_COLORS[z];
    if (z === midiSplitActiveZone) chip.style.backgroundColor = MIDI_SPLIT_ZONE_COLORS[z];

    const label = document.createElement("span");
    label.textContent = `Zone ${z + 1}`;
    chip.appendChild(label);

    const octaveInput = document.createElement("input");
    octaveInput.type = "number";
    octaveInput.min = "-4";
    octaveInput.max = "4";
    octaveInput.value = String(midiSplitEditState.zones[z].octave);
    octaveInput.className = "midi-split-zone-octave";
    octaveInput.title = "Octave Transpose";
    octaveInput.addEventListener("mousedown", (e) => e.stopPropagation());
    octaveInput.addEventListener("click", (e) => e.stopPropagation());
    octaveInput.addEventListener("change", () => {
      midiSplitEditState.zones[z].octave = clamp(parseInt(octaveInput.value, 10) || 0, -4, 4);
    });
    chip.appendChild(octaveInput);

    if (midiSplitEditState.zoneCount > 0) {
      const deleteBtn = document.createElement("button");
      deleteBtn.type = "button";
      deleteBtn.className = "midi-split-zone-delete";
      deleteBtn.textContent = "🗑";
      deleteBtn.title = "Delete zone";
      deleteBtn.addEventListener("mousedown", (e) => e.stopPropagation());
      deleteBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        midiSplitEditState.zones.splice(z, 1);
        midiSplitEditState.zones.push({ from: 0, to: 127, octave: 0 }); 
        midiSplitEditState.zoneCount -= 1;
        if (midiSplitActiveZone >= midiSplitEditState.zoneCount) {
          midiSplitActiveZone = midiSplitEditState.zoneCount - 1;
        }
        renderMidiSplitEditor();
      });
      chip.appendChild(deleteBtn);
    }

    chip.addEventListener("click", () => {
      midiSplitActiveZone = z;
      renderMidiSplitEditor();
    });

    midiSplitZoneChips.appendChild(chip);
  }

  midiSplitKeyboardContainer.innerHTML = "";
  const zonesForKeyboard = midiSplitEditState.zones.slice(0, midiSplitEditState.zoneCount).map((z, i) => ({
    from: z.from,
    to: z.to,
    color: MIDI_SPLIT_ZONE_COLORS[i],
    active: i === midiSplitActiveZone,
  }));
  const keyboard = createPianoKeyboard({
    zones: zonesForKeyboard,
    onRangeSelect: (from, to) => {
      midiSplitEditState.zones[midiSplitActiveZone].from = from;
      midiSplitEditState.zones[midiSplitActiveZone].to = to;
      renderMidiSplitEditor();
    },
  });
  midiSplitKeyboardContainer.appendChild(keyboard);
}

function openMidiSplitSettings(widgetId) {
  const state = midiSplitStates.get(widgetId);
  if (!state) return;
  pendingMidiSplitId = widgetId;
  midiSplitEditState = { zoneCount: state.zoneCount, zones: state.zones.map((z) => ({ ...z })) };
  midiSplitActiveZone = 0;
  renderMidiSplitEditor();
  midiSplitSettingsOverlay.classList.remove("hidden");
}

function closeMidiSplitSettings() {
  midiSplitSettingsOverlay.classList.add("hidden");
  pendingMidiSplitId = null;
  midiSplitEditState = null;
}

midiSplitSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === midiSplitSettingsOverlay) closeMidiSplitSettings();
});

document.getElementById("midiSplitSettingsCancel").addEventListener("click", closeMidiSplitSettings);

midiSplitAddZoneButton.addEventListener("click", () => {
  if (!midiSplitEditState || midiSplitEditState.zoneCount >= MIDI_SPLIT_MAX_ZONES) return;
  
  
  
  midiSplitEditState.zoneCount += 1;
  midiSplitActiveZone = midiSplitEditState.zoneCount - 1;
  renderMidiSplitEditor();
});

document.getElementById("midiSplitSettingsSave").addEventListener("click", () => {
  if (!pendingMidiSplitId || !midiSplitEditState) {
    closeMidiSplitSettings();
    return;
  }
  const state = midiSplitStates.get(pendingMidiSplitId);
  if (state) {
    const oldCount = state.zoneCount;
    const newCount = midiSplitEditState.zoneCount;

    if (newCount < oldCount) {
      for (let z = newCount + 1; z <= oldCount; z += 1) {
        removeConnectionsForPort(pendingMidiSplitId, `zone${z}Midi`, "out");
        removeConnectionsForPort(pendingMidiSplitId, `zone${z}Pitch`, "out");
        removeConnectionsForPort(pendingMidiSplitId, `zone${z}Note`, "out");
        removeConnectionsForPort(pendingMidiSplitId, `zone${z}Velocity`, "out");
        removeConnectionsForPort(pendingMidiSplitId, `zone${z}Gate`, "out");
      }
    }

    state.zoneCount = newCount;
    state.zones = midiSplitEditState.zones.map((z) => ({ ...z }));

    const el = document.getElementById(pendingMidiSplitId);
    const portsEl = el ? el.querySelector(".widget-ports") : null;
    if (portsEl) renderMidiSplitPorts(pendingMidiSplitId, portsEl);

    drawConnections();
    savePresetNow(); 
  }
  closeMidiSplitSettings();
});



const fileExplorerOverlay = document.getElementById("fileExplorerOverlay");
const fileExplorerBreadcrumb = document.getElementById("fileExplorerBreadcrumb");
const fileExplorerSpace = document.getElementById("fileExplorerSpace");
const fileExplorerDropzone = document.getElementById("fileExplorerDropzone");
const fileExplorerList = document.getElementById("fileExplorerList");
const fileExplorerUploadInput = document.getElementById("fileExplorerUploadInput");
const fileExplorerAudioPreview = document.getElementById("fileExplorerAudioPreview");
const fileExplorerFlashVolume = document.getElementById("fileExplorerFlashVolume");
const fileExplorerSdVolume = document.getElementById("fileExplorerSdVolume");
const fileExplorerModal = document.querySelector(".file-explorer-modal");
const fileExplorerUploadProgress = document.getElementById("fileExplorerUploadProgress");
const fileExplorerUploadProgressLabel = document.getElementById("fileExplorerUploadProgressLabel");
const fileExplorerUploadProgressFill = document.getElementById("fileExplorerUploadProgressFill");

let fileExplorerDir = "";
let fileExplorerVolume = "flash";
let fileExplorerFreeBytes = 0;
let fileExplorerPlayingPath = null;










let fileExplorerRenderToken = 0;

const FILE_ICONS = { wav: "🎵", json: "📄", txt: "📄", log: "📄" };

function fileIconFor(name, isDir) {
  if (isDir) return "📁";
  const dot = name.lastIndexOf(".");
  const ext = dot >= 0 ? name.slice(dot + 1).toLowerCase() : "";
  return FILE_ICONS[ext] || "📦";
}

function humanFileSize(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${roundFloat(bytes / 1024, 1)} KB`;
  return `${roundFloat(bytes / (1024 * 1024), 2)} MB`;
}

function fileExplorerUrl(dir, name) {
  const base = fileExplorerVolume === "sd" ? "/sdcard" : "/storage";
  return `${base}/${dir ? `${dir}/` : ""}${encodeURIComponent(name)}`;
}

async function fetchFileList(dir) {
  try {
    const res = await fetch(`/api/files/list?volume=${fileExplorerVolume}&dir=${encodeURIComponent(dir)}`);
    if (!res.ok) return { dir, entries: [] };
    return await res.json();
  } catch (err) {
    appendLog(`File list could not be loaded: ${err.message}`);
    return { dir, entries: [] };
  }
}

async function fetchFsInfo() {
  
  
  
  
  
  const info = await requestFsInfo(fileExplorerVolume);
  if (!info) appendLog("Storage info could not be loaded");
  return info;
}

async function renderFileExplorer() {
  fileExplorerBreadcrumb.innerHTML = "";
  const rootCrumb = document.createElement("span");
  rootCrumb.className = "file-explorer-crumb";
  rootCrumb.textContent = fileExplorerVolume === "sd" ? "sdcard" : "storage";
  rootCrumb.addEventListener("click", () => {
    fileExplorerDir = "";
    renderFileExplorer();
  });
  fileExplorerBreadcrumb.appendChild(rootCrumb);

  const segments = fileExplorerDir ? fileExplorerDir.split("/") : [];
  let pathSoFar = "";
  segments.forEach((seg) => {
    pathSoFar = pathSoFar ? `${pathSoFar}/${seg}` : seg;
    const sep = document.createElement("span");
    sep.className = "file-explorer-crumb-sep";
    sep.textContent = "/";
    fileExplorerBreadcrumb.appendChild(sep);

    const crumb = document.createElement("span");
    crumb.className = "file-explorer-crumb";
    crumb.textContent = seg;
    const target = pathSoFar;
    crumb.addEventListener("click", () => {
      fileExplorerDir = target;
      renderFileExplorer();
    });
    fileExplorerBreadcrumb.appendChild(crumb);
  });

  
  
  
  
  
  const myToken = ++fileExplorerRenderToken;
  fileExplorerList.innerHTML = '<div class="file-explorer-loading">⏳ Lädt…</div>';

  const [listData, info] = await Promise.all([fetchFileList(fileExplorerDir), fetchFsInfo()]);
  if (myToken !== fileExplorerRenderToken) return;

  if (info?.available) {
    fileExplorerFreeBytes = info.freeBytes;
    fileExplorerSpace.textContent = `${humanFileSize(info.usedBytes)} / ${humanFileSize(info.totalBytes)} belegt`;
  } else {
    fileExplorerFreeBytes = 0;
    fileExplorerSpace.textContent = fileExplorerVolume === "sd" ? "Keine SD-Karte verfügbar" : "Nicht verfügbar";
  }

  fileExplorerList.innerHTML = "";
  const entries = (listData.entries || []).slice().sort((a, b) => {
    if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
    return a.name.localeCompare(b.name);
  });

  entries.forEach((entry) => {
    const row = document.createElement("div");
    row.className = "file-explorer-row";

    const icon = document.createElement("span");
    icon.className = "file-explorer-icon";
    icon.textContent = fileIconFor(entry.name, entry.isDir);
    row.appendChild(icon);

    const nameSpan = document.createElement("span");
    nameSpan.className = "file-explorer-name";
    nameSpan.textContent = entry.name;
    row.appendChild(nameSpan);

    const sizeSpan = document.createElement("span");
    sizeSpan.className = "file-explorer-size";
    sizeSpan.textContent = entry.isDir ? "–" : humanFileSize(entry.size);
    row.appendChild(sizeSpan);

    const actions = document.createElement("span");
    actions.className = "file-explorer-actions";

    if (entry.isDir) {
      row.classList.add("file-explorer-row-dir");
      row.addEventListener("click", () => {
        fileExplorerDir = fileExplorerDir ? `${fileExplorerDir}/${entry.name}` : entry.name;
        renderFileExplorer();
      });
    } else {
      const path = fileExplorerDir ? `${fileExplorerDir}/${entry.name}` : entry.name;
      const url = fileExplorerUrl(fileExplorerDir, entry.name);

      if (entry.name.toLowerCase().endsWith(".wav")) {
        const playBtn = document.createElement("button");
        playBtn.className = "file-explorer-action-btn";
        playBtn.textContent = fileExplorerPlayingPath === path ? "⏸" : "▶";
        playBtn.title = "Preview";
        playBtn.addEventListener("click", (e) => {
          e.stopPropagation();
          if (fileExplorerPlayingPath === path) {
            fileExplorerAudioPreview.pause();
            fileExplorerPlayingPath = null;
          } else {
            fileExplorerAudioPreview.src = url;
            fileExplorerAudioPreview.play();
            fileExplorerPlayingPath = path;
          }
          renderFileExplorer();
        });
        actions.appendChild(playBtn);
      }

      const downloadLink = document.createElement("a");
      downloadLink.className = "file-explorer-action-btn";
      downloadLink.href = url;
      downloadLink.download = entry.name;
      downloadLink.textContent = "⬇";
      downloadLink.title = "Download";
      downloadLink.addEventListener("click", (e) => e.stopPropagation());
      actions.appendChild(downloadLink);

      const renameBtn = document.createElement("button");
      renameBtn.className = "file-explorer-action-btn";
      renameBtn.textContent = "✏";
      renameBtn.title = "Rename";
      renameBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        startFileExplorerRename(nameSpan, path, entry.name);
      });
      actions.appendChild(renameBtn);

      const deleteBtn = document.createElement("button");
      deleteBtn.className = "file-explorer-action-btn";
      deleteBtn.textContent = "🗑";
      deleteBtn.title = "Delete";
      deleteBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        deleteFileExplorerFile(path, entry.name);
      });
      actions.appendChild(deleteBtn);
    }

    row.appendChild(actions);
    fileExplorerList.appendChild(row);
  });
}

function startFileExplorerRename(nameSpan, path, currentName) {
  const input = document.createElement("input");
  input.type = "text";
  input.className = "file-explorer-rename-input";
  input.value = currentName;
  nameSpan.replaceWith(input);
  input.focus();
  input.select();

  const commit = async () => {
    const newName = input.value.trim();
    if (!newName || newName === currentName) {
      renderFileExplorer();
      return;
    }
    try {
      const res = await fetch(
        `/api/files/rename?volume=${fileExplorerVolume}&path=${encodeURIComponent(path)}&newName=${encodeURIComponent(newName)}`,
        { method: "POST" }
      );
      if (!res.ok) {
        const data = await res.json().catch(() => ({}));
        showToast(`Rename failed: ${data.error || res.status}`);
      }
    } catch (err) {
      appendLog(`Rename failed: ${err.message}`);
      showToast("Rename failed");
    }
    renderFileExplorer();
  };

  input.addEventListener("blur", commit);
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") input.blur();
    if (e.key === "Escape") {
      input.value = currentName;
      input.blur();
    }
  });
}

async function deleteFileExplorerFile(path, name) {
  if (!confirm(`Delete "${name}"?`)) return;
  try {
    const res = await fetch(`/api/files/delete?volume=${fileExplorerVolume}&path=${encodeURIComponent(path)}`, { method: "POST" });
    if (!res.ok) {
      const data = await res.json().catch(() => ({}));
      showToast(`Delete failed: ${data.error || res.status}`);
    }
  } catch (err) {
    appendLog(`Delete failed: ${err.message}`);
    showToast("Delete failed");
  }
  renderFileExplorer();
}

async function uploadFileExplorerFile(file, onProgress) {
  if (fileExplorerFreeBytes && file.size > fileExplorerFreeBytes) {
    showToast(`"${file.name}" is too large for the remaining storage space`);
    return;
  }
  await new Promise((resolve) => {
    const xhr = new XMLHttpRequest();
    const url = `/api/files/upload?volume=${fileExplorerVolume}&dir=${encodeURIComponent(fileExplorerDir)}&name=${encodeURIComponent(file.name)}`;
    xhr.open("POST", url);
    xhr.upload.addEventListener("progress", (e) => {
      if (e.lengthComputable) onProgress(e.loaded / e.total);
    });
    xhr.onload = () => {
      if (xhr.status !== 200) {
        showToast(`Upload failed: "${file.name}"`);
      }
      resolve();
    };
    xhr.onerror = () => {
      showToast(`Upload failed: "${file.name}"`);
      resolve();
    };
    xhr.send(file);
  });
}

async function uploadFileExplorerFiles(files) {
  fileExplorerModal.classList.add("uploading");
  fileExplorerUploadProgress.classList.remove("hidden");
  try {
    
    
    
    for (let i = 0; i < files.length; i += 1) {
      const file = files[i];
      fileExplorerUploadProgressLabel.textContent = `Uploading "${file.name}" (${i + 1}/${files.length})…`;
      fileExplorerUploadProgressFill.style.width = "0%";
      await uploadFileExplorerFile(file, (fraction) => {
        fileExplorerUploadProgressFill.style.width = `${Math.round(fraction * 100)}%`;
      });
    }
  } finally {
    fileExplorerUploadProgress.classList.add("hidden");
    fileExplorerModal.classList.remove("uploading");
  }
  renderFileExplorer();
}

fileExplorerDropzone.addEventListener("dragover", (e) => {
  e.preventDefault();
  fileExplorerDropzone.classList.add("drag-over");
});
fileExplorerDropzone.addEventListener("dragleave", () => {
  fileExplorerDropzone.classList.remove("drag-over");
});
fileExplorerDropzone.addEventListener("drop", (e) => {
  e.preventDefault();
  fileExplorerDropzone.classList.remove("drag-over");
  if (e.dataTransfer.files.length > 0) {
    uploadFileExplorerFiles(Array.from(e.dataTransfer.files));
  }
});

document.getElementById("fileExplorerUploadButton").addEventListener("click", () => {
  fileExplorerUploadInput.value = "";
  fileExplorerUploadInput.click();
});
fileExplorerUploadInput.addEventListener("change", () => {
  if (fileExplorerUploadInput.files.length > 0) {
    uploadFileExplorerFiles(Array.from(fileExplorerUploadInput.files));
  }
});

function openFileExplorer() {
  fileExplorerDir = "";
  fileExplorerPlayingPath = null;
  fileExplorerAudioPreview.pause();
  fileExplorerOverlay.classList.remove("hidden");
  renderFileExplorer();
}

function selectFileExplorerVolume(volume) {
  fileExplorerVolume = volume;
  fileExplorerDir = "";
  fileExplorerPlayingPath = null;
  fileExplorerAudioPreview.pause();
  fileExplorerFlashVolume.classList.toggle("active", volume === "flash");
  fileExplorerSdVolume.classList.toggle("active", volume === "sd");
  renderFileExplorer();
}

fileExplorerFlashVolume.addEventListener("click", () => selectFileExplorerVolume("flash"));
fileExplorerSdVolume.addEventListener("click", () => selectFileExplorerVolume("sd"));

function closeFileExplorer() {
  fileExplorerOverlay.classList.add("hidden");
  fileExplorerAudioPreview.pause();
}

fileExplorerOverlay.addEventListener("click", (e) => {
  if (e.target === fileExplorerOverlay) closeFileExplorer();
});
document.getElementById("fileExplorerClose").addEventListener("click", closeFileExplorer);



const wavePlayerStates = new Map();
const wavePlayerSettingsOverlay = document.getElementById("wavePlayerSettingsOverlay");
const wavePlayerSampleSelect = document.getElementById("wavePlayerSampleSelect");
const wavePlayerRootNoteSelect = document.getElementById("wavePlayerRootNoteSelect");
let pendingWavePlayerId = null;





const WAVE_PLAYER_NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
for (let note = 0; note <= 127; note += 1) {
  const octave = Math.floor(note / 12) - 1;
  const opt = document.createElement("option");
  opt.value = String(note);
  opt.textContent = `${WAVE_PLAYER_NOTE_NAMES[note % 12]}${octave} (${note})`;
  wavePlayerRootNoteSelect.appendChild(opt);
}

function updateWavePlayerLabel(widgetId) {
  const state = wavePlayerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  const label = el ? el.querySelector('[data-role="wave-player-sample-label"]') : null;
  if (label) label.textContent = (state && state.sampleFile) || "No sample";
}

async function fetchSampleList() {
  try {
    const res = await fetch("/api/samples");
    if (!res.ok) return [];
    const data = await res.json();
    return Array.isArray(data.samples) ? data.samples : [];
  } catch (err) {
    appendLog(`Sample list could not be loaded: ${err.message}`);
    return [];
  }
}

async function openWavePlayerSettings(widgetId) {
  const state = wavePlayerStates.get(widgetId);
  if (!state) return;
  pendingWavePlayerId = widgetId;

  const files = await fetchSampleList();
  wavePlayerSampleSelect.innerHTML = "";
  files.forEach((f) => {
    const opt = document.createElement("option");
    opt.value = f;
    opt.textContent = f;
    wavePlayerSampleSelect.appendChild(opt);
  });
  if (state.sampleFile && !files.includes(state.sampleFile)) {
    const opt = document.createElement("option");
    opt.value = state.sampleFile;
    opt.textContent = `${state.sampleFile} (missing)`;
    wavePlayerSampleSelect.appendChild(opt);
  }
  wavePlayerSampleSelect.value = state.sampleFile || "";
  wavePlayerRootNoteSelect.value = String(state.rootNoteMidi);
  wavePlayerSettingsOverlay.classList.remove("hidden");
}

function closeWavePlayerSettings() {
  wavePlayerSettingsOverlay.classList.add("hidden");
  pendingWavePlayerId = null;
}

wavePlayerSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === wavePlayerSettingsOverlay) closeWavePlayerSettings();
});

document.getElementById("wavePlayerSettingsCancel").addEventListener("click", closeWavePlayerSettings);

document.getElementById("wavePlayerSettingsSave").addEventListener("click", () => {
  if (!pendingWavePlayerId) return;
  const state = wavePlayerStates.get(pendingWavePlayerId);
  if (state) {
    const newSampleFile = wavePlayerSampleSelect.value;
    if (newSampleFile !== state.sampleFile) {
      state.sampleFile = newSampleFile;
      audioEngine.setParam(pendingWavePlayerId, "sampleFile", state.sampleFile);
    }
    state.rootNoteMidi = parseInt(wavePlayerRootNoteSelect.value, 10) || 60;
    audioEngine.setParam(pendingWavePlayerId, "rootNoteMidi", state.rootNoteMidi);
    updateWavePlayerLabel(pendingWavePlayerId);
    
    
    
    
    savePresetNow();
  }
  closeWavePlayerSettings();
});


const soundFontPlayerStates = new Map();
const soundFontPlayerSettingsOverlay = document.getElementById("soundFontPlayerSettingsOverlay");
const soundFontPlayerFileSelect = document.getElementById("soundFontPlayerFileSelect");
const soundFontPlayerVoicesInput = document.getElementById("soundFontPlayerVoicesInput");
const soundFontPlayerPresetDisplay = document.getElementById("soundFontPlayerPresetDisplay");
let pendingSoundFontPlayerId = null;
let soundFontModalPreviewRequestId = 0;

async function fetchSoundFonts() {
  try {
    const res = await fetch("/api/soundfonts");
    if (!res.ok) return [];
    const data = await res.json();
    return Array.isArray(data.soundfonts) ? data.soundfonts : [];
  } catch (err) {
    appendLog(`SoundFont list could not be loaded: ${err.message}`);
    return [];
  }
}

function soundFontKey(item) { return `${item.volume}:${item.path}`; }

function updateSoundFontPlayerLabel(widgetId) {
  const state = soundFontPlayerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  const fileLabel = el?.querySelector('[data-role="soundfont-player-file-label"]');
  const presetLabel = el?.querySelector('[data-role="soundfont-player-preset-label"]');
  if (!fileLabel || !presetLabel) return;
  fileLabel.textContent = state?.path ? state.path.split("/").pop() : "No SoundFont";
  presetLabel.textContent = state?.path ? (state.presetName || "") : "";
}

function updateSoundFontPlayerControls(widgetId) {
  const state = soundFontPlayerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  ["bank", "program"].forEach((name) => {
    const slider = el.querySelector(`[data-role="soundfont-${name}-slider"]`);
    if (slider) {
      if (name === "bank") slider.max = String(state.maxBank ?? SOUNDFONT_DEFAULT_MAX_BANK);
      slider.value = String(state[name]);
    }
    const value = el.querySelector(`[data-role="soundfont-${name}-value"]`);
    if (value) value.textContent = String(state[name]);
  });
  ["enableCc", "enableModWheel", "enablePitchWheel"].forEach((name) => {
    const toggle = el.querySelector(`[data-role="soundfont-${name}-switch"]`);
    if (toggle) toggle.checked = state[name] === true;
  });
  const gainSlider = el.querySelector('[data-role="soundfont-gain-slider"]');
  if (gainSlider) gainSlider.value = String(state.gain ?? CV_MAX_VOLTS);
  const gainValue = el.querySelector('[data-role="soundfont-gain-value"]');
  if (gainValue) gainValue.textContent = formatNumber(state.gain ?? CV_MAX_VOLTS);
  updateSoundFontPlayerLabel(widgetId);
}





async function refreshSoundFontPlayerMeta(widgetId) {
  const state = soundFontPlayerStates.get(widgetId);
  if (!state) return;
  if (!state.path) {
    state.maxBank = SOUNDFONT_DEFAULT_MAX_BANK;
    state.presetName = null;
    updateSoundFontPlayerLabel(widgetId);
    updateSoundFontPlayerControls(widgetId);
    return;
  }
  const info = await audioEngine.getSf2Info(state.volume, state.path);
  if (soundFontPlayerStates.get(widgetId) !== state) return; 
  state.maxBank = info && info.presets.length ? getMaxBank(info.presets) : SOUNDFONT_DEFAULT_MAX_BANK;
  state.bank = clamp(state.bank, 0, state.maxBank);
  state.presetName = info ? getPresetName(info.presets, state.bank, state.program) : null;
  updateSoundFontPlayerLabel(widgetId);
  updateSoundFontPlayerControls(widgetId);
}





async function refreshSoundFontModalPreview() {
  const state = soundFontPlayerStates.get(pendingSoundFontPlayerId);
  const volume = soundFontPlayerFileSelect.selectedOptions[0]?.dataset.volume || "";
  const path = soundFontPlayerFileSelect.selectedOptions[0]?.dataset.path || "";
  const requestId = (soundFontModalPreviewRequestId += 1);
  if (!path || !state) {
    soundFontPlayerPresetDisplay.textContent = "–";
    return;
  }
  const info = await audioEngine.getSf2Info(volume, path);
  if (requestId !== soundFontModalPreviewRequestId) return; 
  const maxBank = info && info.presets.length ? getMaxBank(info.presets) : SOUNDFONT_DEFAULT_MAX_BANK;
  const bank = clamp(state.bank, 0, maxBank);
  const name = info ? getPresetName(info.presets, bank, state.program) : null;
  soundFontPlayerPresetDisplay.textContent = name || (info ? "(not found)" : "(SoundFont could not be read)");
}

async function openSoundFontPlayerSettings(widgetId) {
  const state = soundFontPlayerStates.get(widgetId);
  if (!state) return;
  pendingSoundFontPlayerId = widgetId;
  const files = await fetchSoundFonts();
  soundFontPlayerFileSelect.innerHTML = '<option value="">No SoundFont</option>';
  files.forEach((file) => {
    const opt = document.createElement("option");
    opt.value = soundFontKey(file);
    opt.textContent = `${file.volume === "sd" ? "SD" : "Flash"}: ${file.name}`;
    opt.dataset.volume = file.volume;
    opt.dataset.path = file.path;
    soundFontPlayerFileSelect.appendChild(opt);
  });
  const currentKey = state.path ? soundFontKey(state) : "";
  if (currentKey && !files.some((file) => soundFontKey(file) === currentKey)) {
    const missing = document.createElement("option");
    missing.value = currentKey;
    missing.textContent = `${state.volume}: ${state.path} (missing)`;
    missing.dataset.volume = state.volume;
    missing.dataset.path = state.path;
    soundFontPlayerFileSelect.appendChild(missing);
  }
  soundFontPlayerFileSelect.value = currentKey;
  soundFontPlayerVoicesInput.value = String(state.maxVoices);
  soundFontPlayerSettingsOverlay.classList.remove("hidden");
  refreshSoundFontModalPreview();
}

function closeSoundFontPlayerSettings() {
  soundFontPlayerSettingsOverlay.classList.add("hidden");
  pendingSoundFontPlayerId = null;
}

soundFontPlayerSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === soundFontPlayerSettingsOverlay) closeSoundFontPlayerSettings();
});
document.getElementById("soundFontPlayerSettingsCancel").addEventListener("click", closeSoundFontPlayerSettings);
soundFontPlayerFileSelect.addEventListener("change", refreshSoundFontModalPreview);
document.getElementById("soundFontPlayerSettingsSave").addEventListener("click", () => {
  const id = pendingSoundFontPlayerId;
  const state = soundFontPlayerStates.get(id);
  if (!state) return;
  const selected = soundFontPlayerFileSelect.selectedOptions[0];
  state.volume = selected?.dataset.volume || "flash";
  state.path = selected?.dataset.path || "";
  state.maxVoices = clamp(parseInt(soundFontPlayerVoicesInput.value, 10) || 32, 4, 128);
  audioEngine.setSoundFontParams(id, { volume: state.volume, path: state.path, maxVoices: state.maxVoices });
  refreshSoundFontPlayerMeta(id);
  savePresetNow();
  closeSoundFontPlayerSettings();
});


const soundFontPresetPickerOverlay = document.getElementById("soundFontPresetPickerOverlay");
const soundFontPresetPickerList = document.getElementById("soundFontPresetPickerList");
const soundFontPresetPickerFilter = document.getElementById("soundFontPresetPickerFilter");
let pendingSoundFontPresetPickerId = null;
let soundFontPresetPickerPresets = [];

function renderSoundFontPresetPickerList() {
  const state = soundFontPlayerStates.get(pendingSoundFontPresetPickerId);
  soundFontPresetPickerList.innerHTML = "";
  const filterText = soundFontPresetPickerFilter.value.trim().toLowerCase();
  const byBank = new Map();
  soundFontPresetPickerPresets.forEach((p) => {
    if (filterText) {
      const haystack = `${p.bank} ${p.program} ${p.name}`.toLowerCase();
      if (!haystack.includes(filterText)) return;
    }
    if (!byBank.has(p.bank)) byBank.set(p.bank, []);
    byBank.get(p.bank).push(p);
  });
  const banks = [...byBank.keys()].sort((a, b) => a - b);
  if (banks.length === 0) {
    const empty = document.createElement("div");
    empty.className = "soundfont-preset-picker-empty";
    empty.textContent = soundFontPresetPickerPresets.length ? "No matching presets" : "No presets in this SoundFont";
    soundFontPresetPickerList.appendChild(empty);
    return;
  }
  banks.forEach((bank) => {
    const heading = document.createElement("div");
    heading.className = "soundfont-preset-picker-bank";
    heading.textContent = `Bank ${bank}`;
    soundFontPresetPickerList.appendChild(heading);

    byBank.get(bank).sort((a, b) => a.program - b.program).forEach((p) => {
      const item = document.createElement("div");
      item.className = "soundfont-preset-picker-item";
      if (state && state.bank === p.bank && state.program === p.program) item.classList.add("active");
      const programSpan = document.createElement("span");
      programSpan.className = "soundfont-preset-picker-item-program";
      programSpan.textContent = String(p.program);
      const nameSpan = document.createElement("span");
      nameSpan.textContent = p.name;
      item.appendChild(programSpan);
      item.appendChild(nameSpan);
      item.addEventListener("click", () => selectSoundFontPreset(p.bank, p.program));
      soundFontPresetPickerList.appendChild(item);
    });
  });
}

function selectSoundFontPreset(bank, program) {
  const widgetId = pendingSoundFontPresetPickerId;
  if (!widgetId) return;
  deliverToWidget(widgetId, "bank", bank);
  deliverToWidget(widgetId, "program", program);
  closeSoundFontPresetPicker();
}

async function openSoundFontPresetPicker(widgetId) {
  const state = soundFontPlayerStates.get(widgetId);
  if (!state) return;
  if (!state.path) {
    showToast("Load a SoundFont first");
    return;
  }
  pendingSoundFontPresetPickerId = widgetId;
  soundFontPresetPickerPresets = [];
  soundFontPresetPickerList.innerHTML = '<div class="soundfont-preset-picker-empty">Loading…</div>';
  soundFontPresetPickerFilter.value = "";
  soundFontPresetPickerOverlay.classList.remove("hidden");
  const info = await audioEngine.getSf2Info(state.volume, state.path);
  if (pendingSoundFontPresetPickerId !== widgetId) return; 
  soundFontPresetPickerPresets = info ? info.presets : [];
  renderSoundFontPresetPickerList();
  soundFontPresetPickerFilter.focus();
}

function closeSoundFontPresetPicker() {
  soundFontPresetPickerOverlay.classList.add("hidden");
  pendingSoundFontPresetPickerId = null;
  soundFontPresetPickerPresets = [];
}

soundFontPresetPickerOverlay.addEventListener("click", (e) => {
  if (e.target === soundFontPresetPickerOverlay) closeSoundFontPresetPicker();
});
document.getElementById("soundFontPresetPickerCancel").addEventListener("click", closeSoundFontPresetPicker);
soundFontPresetPickerFilter.addEventListener("input", renderSoundFontPresetPickerList);
















function updateDualModeAudioNode(widgetId, type, state, inputPortIds, params) {
  const wantAudio = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      inputPortIds.includes(c.to.portId) &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );

  if (wantAudio && !state.audioMode) {
    audioEngine.createNode(widgetId, type, params || {}, 1);
    state.audioMode = true;
    connections
      .filter((c) => c.from.widgetId === widgetId || c.to.widgetId === widgetId)
      .forEach((c) => {
        if (
          getPortKind(c.from.widgetId, c.from.portId, "out") === "audio" &&
          getPortKind(c.to.widgetId, c.to.portId, "in") === "audio"
        ) {
          audioEngine.connect(c.from.widgetId, c.from.portId, c.to.widgetId, c.to.portId);
        }
      });
  } else if (!wantAudio && state.audioMode) {
    audioEngine.removeNode(widgetId);
    connections
      .filter((c) => c.from.widgetId === widgetId)
      .forEach((c) => audioEngine.disconnect(widgetId, c.from.portId, c.to.widgetId, c.to.portId));
    state.audioMode = false;
  }
}



const multiplierStates = new Map();
const MULTIPLIER_MIN_INPUTS = 2;
const MULTIPLIER_MAX_INPUTS = 8;

function renderMultiplierPorts(widgetId, portsEl) {
  const state = multiplierStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  for (let i = 1; i <= state.inputCount; i += 1) {
    portsEl.appendChild(createPortRow(widgetId, { id: `in${i}`, label: `In ${i}` }, "in", "multiplier"));
  }

  const outRow = createPortRow(widgetId, { id: "value", label: "" }, "out", "multiplier");
  outRow.classList.add("multiplier-output-row");
  const readout = document.createElement("span");
  readout.className = "port-value";
  readout.dataset.role = "multiplier-readout";
  readout.textContent = formatNumber(multiplierProduct(state));
  outRow.appendChild(readout);
  portsEl.appendChild(outRow);
  layoutPortsGrid(portsEl);
}

function multiplierProduct(state) {
  let product = 1;
  for (let i = 0; i < state.inputCount; i += 1) {
    product *= state.values[i] === undefined ? 1 : state.values[i];
  }
  return product;
}

function recomputeMultiplier(widgetId) {
  const state = multiplierStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const readout = el.querySelector('[data-role="multiplier-readout"]');

  const product = multiplierProduct(state);
  if (readout) readout.textContent = formatNumber(product);
  if (!state.audioMode) propagate(widgetId, "value", product);
}



function updateMultiplierMode(widgetId) {
  const state = multiplierStates.get(widgetId);
  if (!state) return;
  const inputPortIds = [];
  for (let i = 0; i < state.inputCount; i += 1) {
    inputPortIds.push(`in${i + 1}`);
    if (!connections.some((c) => c.to.widgetId === widgetId && c.to.portId === `in${i + 1}`)) {
      state.values[i] = 1;
    }
  }

  updateDualModeAudioNode(widgetId, "multiplier", state, inputPortIds, { inputCount: state.inputCount });

  recomputeMultiplier(widgetId);
}

function refreshAllDualModeWidgets() {
  oscillatorStates.forEach((_, id) => {
    
    
    
    
    if (!connections.some((c) => c.to.widgetId === id && c.to.portId === "amplitude")) {
      audioEngine.setParam(id, "amplitude", 0, 0);
    }
    updateOscillatorGainVisual(id);
    updateOscillatorTuneVisual(id);
  });
  hammondStates.forEach((_, id) => {
    if (!connections.some((c) => c.to.widgetId === id && c.to.portId === "amplitude")) {
      audioEngine.setParam(id, "amplitude", 0, 0);
    }
    updateHammondVisual(id);
  });

  
  
  gainStates.forEach((_, id) => updateGainVisual(id));
  filterStates.forEach((_, id) => updateFilterVisual(id));
  multiplexerStates.forEach((_, id) => updateMultiplexerVisual(id));
  switchStates.forEach((_, id) => updateSwitchVisual(id));
  
  
  
  
  
  
}





function refreshControlGraph() {
  refreshAllDualModeWidgets();

  const sources = [];
  const push = (widgetId, portId, value) => sources.push({ widgetId, portId, value });
  constStates.forEach((state, id) => push(id, "value", state.value));
  knobStates.forEach((state, id) => {
    push(id, "voltage", roundFloat(mapKnobValue(state)));
  });
  buttonStates.forEach((state, id) => push(id, "value", buttonCurrentOutput(state)));
  upDownButtonStates.forEach((state, id) => push(id, "value", upDownButtonCurrentOutput(state)));
  sources.forEach(({ widgetId, portId, value }) => propagate(widgetId, portId, value));

  // Source values are replayed above. Re-apply direct control-source voltage
  // after replay so a loaded 10 V Knob immediately selects the bottom row.
  multiplexerStates.forEach((state, muxId) => {
    const connection = connections.find(
      (c) => c.to.widgetId === muxId && c.to.portId === "select"
    );
    if (!connection) return;
    const sourceEl = document.getElementById(connection.from.widgetId);
    const sourceType = sourceEl?.dataset.widgetType;
    let value = null;
    if (sourceType === "knob" && connection.from.portId === "voltage") {
      const sourceState = knobStates.get(connection.from.widgetId);
      if (sourceState) value = mapKnobValue(sourceState);
    } else if (sourceType === "const" && connection.from.portId === "value") {
      value = constStates.get(connection.from.widgetId)?.value ?? null;
    } else if (sourceType === "button" && connection.from.portId === "value") {
      const sourceState = buttonStates.get(connection.from.widgetId);
      if (sourceState) value = buttonCurrentOutput(sourceState);
    } else if (sourceType === "up-down-button" && connection.from.portId === "value") {
      const sourceState = upDownButtonStates.get(connection.from.widgetId);
      if (sourceState) value = upDownButtonCurrentOutput(sourceState);
    }
    if (Number.isFinite(value)) {
      state.cvVolts = clamp(value, 0, CV_MAX_VOLTS);
      multiplexerSetActiveIndex(muxId, multiplexerSelectFromCv(state.cvVolts, state.inputCount));
    }
  });

  const derivedTypes = new Set(["multiplier", "adder", "divide", "frequency-divider", "abs", "value-mapper", "detune"]);
  const pending = new Set(
    [...document.querySelectorAll(".widget-instance")]
      .filter((el) => derivedTypes.has(el.dataset.widgetType))
      .map((el) => el.id)
  );
  const maxPasses = pending.size + 1;
  for (let pass = 0; pass < maxPasses && pending.size > 0; pass += 1) {
    for (const id of [...pending]) {
      const incomingDerived = connections.some((c) => pending.has(c.from.widgetId) && c.to.widgetId === id);
      if (incomingDerived) continue;
      const type = document.getElementById(id)?.dataset.widgetType;
      
      
      
      
      if (type === "multiplier") updateMultiplierMode(id);
      else if (type === "adder") updateAdderMode(id);
      else if (type === "divide") updateDivideMode(id);
      else if (type === "frequency-divider") updateFrequencyDividerMode(id);
      else if (type === "abs") updateAbsMode(id);
      else if (type === "value-mapper") updateValueMapperMode(id);
      else if (type === "detune") updateDetuneMode(id);
      pending.delete(id);
    }
  }
}















function traverseVoiceSet(midiInId) {
  const visited = new Set();
  const queue = [];
  connections
    .filter((c) => c.from.widgetId === midiInId)
    .forEach((c) => queue.push(c.to.widgetId));

  while (queue.length > 0) {
    const widgetId = queue.shift();
    if (visited.has(widgetId)) continue;
    const el = document.getElementById(widgetId);
    const type = el ? el.dataset.widgetType : null;
    if (!type) continue;

    const outputs = (WIDGET_TYPES[type] && WIDGET_TYPES[type].outputs) || [];
    const isBoundary = LINK_WIDGET_TYPES.includes(type) || outputs.length === 0 || type === "soundfont-player";
    if (isBoundary) continue;

    visited.add(widgetId);
    connections
      .filter((c) => c.from.widgetId === widgetId)
      .forEach((c) => {
        if (!visited.has(c.to.widgetId)) queue.push(c.to.widgetId);
      });
  }
  return visited;
}










function refreshVoiceGroups() {
  const desiredVoiceCounts = new Map();

  midiInStates.forEach((state, midiInId) => {
    resizeVoiceSlots(midiInId, state.voices);
    if (state.voices > 1) {
      traverseVoiceSet(midiInId).forEach((widgetId) => {
        const current = desiredVoiceCounts.get(widgetId) || 1;
        desiredVoiceCounts.set(widgetId, Math.max(current, state.voices));
      });
    }
  });

  document.querySelectorAll(".widget-instance").forEach((el) => {
    const widgetId = el.id;
    const type = el.dataset.widgetType;
    if (LINK_WIDGET_TYPES.includes(type)) return;

    const currentCount = audioEngine.getVoiceCount(widgetId);
    if (currentCount === 0) return; 

    const desired = desiredVoiceCounts.get(widgetId) || 1;
    if (currentCount === desired) return;

    
    
    
    
    
    
    
    
    const engine = WIDGET_ENGINES[type];
    const params = engine ? engine.getParams(widgetId) : {};
    audioEngine.removeNode(widgetId);
    audioEngine.createNode(widgetId, type, params, desired);
    if (engine) engine.applyParams(widgetId, params);

    connections
      .filter((c) => c.from.widgetId === widgetId || c.to.widgetId === widgetId)
      .forEach((c) => {
        const fromKind = getPortKind(c.from.widgetId, c.from.portId, "out");
        const toKind = getPortKind(c.to.widgetId, c.to.portId, "in");
        if (fromKind === "audio" && toKind === "audio") {
          audioEngine.connect(c.from.widgetId, c.from.portId, c.to.widgetId, c.to.portId);
        }
      });
  });
}

const multiplierSettingsOverlay = document.getElementById("multiplierSettingsOverlay");
const multiplierCountInput = document.getElementById("multiplierCountInput");
let pendingMultiplierId = null;

function openMultiplierSettings(widgetId) {
  const state = multiplierStates.get(widgetId);
  if (!state) return;
  pendingMultiplierId = widgetId;
  multiplierCountInput.value = state.inputCount;
  multiplierSettingsOverlay.classList.remove("hidden");
}

function closeMultiplierSettings() {
  multiplierSettingsOverlay.classList.add("hidden");
  pendingMultiplierId = null;
}

multiplierSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === multiplierSettingsOverlay) closeMultiplierSettings();
});

document.getElementById("multiplierSettingsCancel").addEventListener("click", closeMultiplierSettings);

document.getElementById("multiplierSettingsSave").addEventListener("click", () => {
  if (!pendingMultiplierId) return;
  const state = multiplierStates.get(pendingMultiplierId);
  if (state) {
    let count = parseInt(multiplierCountInput.value, 10);
    if (Number.isNaN(count)) count = MULTIPLIER_MIN_INPUTS;
    count = clamp(count, MULTIPLIER_MIN_INPUTS, MULTIPLIER_MAX_INPUTS);

    if (count < state.inputCount) {
      for (let i = count + 1; i <= state.inputCount; i += 1) {
        removeConnectionsForPort(pendingMultiplierId, `in${i}`, "in");
      }
    }
    state.inputCount = count;

    const el = document.getElementById(pendingMultiplierId);
    const portsEl = el ? el.querySelector(".widget-ports") : null;
    if (portsEl) renderMultiplierPorts(pendingMultiplierId, portsEl);

    recomputeMultiplier(pendingMultiplierId);
    drawConnections();
    savePresetNow(); 
  }
  closeMultiplierSettings();
});



const adderStates = new Map();
const ADDER_MIN_INPUTS = 2;
const ADDER_MAX_INPUTS = 8;

function renderAdderPorts(widgetId, portsEl) {
  const state = adderStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  for (let i = 1; i <= state.inputCount; i += 1) {
    portsEl.appendChild(createPortRow(widgetId, { id: `in${i}`, label: `In ${i}` }, "in", "adder"));
  }

  const outRow = createPortRow(widgetId, { id: "value", label: "" }, "out", "adder");
  outRow.classList.add("adder-output-row");
  const readout = document.createElement("span");
  readout.className = "port-value";
  readout.dataset.role = "adder-readout";
  readout.textContent = formatNumber(adderSum(state));
  outRow.appendChild(readout);
  portsEl.appendChild(outRow);
  layoutPortsGrid(portsEl);
}

function adderSum(state) {
  let sum = 0;
  for (let i = 0; i < state.inputCount; i += 1) {
    sum += state.values[i] === undefined ? 0 : state.values[i];
  }
  return sum;
}

function recomputeAdder(widgetId) {
  const state = adderStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const readout = el.querySelector('[data-role="adder-readout"]');

  const sum = adderSum(state);
  if (readout) readout.textContent = formatNumber(sum);
  if (!state.audioMode) propagate(widgetId, "value", sum);
}



function updateAdderMode(widgetId) {
  const state = adderStates.get(widgetId);
  if (!state) return;
  const inputPortIds = [];
  for (let i = 0; i < state.inputCount; i += 1) {
    inputPortIds.push(`in${i + 1}`);
    if (!connections.some((c) => c.to.widgetId === widgetId && c.to.portId === `in${i + 1}`)) {
      state.values[i] = 0;
    }
  }

  updateDualModeAudioNode(widgetId, "adder", state, inputPortIds);

  recomputeAdder(widgetId);
}

const adderSettingsOverlay = document.getElementById("adderSettingsOverlay");
const adderCountInput = document.getElementById("adderCountInput");
let pendingAdderId = null;

function openAdderSettings(widgetId) {
  const state = adderStates.get(widgetId);
  if (!state) return;
  pendingAdderId = widgetId;
  adderCountInput.value = state.inputCount;
  adderSettingsOverlay.classList.remove("hidden");
}

function closeAdderSettings() {
  adderSettingsOverlay.classList.add("hidden");
  pendingAdderId = null;
}

adderSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === adderSettingsOverlay) closeAdderSettings();
});

document.getElementById("adderSettingsCancel").addEventListener("click", closeAdderSettings);

document.getElementById("adderSettingsSave").addEventListener("click", () => {
  if (!pendingAdderId) return;
  const state = adderStates.get(pendingAdderId);
  if (state) {
    let count = parseInt(adderCountInput.value, 10);
    if (Number.isNaN(count)) count = ADDER_MIN_INPUTS;
    count = clamp(count, ADDER_MIN_INPUTS, ADDER_MAX_INPUTS);

    if (count < state.inputCount) {
      for (let i = count + 1; i <= state.inputCount; i += 1) {
        removeConnectionsForPort(pendingAdderId, `in${i}`, "in");
      }
    }
    state.inputCount = count;

    const el = document.getElementById(pendingAdderId);
    const portsEl = el ? el.querySelector(".widget-ports") : null;
    if (portsEl) renderAdderPorts(pendingAdderId, portsEl);

    recomputeAdder(pendingAdderId);
    drawConnections();
    savePresetNow(); 
  }
  closeAdderSettings();
});



const divideStates = new Map();

function renderDividePorts(widgetId, portsEl) {
  const state = divideStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  portsEl.appendChild(createPortRow(widgetId, { id: "in1", label: "In 1" }, "in", "divide"));
  portsEl.appendChild(createPortRow(widgetId, { id: "in2", label: "In 2" }, "in", "divide"));

  const outRow = createPortRow(widgetId, { id: "value", label: "" }, "out", "divide");
  outRow.classList.add("divide-output-row");
  const readout = document.createElement("span");
  readout.className = "port-value";
  readout.dataset.role = "divide-readout";
  readout.textContent = formatNumber(divideQuotient(state));
  outRow.appendChild(readout);
  portsEl.appendChild(outRow);
  layoutPortsGrid(portsEl);
}

function divideQuotient(state) {
  const numerator = state.values[0] === undefined ? 0 : state.values[0];
  const denominator = state.values[1] === undefined ? 1 : state.values[1];
  if (denominator === 0) return 0;
  return numerator / denominator;
}

function recomputeDivide(widgetId) {
  const state = divideStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const readout = el.querySelector('[data-role="divide-readout"]');

  const quotient = divideQuotient(state);
  if (readout) readout.textContent = formatNumber(quotient);
  if (!state.audioMode) propagate(widgetId, "value", quotient);
}




function updateDivideMode(widgetId) {
  const state = divideStates.get(widgetId);
  if (!state) return;
  if (!connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "in1")) {
    state.values[0] = 0;
  }
  if (!connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "in2")) {
    state.values[1] = 1;
  }

  updateDualModeAudioNode(widgetId, "divide", state, ["in1", "in2"]);

  recomputeDivide(widgetId);
}

const frequencyDividerStates = new Map();
const FREQUENCY_DIVIDER_STEPS = [1, 2, 4, 8, 16];

function nearestFrequencyDivider(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return 2;
  return FREQUENCY_DIVIDER_STEPS.reduce((nearest, step) =>
    Math.abs(step - numeric) < Math.abs(nearest - numeric) ? step : nearest
  );
}

function frequencyDividerStepIndex(value) {
  return FREQUENCY_DIVIDER_STEPS.indexOf(nearestFrequencyDivider(value));
}

function recomputeFrequencyDivider(widgetId, voiceIndex) {
  const state = frequencyDividerStates.get(widgetId);
  if (!state || state.audioMode) return;
  const divider = nearestFrequencyDivider(state.divider);
  const outputCv = clamp(state.cv - Math.log2(divider), 0, CV_MAX_VOLTS);
  propagate(widgetId, "cv", outputCv, voiceIndex);
}




function updateFrequencyDividerMode(widgetId) {
  const state = frequencyDividerStates.get(widgetId);
  if (!state) return;
  state.divider = nearestFrequencyDivider(state.divider);
  updateDualModeAudioNode(widgetId, "frequency-divider", state, ["cv", "divider"], { divider: state.divider });
  recomputeFrequencyDivider(widgetId);
  updateFrequencyDividerVisual(widgetId);
}

function updateFrequencyDividerVisual(widgetId) {
  const state = frequencyDividerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const slider = el.querySelector('[data-role="frequency-divider-slider"]');
  if (slider) slider.value = String(frequencyDividerStepIndex(state.divider));
  const value = el.querySelector('[data-role="frequency-divider-value"]');
  if (value) value.textContent = String(state.divider);
}

const absStates = new Map();

function recomputeAbs(widgetId, voiceIndex) {
  const state = absStates.get(widgetId);
  if (!state) return;
  const result = Math.abs(state.value);
  const el = document.getElementById(widgetId);
  const readout = el?.querySelector('[data-role="abs-readout"]');
  if (readout) readout.textContent = formatNumber(result);
  if (!state.audioMode) propagate(widgetId, "value", result, voiceIndex);
}




function updateAbsMode(widgetId) {
  const state = absStates.get(widgetId);
  if (!state) return;
  updateDualModeAudioNode(widgetId, "abs", state, ["in"]);
  recomputeAbs(widgetId);
}


const valueMapperStates = new Map();

function mapValueMapperValue(state) {
  const list = parseValueList(state.listText);
  const { point1In, point1Out, point2In, point2Out, constraintMin, constraintMax, value } = state;

  if (list && list.length >= 2) {
    let idx = 0;
    if (point2In !== point1In) {
      const frac = clamp((value - point1In) / (point2In - point1In), 0, 1);
      idx = clamp(Math.floor(frac * list.length), 0, list.length - 1);
    }
    return list[idx];
  }

  let mapped;
  if (point2In === point1In) {
    mapped = point1Out;
  } else {
    const slope = (point2Out - point1Out) / (point2In - point1In);
    mapped = point1Out + (value - point1In) * slope;
  }

  if (constraintMin !== null && mapped < constraintMin) mapped = constraintMin;
  if (constraintMax !== null && mapped > constraintMax) mapped = constraintMax;

  return mapped;
}

function updateValueMapperVisual(widgetId) {
  const state = valueMapperStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const inValue = el.querySelector('[data-role="value-mapper-in-value"]');
  const outValue = el.querySelector('[data-role="value-mapper-out-value"]');
  const mappedValue = mapValueMapperValue(state);
  if (inValue) inValue.textContent = state.audioMode ? "Audio" : formatNumber(state.value);
  if (outValue) outValue.textContent = state.audioMode ? "Audio" : formatNumber(mappedValue);
}




function updateValueMapperMode(widgetId) {
  const state = valueMapperStates.get(widgetId);
  if (!state) return;
  updateDualModeAudioNode(widgetId, "value-mapper", state, ["in"], {
    point1In: state.point1In,
    point1Out: state.point1Out,
    point2In: state.point2In,
    point2Out: state.point2Out,
    constraintMin: state.constraintMin,
    constraintMax: state.constraintMax,
    listText: state.listText,
  });
  recomputeValueMapper(widgetId);
}

function recomputeValueMapper(widgetId, voiceIndex) {
  const state = valueMapperStates.get(widgetId);
  if (!state) return;
  if (!state.audioMode) propagate(widgetId, "out", roundFloat(mapValueMapperValue(state)), voiceIndex);
  updateValueMapperVisual(widgetId);
}



const valueMapperSettingsOverlay = document.getElementById("valueMapperSettingsOverlay");
const valueMapperPoint1InInput = document.getElementById("valueMapperPoint1InInput");
const valueMapperPoint1OutInput = document.getElementById("valueMapperPoint1OutInput");
const valueMapperPoint2InInput = document.getElementById("valueMapperPoint2InInput");
const valueMapperPoint2OutInput = document.getElementById("valueMapperPoint2OutInput");
const valueMapperConstraintMinInput = document.getElementById("valueMapperConstraintMinInput");
const valueMapperConstraintMaxInput = document.getElementById("valueMapperConstraintMaxInput");
const valueMapperValueListInput = document.getElementById("valueMapperValueListInput");
let pendingValueMapperId = null;

function openValueMapperSettings(widgetId) {
  const state = valueMapperStates.get(widgetId);
  if (!state) return;
  pendingValueMapperId = widgetId;
  valueMapperPoint1InInput.value = state.point1In;
  valueMapperPoint1OutInput.value = state.point1Out;
  valueMapperPoint2InInput.value = state.point2In;
  valueMapperPoint2OutInput.value = state.point2Out;
  valueMapperConstraintMinInput.value = state.constraintMin === null ? "" : state.constraintMin;
  valueMapperConstraintMaxInput.value = state.constraintMax === null ? "" : state.constraintMax;
  valueMapperValueListInput.value = state.listText || "";
  valueMapperSettingsOverlay.classList.remove("hidden");
}

function closeValueMapperSettings() {
  valueMapperSettingsOverlay.classList.add("hidden");
  pendingValueMapperId = null;
}

valueMapperSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === valueMapperSettingsOverlay) closeValueMapperSettings();
});

document.getElementById("valueMapperSettingsCancel").addEventListener("click", closeValueMapperSettings);

document.getElementById("valueMapperSettingsSave").addEventListener("click", () => {
  if (!pendingValueMapperId) return;
  const state = valueMapperStates.get(pendingValueMapperId);
  if (state) {
    const p1In = parseFloatFlexible(valueMapperPoint1InInput.value);
    const p1Out = parseFloatFlexible(valueMapperPoint1OutInput.value);
    const p2In = parseFloatFlexible(valueMapperPoint2InInput.value);
    const p2Out = parseFloatFlexible(valueMapperPoint2OutInput.value);

    state.point1In = Number.isNaN(p1In) ? 0 : p1In;
    state.point1Out = Number.isNaN(p1Out) ? 0 : p1Out;
    state.point2In = Number.isNaN(p2In) ? 127 : p2In;
    state.point2Out = Number.isNaN(p2Out) ? CV_MAX_VOLTS : p2Out;

    let cMin =
      valueMapperConstraintMinInput.value.trim() === "" ? null : parseFloatFlexible(valueMapperConstraintMinInput.value);
    if (cMin !== null && Number.isNaN(cMin)) cMin = null;
    let cMax =
      valueMapperConstraintMaxInput.value.trim() === "" ? null : parseFloatFlexible(valueMapperConstraintMaxInput.value);
    if (cMax !== null && Number.isNaN(cMax)) cMax = null;
    if (cMin !== null && cMax !== null && cMin > cMax) {
      const swap = cMin;
      cMin = cMax;
      cMax = swap;
    }

    state.constraintMin = cMin;
    state.constraintMax = cMax;
    state.listText = valueMapperValueListInput.value;

    
    
    
    
    if (state.audioMode) {
      audioEngine.setParam(pendingValueMapperId, "point1In", state.point1In, 0);
      audioEngine.setParam(pendingValueMapperId, "point1Out", state.point1Out, 0);
      audioEngine.setParam(pendingValueMapperId, "point2In", state.point2In, 0);
      audioEngine.setParam(pendingValueMapperId, "point2Out", state.point2Out, 0);
      audioEngine.setParam(pendingValueMapperId, "constraintMin", cMin, 0);
      audioEngine.setParam(pendingValueMapperId, "constraintMax", cMax, 0);
      audioEngine.setParam(pendingValueMapperId, "listText", state.listText, 0);
    }

    recomputeValueMapper(pendingValueMapperId);
    sendLiveParamUpdate(pendingValueMapperId, {
      point1In: state.point1In,
      point1Out: state.point1Out,
      point2In: state.point2In,
      point2Out: state.point2Out,
      constraintMin: cMin,
      constraintMax: cMax,
      valueList: parseValueList(state.listText) || [],
    });
  }
  closeValueMapperSettings();
});











const LED_MIN_VISIBLE_MS = 100;
const ledTimers = new WeakMap(); 

function setLedActive(dot, active) {
  if (!dot) return;
  const activeClass = dot.classList.contains("port-dot") ? "port-active" : "active";
  let timer = ledTimers.get(dot);
  if (!timer) {
    timer = { onSince: 0, offTimer: null };
    ledTimers.set(dot, timer);
  }
  if (active) {
    if (timer.offTimer) {
      clearTimeout(timer.offTimer);
      timer.offTimer = null;
    }
    if (!dot.classList.contains(activeClass)) {
      timer.onSince = performance.now();
      dot.classList.add(activeClass);
    }
    return;
  }
  if (!dot.classList.contains(activeClass)) return;
  const remaining = LED_MIN_VISIBLE_MS - (performance.now() - timer.onSince);
  if (remaining <= 0) {
    dot.classList.remove(activeClass);
  } else if (!timer.offTimer) {
    timer.offTimer = setTimeout(() => {
      dot.classList.remove(activeClass);
      timer.offTimer = null;
    }, remaining);
  }
}



const clockStates = new Map();

function clockBpm(cvVolts, minBpm, maxBpm) {
  return minBpm + (cvVolts / CV_MAX_VOLTS) * (maxBpm - minBpm);
}

function stopClockTimer(widgetId) {
  const state = clockStates.get(widgetId);
  if (!state) return;
  if (state.tickTimer) {
    clearInterval(state.tickTimer);
    state.tickTimer = null;
  }
  if (state.offTimer) {
    clearTimeout(state.offTimer);
    state.offTimer = null;
  }
}

function updateClockVisual(widgetId) {
  const state = clockStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const bpmLabel = el.querySelector('[data-role="clock-bpm-label"]');
  if (bpmLabel) bpmLabel.textContent = `${formatNumber(state.bpm)} BPM`;
  const slider = el.querySelector('[data-role="clock-cv-slider"]');
  if (slider) slider.value = String(state.cvVolts);
  const cvLabel = el.querySelector('[data-role="clock-cv-value"]');
  if (cvLabel) cvLabel.textContent = `${formatNumber(state.cvVolts)} V`;
}

function fireClockPulse(widgetId) {
  const state = clockStates.get(widgetId);
  if (!state) return;

  propagate(widgetId, "pulse", GATE_ON_VOLTS);
  const el = document.getElementById(widgetId);
  const dot = el ? el.querySelector('[data-role="clock-pulse-dot"]') : null;
  setLedActive(dot, true);

  if (state.offTimer) clearTimeout(state.offTimer);
  state.offTimer = setTimeout(() => {
    propagate(widgetId, "pulse", GATE_OFF_VOLTS);
    setLedActive(dot, false);
    state.offTimer = null;
  }, state.pulseMs);
}

function restartClockTimer(widgetId) {
  const state = clockStates.get(widgetId);
  if (!state) return;

  stopClockTimer(widgetId);
  updateClockVisual(widgetId);
  if (state.bpm <= 0) return;

  const intervalMs = 60000 / state.bpm;
  state.tickTimer = setInterval(() => fireClockPulse(widgetId), intervalMs);
}



const lfoStates = new Map();
const LFO_TICK_MS = 30;
const LFO_MAX_RATE_HZ = 50;
const lfoSettingsOverlay = document.getElementById("lfoSettingsOverlay");
const lfoMaxRateInput = document.getElementById("lfoMaxRateInput");
let pendingLfoId = null;

const LFO_SLIDER_CONFIG = {
  rate: { min: 0, max: LFO_MAX_RATE_HZ, step: 0.01 },
  amplitude: { min: 0, max: 10, step: 0.01 }, 
  phase: { min: -180, max: 180, step: 1 },
};

const LFO_PHASE_DETENTS = [-180, -90, 0, 90, 180];
const LFO_PHASE_SNAP_RANGE = 4;

function closeLfoSettings() {
  lfoSettingsOverlay.classList.add("hidden");
  pendingLfoId = null;
}

function setLfoMaxRate(widgetId, maxRate) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.maxRate = clamp(Number(maxRate) || 0, 0, LFO_MAX_RATE_HZ);
  if (state.rate > state.maxRate) state.rate = state.maxRate;
  audioEngine.setParam(widgetId, "maxRate", state.maxRate);
  audioEngine.setParam(widgetId, "rate", state.rate);
  updateLfoVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { maxRate: state.maxRate, rate: state.rate });
}

lfoSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === lfoSettingsOverlay) closeLfoSettings();
});
document.getElementById("lfoSettingsCancel").addEventListener("click", closeLfoSettings);
document.getElementById("lfoSettingsSave").addEventListener("click", () => {
  if (pendingLfoId !== null) setLfoMaxRate(pendingLfoId, lfoMaxRateInput.value);
  closeLfoSettings();
});

function ensurePhaseDetentDatalist() {
  if (document.getElementById("lfo-phase-detents")) return;
  const datalist = document.createElement("datalist");
  datalist.id = "lfo-phase-detents";
  LFO_PHASE_DETENTS.forEach((value) => {
    const option = document.createElement("option");
    option.value = String(value);
    datalist.appendChild(option);
  });
  document.body.appendChild(datalist);
}

function snapToDetent(value, detents, snapRange) {
  let closest = null;
  let closestDist = Infinity;
  for (const detent of detents) {
    const dist = Math.abs(value - detent);
    if (dist < closestDist) {
      closestDist = dist;
      closest = detent;
    }
  }
  return closestDist <= snapRange ? closest : value;
}

function formatLfoValue(portId, value) {
  if (portId === "rate") return `${formatNumber(value)} Hz`;
  if (portId === "amplitude") return `${formatNumber(value)} V`;
  if (portId === "phase") return `${Math.round(value)}°`;
  return formatNumber(value);
}




function wrapPhase(phase) {
  return ((phase % 1) + 1) % 1;
}

function lfoWaveValue(waveType, phase) {
  switch (waveType) {
    case "square":
      return phase < 0.5 ? 1 : -1;
    case "triangle":
      return 4 * Math.abs(phase - Math.floor(phase + 0.5)) - 1;
    case "sawtooth":
      return 2 * (phase - Math.floor(phase)) - 1;
    default:
      return Math.sin(phase * Math.PI * 2);
  }
}

function tickLfo(widgetId) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  const now = performance.now();
  const dt = (now - state.lastTick) / 1000;
  state.lastTick = now;

  
  
  
  
  
  
  const halfSwing = state.amplitude / 2;
  const center = state.bipolar ? 0 : (state.zeroPointFixed ? state.zeroPoint : halfSwing);
  
  
  
  
  const phaseOffset = state.phase / 360;

  let raw;
  if (state.oneShot && state.oneShotDone) {
    
    
    raw = lfoWaveValue(state.waveType, wrapPhase(state.cyclePos + phaseOffset));
  } else {
    state.cyclePos = (state.cyclePos + state.rate * dt) % 1;
    if (state.cyclePos < 0) state.cyclePos += 1;
    raw = lfoWaveValue(state.waveType, wrapPhase(state.cyclePos + phaseOffset));
    if (state.oneShot) {
      state.oneShotAccum += state.rate * dt;
      if (state.oneShotAccum >= 1) {
        state.oneShotDone = true;
        state.oneShotAccum = 0;
      }
    }
  }

  const value = state.bipolar
    ? clamp(center + raw * halfSwing, -AUDIO_PEAK_VOLTS, AUDIO_PEAK_VOLTS)
    : clamp(center + raw * halfSwing, 0, CV_MAX_VOLTS);
  propagate(widgetId, "value", roundFloat(value, 4));
}

function startLfoTimer(widgetId) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  stopLfoTimer(widgetId);
  state.lastTick = performance.now();
  state.timer = setInterval(() => tickLfo(widgetId), LFO_TICK_MS);
}

function stopLfoTimer(widgetId) {
  const state = lfoStates.get(widgetId);
  if (state && state.timer) {
    clearInterval(state.timer);
    state.timer = null;
  }
}

function updateLfoWaveButtons(widgetId) {
  const state = lfoStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".osc-wave-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.wave === state.waveType);
  });
}

function setLfoWaveType(widgetId, waveType) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.waveType = waveType;
  audioEngine.setParam(widgetId, "waveType", waveType);
  updateLfoWaveButtons(widgetId);
  sendLiveParamUpdate(widgetId, { waveType });
}

function updateLfoZeroModeButtons(widgetId) {
  const state = lfoStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".lfo-zero-mode-buttons .filter-type-btn").forEach((btn) => {
    btn.classList.toggle("active", (btn.dataset.zeroMode === "fixed") === state.zeroPointFixed);
  });
}

function setLfoZeroPointMode(widgetId, fixed) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.zeroPointFixed = fixed;
  audioEngine.setParam(widgetId, "zeroPointFixed", fixed);
  updateLfoZeroModeButtons(widgetId);
  sendLiveParamUpdate(widgetId, { zeroPointFixed: fixed });
}

function updateLfoPolarityButtons(widgetId) {
  const state = lfoStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".lfo-polarity-buttons .filter-type-btn").forEach((btn) => {
    btn.classList.toggle("active", (btn.dataset.polarity === "bipolar") === state.bipolar);
  });
  // Fixed Zero Point only means anything in unipolar mode - center is
  // always 0V while bipolar (see LfoNode.h::render()).
  el.querySelectorAll(".lfo-zero-mode-buttons .filter-type-btn, [data-role='lfo-zeropoint-port-slider']").forEach((elm) => {
    elm.disabled = state.bipolar;
  });
}

function setLfoPolarity(widgetId, bipolar) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.bipolar = bipolar;
  audioEngine.setParam(widgetId, "bipolar", bipolar);
  updateLfoPolarityButtons(widgetId);
  sendLiveParamUpdate(widgetId, { bipolar });
}

function setLfoZeroPoint(widgetId, volts) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.zeroPoint = volts;
  audioEngine.setParam(widgetId, "zeroPoint", volts);
  const el = document.getElementById(widgetId);
  const portSlider = el?.querySelector('[data-role="lfo-zeropoint-port-slider"]');
  if (portSlider) portSlider.value = String(volts);
  const portValue = el?.querySelector('[data-role="lfo-zeropoint-port-value"]');
  if (portValue) portValue.textContent = `${formatNumber(volts)} V`;
  sendLiveParamUpdate(widgetId, { zeroPoint: volts });
}





function setLfoOneShot(widgetId, on) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.oneShot = on;
  audioEngine.setParam(widgetId, "oneShot", on);
  sendLiveParamUpdate(widgetId, { oneShot: on });
}





function setLfoGlobal(widgetId, on) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.global = on;
  audioEngine.setParam(widgetId, "global", on);
  sendLiveParamUpdate(widgetId, { global: on });
}

function updateLfoVisual(widgetId) {
  const state = lfoStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const rateSlider = el.querySelector('[data-role="lfo-rate-slider"]');
  if (rateSlider) rateSlider.max = String(state.maxRate);

  ["rate", "amplitude", "phase"].forEach((portId) => {
    const slider = el.querySelector(`[data-role="lfo-${portId}-slider"]`);
    if (slider) slider.value = String(state[portId]);
    const valueSpan = el.querySelector(`[data-role="lfo-${portId}-value"]`);
    if (valueSpan) valueSpan.textContent = formatLfoValue(portId, state[portId]);
  });

  updateLfoZeroModeButtons(widgetId);
  updateLfoPolarityButtons(widgetId);
  const zpPortSlider = el.querySelector('[data-role="lfo-zeropoint-port-slider"]');
  if (zpPortSlider) zpPortSlider.value = String(state.zeroPoint);
  const zpPortValue = el.querySelector('[data-role="lfo-zeropoint-port-value"]');
  if (zpPortValue) zpPortValue.textContent = `${formatNumber(state.zeroPoint)} V`;
  const oneShotToggle = el.querySelector('[data-role="lfo-oneshot-switch"]');
  if (oneShotToggle) oneShotToggle.checked = state.oneShot === true;
  const globalToggle = el.querySelector('[data-role="lfo-global-switch"]');
  if (globalToggle) globalToggle.checked = state.global === true;
}



function tickAudioMonitors() {
  const now = performance.now();
  document.querySelectorAll('.widget-instance[data-widget-type="vu-meter"], .widget-instance[data-widget-type="vu-meter-horizontal"]').forEach((el) => {
    const state = audioMonitorStates.get(el.id);
    if (!state) return;
    const levelVolts = clamp(audioEngine.getLevel(el.id), 0, state.rangeVolts);
    const normalized = levelVolts / state.rangeVolts;
    const readout = el.querySelector('[data-role="vu-readout"]');
    if (readout) readout.textContent = `${levelVolts.toFixed(2)} V`;
    const needle = el.querySelector('[data-role="vu-needle"]');
    if (needle) needle.style.transform = `rotate(${-52 + normalized * 104}deg)`;
    const fill = el.querySelector('[data-role="vu-fill"]');
    if (fill) fill.style.transform = `scaleX(${normalized})`;

    const elapsedMs = Math.min(now - state.lastFrameMs, 100);
    if (levelVolts >= state.peakVolts) {
      state.peakVolts = levelVolts;
      state.holdUntil = now + 1500;
    } else if (now > state.holdUntil) {
      state.peakVolts = Math.max(levelVolts, state.peakVolts - state.rangeVolts * elapsedMs / 1200);
    }
    state.lastFrameMs = now;
    const peak = el.querySelector('[data-role="vu-peak"]');
    if (peak) {
      peak.style.left = `${clamp(state.peakVolts / state.rangeVolts, 0, 1) * 100}%`;
      peak.classList.toggle("visible", state.peakVolts > 0.001);
    }
  });
  document.querySelectorAll('.widget-instance[data-widget-type="audio-out"], .widget-instance[data-widget-type="usb-out"], .widget-instance[data-widget-type="es8311-out"], .widget-instance[data-widget-type="wm8960-out"]').forEach((el) => {
    ["left", "right"].forEach((channel) => {
      const fill = el.querySelector(`[data-role="audio-out-meter-${channel}"]`);
      if (!fill) return;
      const level = audioEngine.getLevel(el.id, channel);
      fill.style.width = `${clamp(level * 140, 0, 100)}%`;
    });
  });
  document.querySelectorAll('.widget-instance[data-widget-type="oscilloscope"]').forEach((el) => {
    const canvas = el.querySelector('[data-role="scope-canvas"]');
    if (!canvas) return;
    const waveform1 = audioEngine.getWaveform(el.id, "in1");
    if (!waveform1) return;
    const ch2Connected = isScopeChannelConnected(el.id, "in2");
    const waveform2 = ch2Connected ? audioEngine.getWaveform(el.id, "in2") : null;
    const cvMode = isScopeChannelCv(el.id, "in1") || (ch2Connected && isScopeChannelCv(el.id, "in2"));
    drawOscilloscope(canvas, waveform1, waveform2, cvMode);
  });
  document.querySelectorAll('.widget-instance[data-widget-type="spectrum"]').forEach((el) => {
    const state = audioMonitorStates.get(el.id);
    const canvas = el.querySelector('[data-role="spectrum-canvas"]');
    if (canvas && state) {
      const spectrum = audioEngine.getSpectrum(el.id);
      if (spectrum) drawSpectrum(canvas, spectrum, state.rangeVolts);
    }
    const axisCanvas = el.querySelector('[data-role="spectrum-axis-canvas"]');
    if (axisCanvas) drawSpectrumAxis(axisCanvas, audioEngine.ctx ? audioEngine.ctx.sampleRate : 0, 27);
  });
  document.querySelectorAll('.widget-instance[data-widget-type="esp-eq"]').forEach((el) => {
    const canvas = el.querySelector('[data-role="esp-eq-canvas"]');
    if (!canvas) return;
    const spectrum = audioEngine.getEspEqSpectrum(el.id);
    const response = audioEngine.getEspEqFrequencyResponse(el.id);
    if (!spectrum || !response) return;
    drawSpectrum(canvas, spectrum);
    drawEqCurve(canvas, response);
  });
  document.querySelectorAll('.widget-instance[data-widget-type="midi-in"]').forEach((el) => {
    const label = el.querySelector('[data-role="midi-in-voice-slots"]');
    if (!label) return;
    const state = midiInStates.get(el.id);
    const slotState = midiVoiceSlotStates.get(el.id);
    if (!state || !slotState) return;

    if (state.voices <= 1) {
      label.classList.add("hidden");
      return;
    }

    label.classList.remove("hidden");
    const active = slotState.slots.reduce((n, s) => n + (s.note !== null ? 1 : 0), 0);
    label.textContent = `${active}/${state.voices}`;
  });
  requestAnimationFrame(tickAudioMonitors);
}









function tickDebugVoltmeterAudioSources() {
  document
    .querySelectorAll('.widget-instance[data-widget-type="debug"], .widget-instance[data-widget-type="voltmeter"]')
    .forEach((el) => {
      const conn = connections.find((c) => c.to.widgetId === el.id && c.to.portId === "in");
      if (!conn) return;
      if (getPortKind(conn.from.widgetId, conn.from.portId, "out") !== "audio") return;
      const value = audioEngine.getPortValue(conn.from.widgetId, conn.from.portId);
      if (value === null) return;
      deliverToWidget(el.id, "in", value);
    });
}
setInterval(tickDebugVoltmeterAudioSources, 50);







const AUDIO_SIGNAL_WIDGET_TYPES = new Set([
  "oscillator", "hammond", "wave-player", "soundfont-player", "gain", "filter",
  "esp-eq", "esp-alc", "esp-channel-convert", "esp-mixer", "esp-sonic", "esp-drc", "esp-mbc",
  "mixer", "chorus", "delay", "reverb", "reverb-light", "audio-output-link", "audio-input-link", "switch", "multiplexer",
]);

function getScopeConnectedSourceType(widgetId, portId) {
  const conn = connections.find((c) => c.to.widgetId === widgetId && c.to.portId === portId);
  if (!conn) return null;
  const fromEl = document.getElementById(conn.from.widgetId);
  return fromEl ? fromEl.dataset.widgetType : null;
}

function isScopeChannelConnected(widgetId, portId) {
  return getScopeConnectedSourceType(widgetId, portId) !== null;
}




function isScopeChannelCv(widgetId, portId) {
  const sourceType = getScopeConnectedSourceType(widgetId, portId);
  return sourceType !== null && !AUDIO_SIGNAL_WIDGET_TYPES.has(sourceType);
}

const SCOPE_CH1_COLOR = "#4ade80";
const SCOPE_CH2_COLOR = "#f59e0b";





const SCOPE_CV_DISPLAY_VOLTS = 12;
const SCOPE_AUDIO_DISPLAY_VOLTS = 5;

function drawOscilloscopeTrace(ctx2d, waveform, w, toY) {
  ctx2d.beginPath();
  const step = w / waveform.length;
  for (let i = 0; i < waveform.length; i += 1) {
    const x = i * step;
    const y = toY(waveform[i]);
    if (i === 0) ctx2d.moveTo(x, y);
    else ctx2d.lineTo(x, y);
  }
  ctx2d.stroke();
}

function drawOscilloscope(canvas, waveform1, waveform2, cvMode) {
  const ctx2d = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx2d.clearRect(0, 0, w, h);

  
  
  
  
  
  // Audio signals use the engine's nominal +/-5 V domain. Using Web Audio's
  // conventional +/-1 range here clipped every normal full-level waveform.
  // CV stays wider so regular 0-10 V control signals remain fully visible.
  const range = cvMode ? SCOPE_CV_DISPLAY_VOLTS : SCOPE_AUDIO_DISPLAY_VOLTS;
  const toY = (v) => (1 - clamp(v, -range, range) / range) * (h / 2);

  if (cvMode) {
    ctx2d.strokeStyle = "rgba(255, 255, 255, 0.15)";
    ctx2d.lineWidth = 1;
    ctx2d.beginPath();
    ctx2d.moveTo(0, h / 2);
    ctx2d.lineTo(w, h / 2);
    ctx2d.stroke();
  }

  ctx2d.lineWidth = 1.5;
  ctx2d.strokeStyle = SCOPE_CH1_COLOR;
  drawOscilloscopeTrace(ctx2d, waveform1, w, toY);
  if (waveform2) {
    ctx2d.strokeStyle = SCOPE_CH2_COLOR;
    drawOscilloscopeTrace(ctx2d, waveform2, w, toY);
  }
}





function drawSpectrum(canvas, spectrum, rangeVolts = null) {
  const ctx2d = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx2d.clearRect(0, 0, w, h);
  const plotLeft = rangeVolts ? 27 : 0;
  const plotWidth = w - plotLeft;
  if (rangeVolts) {
    ctx2d.font = "9px sans-serif";
    ctx2d.textAlign = "right";
    ctx2d.textBaseline = "middle";
    [0, 0.5, 1].forEach((fraction) => {
      const y = h - 3 - fraction * (h - 8);
      ctx2d.strokeStyle = "rgba(148, 163, 184, 0.18)";
      ctx2d.beginPath();
      ctx2d.moveTo(plotLeft, y);
      ctx2d.lineTo(w, y);
      ctx2d.stroke();
      ctx2d.fillStyle = "#94a3b8";
      ctx2d.fillText(`${rangeVolts * fraction}V`, plotLeft - 3, y);
    });
  }
  ctx2d.fillStyle = "#4ade80";
  const barWidth = plotWidth / spectrum.length;
  for (let i = 0; i < spectrum.length; i += 1) {
    const normalized = rangeVolts ? clamp(spectrum[i] / rangeVolts, 0, 1) : spectrum[i] / 255;
    const barHeight = normalized * (h - (rangeVolts ? 5 : 0));
    ctx2d.fillRect(plotLeft + i * barWidth, h - barHeight, Math.ceil(barWidth), barHeight);
  }
}






const SPECTRUM_AXIS_TICKS_HZ = [0, 2000, 5000, 10000, 15000, 20000];

function drawSpectrumAxis(canvas, sampleRate, leftPadding = 0) {
  const ctx2d = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx2d.clearRect(0, 0, w, h);
  if (!sampleRate) return;
  const nyquist = sampleRate / 2;
  ctx2d.fillStyle = "#94a3b8";
  ctx2d.font = "9px sans-serif";
  ctx2d.textBaseline = "top";
  SPECTRUM_AXIS_TICKS_HZ.filter((hz) => hz <= nyquist).forEach((hz) => {
    const x = leftPadding + (hz / nyquist) * (w - leftPadding);
    ctx2d.textAlign = x < 12 ? "left" : x > w - 12 ? "right" : "center";
    ctx2d.fillText(hz === 0 ? "0" : `${hz / 1000}k`, x, 2);
  });
}





const EQ_CURVE_RANGE_DB = 24;
function drawEqCurve(canvas, magResponse) {
  const ctx2d = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  const step = w / magResponse.length;
  ctx2d.strokeStyle = "#facc15";
  ctx2d.lineWidth = 2;
  ctx2d.beginPath();
  for (let i = 0; i < magResponse.length; i += 1) {
    const db = clamp(20 * Math.log10(Math.max(magResponse[i], 1e-6)), -EQ_CURVE_RANGE_DB, EQ_CURVE_RANGE_DB);
    const x = i * step;
    const y = h / 2 - (db / EQ_CURVE_RANGE_DB) * (h / 2);
    if (i === 0) ctx2d.moveTo(x, y);
    else ctx2d.lineTo(x, y);
  }
  ctx2d.stroke();
}

requestAnimationFrame(tickAudioMonitors);

const clockSettingsOverlay = document.getElementById("clockSettingsOverlay");
const clockPulseMsInput = document.getElementById("clockPulseMsInput");
const clockMinBpmInput = document.getElementById("clockMinBpmInput");
const clockMaxBpmInput = document.getElementById("clockMaxBpmInput");
let pendingClockId = null;

function openClockSettings(widgetId) {
  const state = clockStates.get(widgetId);
  if (!state) return;
  pendingClockId = widgetId;
  clockPulseMsInput.value = state.pulseMs;
  clockMinBpmInput.value = state.minBpm;
  clockMaxBpmInput.value = state.maxBpm;
  clockSettingsOverlay.classList.remove("hidden");
}

function closeClockSettings() {
  clockSettingsOverlay.classList.add("hidden");
  pendingClockId = null;
}

clockSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === clockSettingsOverlay) closeClockSettings();
});

document.getElementById("clockSettingsCancel").addEventListener("click", closeClockSettings);

document.getElementById("clockSettingsSave").addEventListener("click", () => {
  if (!pendingClockId) return;
  const state = clockStates.get(pendingClockId);
  if (state) {
    let ms = parseFloatFlexible(clockPulseMsInput.value);
    if (Number.isNaN(ms) || ms <= 0) ms = 100;
    let minBpm = parseFloatFlexible(clockMinBpmInput.value);
    let maxBpm = parseFloatFlexible(clockMaxBpmInput.value);
    if (Number.isNaN(minBpm) || minBpm < 0) minBpm = 0;
    if (Number.isNaN(maxBpm) || maxBpm < 0) maxBpm = 240;
    if (maxBpm < minBpm) maxBpm = minBpm;
    state.pulseMs = ms;
    state.minBpm = minBpm;
    state.maxBpm = maxBpm;
    state.bpm = clockBpm(state.cvVolts, state.minBpm, state.maxBpm);
    restartClockTimer(pendingClockId);
    markDirty();
  }
  closeClockSettings();
});



const oscillatorStates = new Map();

const hammondStates = new Map();
const HAMMOND_DEFAULT_DRAWBARS = [10, 10, 10, 0, 0, 0, 0, 0, 0]; // volts, 888 000 000
const HAMMOND_DEFAULT_RATIOS = [0.5, 1.5, 1, 2, 3, 4, 5, 6, 8];   // classic Hammond footages
const HAMMOND_FOOTAGES = ["16′", "5⅓′", "8′", "4′", "2⅔′", "2′", "1⅗′", "1⅓′", "1′"];

function updateOscillatorFreqLabel(widgetId) {
  const state = oscillatorStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const freqLabel = el.querySelector('[data-role="osc-freq-label"]');
  if (freqLabel) freqLabel.textContent = `${formatNumber(state.freq)} Hz`;
}








function recomputeOscillatorFreq(widgetId, voiceIndex) {
  const state = oscillatorStates.get(widgetId);
  if (!state) return;
  const center = CV_MAX_VOLTS / 2;
  const totalVolts = state.pitchCv + (state.tuneVolts - center);
  const hz = hzFromPitchCv(totalVolts);
  state.freq = hz;
  if (voiceIndex !== undefined) {
    audioEngine.setVoiceParam(widgetId, voiceIndex, "frequency", hz, 0);
    
    
    
    
    
    audioEngine.setVoiceParam(widgetId, voiceIndex, "tuneVolts", state.tuneVolts, 0);
  } else {
    audioEngine.setParam(widgetId, "frequency", hz, 0);
    audioEngine.setParam(widgetId, "tuneVolts", state.tuneVolts, 0);
  }
  updateOscillatorFreqLabel(widgetId);
}





function updateOscillatorTuneVisual(widgetId) {
  const state = oscillatorStates.get(widgetId);
  if (!state) return;
  
  
  
  
  
  const tuneAudioConnected = connections.some(
    (c) => c.to.widgetId === widgetId && c.to.portId === "tune" && getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = document.querySelector(`#${widgetId} [data-role="oscillator-tune-slider"]`);
  if (slider) {
    slider.value = String(state.tuneVolts);
    slider.disabled = tuneAudioConnected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="oscillator-tune-value"]`);
  if (valueSpan) {
    valueSpan.textContent = formatNumber(state.tuneVolts);
  }
}






function updateOscillatorGainVisual(widgetId) {
  const state = oscillatorStates.get(widgetId);
  if (!state) return;
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "gain" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = document.querySelector(`#${widgetId} [data-role="oscillator-gain-slider"]`);
  if (slider) {
    slider.value = String(state.gain);
    slider.disabled = audioConnected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="oscillator-gain-value"]`);
  if (valueSpan) {
    valueSpan.textContent = formatNumber(state.gain);
  }
}

function updateOscillatorWaveButtons(widgetId) {
  const state = oscillatorStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".osc-wave-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.wave === state.waveType);
  });
}

function setOscillatorWaveType(widgetId, waveType) {
  const state = oscillatorStates.get(widgetId);
  if (!state) return;
  state.waveType = waveType;
  audioEngine.setParam(widgetId, "waveType", waveType);
  updateOscillatorWaveButtons(widgetId);
  sendLiveParamUpdate(widgetId, { waveType });
}





function mapWaveformValue(value) {
  const idx = clamp(Math.floor(value / (CV_MAX_VOLTS / WAVE_TYPE_BUTTONS.length)), 0, WAVE_TYPE_BUTTONS.length - 1);
  return WAVE_TYPE_BUTTONS[idx].id;
}



const audioOutStates = new Map();
const audioInputLinkStates = new Map();

function buildAudioMeterTrack(role) {
  const track = document.createElement("div");
  track.className = "audio-out-meter-track";

  const fill = document.createElement("div");
  fill.className = "audio-out-meter-fill";
  fill.dataset.role = role;
  track.appendChild(fill);
  return track;
}

function updateAudioOutVisual(widgetId) {
  const state = audioOutStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const mute = el.querySelector('[data-role="audio-out-mute"]');
  const volume = el.querySelector('[data-role="audio-out-volume"]');
  const value = el.querySelector('[data-role="audio-out-volume-value"]');
  if (mute) mute.checked = state.muted;
  if (volume) volume.value = String(state.gain);
  if (value) value.textContent = `${state.gain.toFixed(1)} V`;
}

function setAudioOutParams(widgetId, params, sendLive = true) {
  const state = audioOutStates.get(widgetId);
  if (!state) return;
  if (typeof params.muted === "boolean") state.muted = params.muted;
  if (typeof params.gain === "number") state.gain = clamp(params.gain, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, "muted", state.muted);
  audioEngine.setParam(widgetId, "gain", state.gain);
  updateAudioOutVisual(widgetId);
  if (sendLive) sendLiveParamUpdateThrottled(widgetId, { muted: state.muted, gain: state.gain });
}

function updateAudioInputLinkVisual(widgetId) {
  const state = audioInputLinkStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  state.gains.forEach((gain, channel) => {
    const slider = el.querySelector(`[data-role="audio-input-link-gain-${channel}"]`);
    const value = el.querySelector(`[data-role="audio-input-link-gain-value-${channel}"]`);
    if (slider) slider.value = String(gain);
    if (value) value.textContent = `${formatNumber(gain)} V`;
  });
}

function setAudioInputLinkGain(widgetId, channel, value, sendLive = true) {
  const state = audioInputLinkStates.get(widgetId);
  if (!state || channel < 0 || channel >= LINK_BUS_COUNT) return;
  state.gains[channel] = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, `gain${channel + 1}`, state.gains[channel]);
  updateAudioInputLinkVisual(widgetId);
  if (sendLive) sendLiveParamUpdateThrottled(widgetId, { [`gain${channel + 1}`]: state.gains[channel] });
}



const gainStates = new Map();

const GAIN_CURVE_BUTTONS = [
  { id: "linear", label: "Lin", title: "Linear: A = CV/10" },
  { id: "exponential", label: "Exp", title: "Exponential: A = (10^(CV/10) - 1) / 9" },
];

function setGainCurve(widgetId, curve) {
  const state = gainStates.get(widgetId);
  if (!state) return;
  state.curve = curve;
  audioEngine.setParam(widgetId, "curve", state.curve);
  updateGainVisual(widgetId);
  sendLiveParamUpdate(widgetId, { curve: state.curve });
}





function setGainGlobal(widgetId, on) {
  const state = gainStates.get(widgetId);
  if (!state) return;
  state.global = on;
  audioEngine.setParam(widgetId, "global", on);
  updateGainVisual(widgetId);
  sendLiveParamUpdate(widgetId, { global: on });
}



function formatGainCvValue(state) {
  const mapped = vcaAmountFromCvDisplay(state.amount, state.curve);
  return `${formatNumber(state.amount)}V → A=${mapped.toFixed(2)}`;
}




function vcaAmountFromCvDisplay(volts, curve) {
  const v = clamp(volts / CV_MAX_VOLTS, 0, 1);
  return curve === "exponential" ? (10 ** v - 1) / 9 : v;
}

function updateGainVisual(widgetId) {
  const state = gainStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".gain-body .filter-type-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.curve === state.curve);
  });
  
  
  
  
  
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "amount" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = el.querySelector('[data-role="gain-amount-slider"]');
  if (slider) {
    slider.value = String(state.amount);
    slider.disabled = audioConnected;
  }
  const valueSpan = el.querySelector('[data-role="gain-amount-value"]');
  if (valueSpan) {
    valueSpan.textContent = formatGainCvValue(state);
  }
  const globalToggle = el.querySelector('[data-role="gain-global-switch"]');
  if (globalToggle) globalToggle.checked = state.global === true;
}



const filterStates = new Map();
const ESP_EFFECT_DEFAULTS = {
  "esp-eq": { filterType: "peak", frequency: 1000, q: 1, gainDb: 0, bypass: false },
  "esp-alc": { gainL: 0, gainR: 0, bypass: false },
  "esp-channel-convert": { weightL: 0.70710678, weightR: 0.70710678, bypass: false },
  // esp-mixer deliberately excluded from bypass (see hasBypass()'s comment).
  "esp-mixer": { weight1: 1, weight2: 0, fadeMs: 100, mode: 0 },
  "esp-sonic": { speed: 1, pitch: 1, bypass: false },
  "esp-drc": { threshold: -20, ratio: 4, attackMs: 10, releaseMs: 100, makeupDb: 0, kneeDb: 2, bypass: false },
  "esp-mbc": { threshold: -22, ratio: 2, makeupDb: 0, bypass: false },
};
const espEffectStates = new Map();

const ESP_EFFECT_SLIDER_CONFIG = {
  "esp-eq": {
    frequency: { min: 20, max: 22049, step: 1, unit: " Hz" },
    q: { min: 0.1, max: 20, step: 0.1, unit: "" },
    gainDb: { min: -15, max: 15, step: 0.1, unit: " dB" },
  },
  "esp-sonic": {
    speed: { min: 0.5, max: 2, step: 0.01, unit: "×" },
    pitch: { min: 0.5, max: 2, step: 0.01, unit: "×" },
  },
  "esp-drc": {
    attackMs: { min: 0, max: 500, step: 1, unit: " ms" },
    releaseMs: { min: 0, max: 500, step: 1, unit: " ms" },
    makeupDb: { min: -10, max: 10, step: 0.1, unit: " dB" },
  },
};

function formatEspEffectValue(config, value) {
  return `${formatNumber(value)}${config.unit}`;
}

function updateEspEffectVisual(widgetId) {
  const state = espEffectStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  if (el.dataset.widgetType === "esp-eq") {
    el.querySelectorAll(".eq-filter-type-btn").forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.filterType === state.filterType);
    });
  }
  const bypassToggle = el.querySelector(`[data-role="${el.dataset.widgetType}-bypass-switch"]`);
  if (bypassToggle) bypassToggle.checked = state.bypass === true;
  const config = ESP_EFFECT_SLIDER_CONFIG[el.dataset.widgetType];
  if (!config) return;
  Object.entries(config).forEach(([portId, cfg]) => {
    const slider = el.querySelector(`[data-role="esp-effect-${portId}-slider"]`);
    if (slider) slider.value = String(state[portId]);
    const valueSpan = el.querySelector(`[data-role="esp-effect-${portId}-value"]`);
    if (valueSpan) valueSpan.textContent = formatEspEffectValue(cfg, state[portId]);
  });
}





const EQ_FILTER_TYPE_LABELS = {
  highpass: "Highpass",
  lowpass: "Lowpass",
  peak: "Peak",
  highshelf: "High Shelf",
  lowshelf: "Low Shelf",
};

const EQ_FILTER_TYPE_BUTTONS = [
  { id: "highpass", label: "HP" },
  { id: "lowpass", label: "LP" },
  { id: "peak", label: "PK" },
  { id: "highshelf", label: "HS" },
  { id: "lowshelf", label: "LS" },
];

function setEspEqFilterType(widgetId, filterType) {
  const state = espEffectStates.get(widgetId);
  if (!state) return;
  state.filterType = filterType;
  audioEngine.setParam(widgetId, "filterType", filterType);
  updateEspEffectVisual(widgetId);
  sendLiveParamUpdate(widgetId, { filterType });
}

const FILTER_TYPE_LABELS = {
  lowpass: "Lowpass",
  highpass: "Highpass",
  bandpass: "Bandpass",
};

const FILTER_TYPE_BUTTONS = [
  { id: "lowpass", label: "LP" },
  { id: "highpass", label: "HP" },
  { id: "bandpass", label: "BP" },
];

const CURTIS_FILTER_TYPE_LABELS = {
  lp24: "24 dB Lowpass",
  lp12: "12 dB Lowpass",
  bp12: "12 dB Bandpass",
  hp12: "12 dB Highpass",
};

const CURTIS_FILTER_TYPE_BUTTONS = [
  { id: "lp24", label: "LP24" },
  { id: "lp12", label: "LP12" },
  { id: "bp12", label: "BP12" },
  { id: "hp12", label: "HP12" },
];

function setFilterType(widgetId, filterType) {
  const state = filterStates.get(widgetId);
  if (!state) return;
  state.filterType = filterType;
  audioEngine.setParam(widgetId, "filterType", filterType);
  updateFilterVisual(widgetId);
  sendLiveParamUpdate(widgetId, { filterType });
}




function setFilterGlobal(widgetId, on) {
  const state = filterStates.get(widgetId);
  if (!state) return;
  state.global = on;
  audioEngine.setParam(widgetId, "global", on);
  updateFilterVisual(widgetId);
  sendLiveParamUpdate(widgetId, { global: on });
}




function formatFilterCvValue(state, portId) {
  const isCutoff = portId === "cutoff";
  const min = isCutoff ? state.cutoffMinHz : state.resonanceMinQ;
  const max = isCutoff ? state.cutoffMaxHz : state.resonanceMaxQ;
  const curve = isCutoff ? state.cutoffCurve : state.resonanceCurve;
  const volts = isCutoff ? state.cutoff : state.resonance;
  const mapped = cvToRange(volts, min, max, curve);
  return isCutoff ? `${Math.round(mapped)} Hz` : formatNumber(mapped);
}

function updateFilterVisual(widgetId) {
  const state = filterStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  el.querySelectorAll(".filter-type-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.filterType === state.filterType);
  });

  const globalToggle = el.querySelector('[data-role="filter-global-switch"]');
  if (globalToggle) globalToggle.checked = state.global === true;
  const bypassToggle = el.querySelector(`[data-role="${el.dataset.widgetType}-bypass-switch"]`);
  if (bypassToggle) bypassToggle.checked = state.bypass === true;

  
  
  
  
  
  const cutoffAudioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId && c.to.portId === "cutoff" && getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const cutoffSlider = el.querySelector('[data-role="filter-cutoff-slider"]');
  if (cutoffSlider) {
    cutoffSlider.value = String(state.cutoff);
    cutoffSlider.disabled = cutoffAudioConnected;
  }
  const cutoffValue = el.querySelector('[data-role="filter-cutoff-value"]');
  if (cutoffValue) {
    cutoffValue.textContent = formatFilterCvValue(state, "cutoff");
  }

  const resonanceAudioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId && c.to.portId === "resonance" && getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const resonanceSlider = el.querySelector('[data-role="filter-resonance-slider"]');
  if (resonanceSlider) {
    resonanceSlider.value = String(state.resonance);
    resonanceSlider.disabled = resonanceAudioConnected;
  }
  const resonanceValue = el.querySelector('[data-role="filter-resonance-value"]');
  if (resonanceValue) {
    resonanceValue.textContent = formatFilterCvValue(state, "resonance");
  }
}



const filterSettingsOverlay = document.getElementById("filterSettingsOverlay");
const filterCutoffMinInput = document.getElementById("filterCutoffMinInput");
const filterCutoffMaxInput = document.getElementById("filterCutoffMaxInput");
const filterCutoffCurveSelect = document.getElementById("filterCutoffCurveSelect");
const filterResonanceMinInput = document.getElementById("filterResonanceMinInput");
const filterResonanceMaxInput = document.getElementById("filterResonanceMaxInput");
const filterResonanceCurveSelect = document.getElementById("filterResonanceCurveSelect");
const filterCoeffStrideSelect = document.getElementById("filterCoeffStrideSelect");
let pendingFilterId = null;

function openFilterSettings(widgetId) {
  const state = filterStates.get(widgetId);
  if (!state) return;
  pendingFilterId = widgetId;
  filterCutoffMinInput.value = state.cutoffMinHz;
  filterCutoffMaxInput.value = state.cutoffMaxHz;
  filterCutoffCurveSelect.value = state.cutoffCurve;
  filterResonanceMinInput.value = state.resonanceMinQ;
  filterResonanceMaxInput.value = state.resonanceMaxQ;
  filterResonanceCurveSelect.value = state.resonanceCurve;
  filterCoeffStrideSelect.value = String(state.coeffStride ?? 4);
  filterSettingsOverlay.classList.remove("hidden");
}

function closeFilterSettings() {
  filterSettingsOverlay.classList.add("hidden");
  pendingFilterId = null;
}

filterSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === filterSettingsOverlay) closeFilterSettings();
});

document.getElementById("filterSettingsCancel").addEventListener("click", closeFilterSettings);

document.getElementById("filterSettingsSave").addEventListener("click", () => {
  if (!pendingFilterId) return;
  const state = filterStates.get(pendingFilterId);
  if (state) {
    const cutoffMin = parseFloatFlexible(filterCutoffMinInput.value);
    const cutoffMax = parseFloatFlexible(filterCutoffMaxInput.value);
    const resonanceMin = parseFloatFlexible(filterResonanceMinInput.value);
    const resonanceMax = parseFloatFlexible(filterResonanceMaxInput.value);
    state.cutoffMinHz = Number.isNaN(cutoffMin) || cutoffMin <= 0 ? 20 : cutoffMin;
    state.cutoffMaxHz = Number.isNaN(cutoffMax) || cutoffMax <= state.cutoffMinHz ? 20000 : cutoffMax;
    state.cutoffCurve = filterCutoffCurveSelect.value === "linear" ? "linear" : "log";
    state.resonanceMinQ = Number.isNaN(resonanceMin) || resonanceMin <= 0 ? 0.1 : resonanceMin;
    state.resonanceMaxQ = Number.isNaN(resonanceMax) || resonanceMax <= state.resonanceMinQ ? 20 : resonanceMax;
    state.resonanceCurve = filterResonanceCurveSelect.value === "linear" ? "linear" : "log";
    
    
    
    
    const coeffStride = parseInt(filterCoeffStrideSelect.value, 10);
    state.coeffStride = Number.isNaN(coeffStride) ? 4 : clamp(coeffStride, 1, 16);

    audioEngine.setParam(pendingFilterId, "cutoffMinHz", state.cutoffMinHz);
    audioEngine.setParam(pendingFilterId, "cutoffMaxHz", state.cutoffMaxHz);
    audioEngine.setParam(pendingFilterId, "cutoffCurve", state.cutoffCurve);
    audioEngine.setParam(pendingFilterId, "resonanceMinQ", state.resonanceMinQ);
    audioEngine.setParam(pendingFilterId, "resonanceMaxQ", state.resonanceMaxQ);
    audioEngine.setParam(pendingFilterId, "resonanceCurve", state.resonanceCurve);
    updateFilterVisual(pendingFilterId);
    sendLiveParamUpdate(pendingFilterId, {
      cutoffMinHz: state.cutoffMinHz,
      cutoffMaxHz: state.cutoffMaxHz,
      cutoffCurve: state.cutoffCurve,
      resonanceMinQ: state.resonanceMinQ,
      resonanceMaxQ: state.resonanceMaxQ,
      resonanceCurve: state.resonanceCurve,
      coeffStride: state.coeffStride,
    });
  }
  closeFilterSettings();
});



const detuneStates = new Map();

const DETUNE_MAX_VOLTS = 0.5;



const DETUNE_MAX_CENTS = DETUNE_MAX_VOLTS * 1200;
const DETUNE_CENTS_STEP = 100; 

function detuneAmountToCents(amount) {
  return clamp(amount, -1, 1) * DETUNE_MAX_CENTS;
}

function detuneCentsToAmount(cents) {
  return clamp(cents, -DETUNE_MAX_CENTS, DETUNE_MAX_CENTS) / DETUNE_MAX_CENTS;
}

function detuneOutputs(state) {
  const amount = clamp(state.amount, -1, 1);
  const deltaVolts = amount * DETUNE_MAX_VOLTS;
  return { f1: state.cv + deltaVolts, f2: state.cv - deltaVolts };
}








function recomputeDetune(widgetId, cv, voiceIndex) {
  const state = detuneStates.get(widgetId);
  if (!state) return;
  if (cv !== undefined) state.cv = cv;
  if (state.audioMode) return;
  const { f1, f2 } = detuneOutputs(state);
  propagate(widgetId, "f1", roundFloat(f1, 4), voiceIndex);
  propagate(widgetId, "f2", roundFloat(f2, 4), voiceIndex);
}




function updateDetuneMode(widgetId) {
  const state = detuneStates.get(widgetId);
  if (!state) return;
  updateDualModeAudioNode(widgetId, "detune", state, ["cv"], { amount: state.amount });
  recomputeDetune(widgetId);
}

function updateDetuneVisual(widgetId) {
  const state = detuneStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const cents = Math.round(detuneAmountToCents(state.amount) / DETUNE_CENTS_STEP) * DETUNE_CENTS_STEP;
  const slider = el.querySelector('[data-role="detune-amount-slider"]');
  if (slider) slider.value = String(cents);
  const valueSpan = el.querySelector('[data-role="detune-amount-value"]');
  if (valueSpan) valueSpan.textContent = `${cents > 0 ? "+" : ""}${cents} ct`;
}



const MIDI_SPLIT_MAX_ZONES = 4;
const midiSplitStates = new Map();









function deliverMidiSplitZones(midiInId, note, velocity, noteOn, voiceIndex) {
  connections
    .filter(
      (c) =>
        c.from.widgetId === midiInId &&
        c.from.portId === "midi" &&
        c.to.portId === "midi" &&
        document.getElementById(c.to.widgetId)?.dataset.widgetType === "midi-split"
    )
    .forEach((c) => {
      const splitId = c.to.widgetId;
      const state = midiSplitStates.get(splitId);
      if (!state) return;
      for (let z = 0; z < state.zoneCount; z += 1) {
        const zone = state.zones[z];
        if (note < zone.from || note > zone.to) continue;
        const transposedNote = clamp(note + zone.octave * 12, 0, 127);
        const prefix = `zone${z + 1}`;
        if (noteOn) propagate(splitId, `${prefix}Velocity`, velocityToVoltage(velocity), voiceIndex);
        propagate(splitId, `${prefix}Pitch`, noteToVoltage(transposedNote), voiceIndex);
        propagate(splitId, `${prefix}Note`, transposedNote, voiceIndex);
        propagate(splitId, `${prefix}Gate`, noteOn ? GATE_ON_VOLTS : GATE_OFF_VOLTS, voiceIndex);
        updateMidiSplitZoneLed(splitId, z, noteOn);
      }
    });
}




function updateMidiSplitZoneLed(widgetId, zoneIdx, noteOn) {
  const state = midiSplitStates.get(widgetId);
  if (!state) return;
  if (!state.zoneActiveCount) state.zoneActiveCount = [0, 0, 0, 0];
  state.zoneActiveCount[zoneIdx] = Math.max(0, state.zoneActiveCount[zoneIdx] + (noteOn ? 1 : -1));
  const el = document.getElementById(widgetId);
  const dot = el?.querySelector(`[data-role="midi-split-zone${zoneIdx + 1}-midi-dot"]`);
  if (dot) setLedActive(dot, state.zoneActiveCount[zoneIdx] > 0);
}

function renderMidiSplitPorts(widgetId, portsEl) {
  const state = midiSplitStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  WIDGET_TYPES["midi-split"].inputs.forEach((port) => {
    portsEl.appendChild(createPortRow(widgetId, port, "in", "midi-split"));
  });

  for (let z = 1; z <= state.zoneCount; z += 1) {
    const midiRow = createPortRow(widgetId, { id: `zone${z}Midi`, label: `Zone ${z} MIDI` }, "out", "midi-split");
    const dot = document.createElement("span");
    dot.className = "adsr-gate-dot";
    dot.dataset.role = `midi-split-zone${z}-midi-dot`;
    midiRow.insertBefore(dot, midiRow.querySelector(".port-label"));
    portsEl.appendChild(midiRow);
    portsEl.appendChild(createPortRow(widgetId, { id: `zone${z}Pitch`, label: `Zone ${z} Pitch` }, "out", "midi-split"));
    portsEl.appendChild(createPortRow(widgetId, { id: `zone${z}Note`, label: `Zone ${z} Note` }, "out", "midi-split"));
    portsEl.appendChild(createPortRow(widgetId, { id: `zone${z}Velocity`, label: `Zone ${z} Vel` }, "out", "midi-split"));
    portsEl.appendChild(createPortRow(widgetId, { id: `zone${z}Gate`, label: `Zone ${z} Gate` }, "out", "midi-split"));
  }
  layoutPortsGrid(portsEl);
}



const chorusStates = new Map();

const CHORUS_SLIDER_CONFIG = {
  rate: { min: 0.05, max: 8, step: 0.01 },
  depth: { min: 0, max: 0.015, step: 0.0002 },
  offset: { min: 0, max: 0.04, step: 0.001 },
};

function formatChorusValue(portId, value) {
  if (portId === "rate") return `${formatNumber(value)} Hz`;
  return `${formatNumber(value * 1000)} ms`;
}

function updateChorusVisual(widgetId) {
  const state = chorusStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  ["rate", "depth", "offset"].forEach((portId) => {
    const slider = el.querySelector(`[data-role="chorus-${portId}-slider"]`);
    if (slider) slider.value = String(state[portId]);
    const valueSpan = el.querySelector(`[data-role="chorus-${portId}-value"]`);
    if (valueSpan) valueSpan.textContent = formatChorusValue(portId, state[portId]);
  });
  const bypassToggle = el.querySelector('[data-role="chorus-bypass-switch"]');
  if (bypassToggle) bypassToggle.checked = state.bypass === true;
}



const EFFECT_CV_PARAMS = {
  delay: [
    { portId: "time", paramName: "delayTimeMs", label: "Time", unit: "ms", defaultMin: 0, defaultMax: 2000, defaultCurve: "linear", defaultValue: 300 },
    { portId: "feedback", paramName: "feedback", label: "Feedback", unit: "", defaultMin: 0, defaultMax: 0.95, defaultCurve: "linear", defaultValue: 0.3 },
    { portId: "mix", paramName: "mix", label: "Mix", unit: "", defaultMin: 0, defaultMax: 1, defaultCurve: "linear", defaultValue: 0.35 },
  ],
  reverb: [
    { portId: "roomSize", paramName: "roomSize", label: "Room Size", unit: "", defaultMin: 0, defaultMax: 1, defaultCurve: "linear", defaultValue: 0.5 },
    { portId: "damping", paramName: "damping", label: "Damping", unit: "", defaultMin: 0, defaultMax: 1, defaultCurve: "linear", defaultValue: 0.5 },
    { portId: "wetLevel", paramName: "wetLevelDb", label: "Wet Level", unit: "dB", defaultMin: -96, defaultMax: 0, defaultCurve: "linear", defaultValue: -6 },
    { portId: "dryLevel", paramName: "dryLevelDb", label: "Dry Level", unit: "dB", defaultMin: -96, defaultMax: 0, defaultCurve: "linear", defaultValue: 0 },
    { portId: "preDelay", paramName: "preDelayMs", label: "Pre-Delay", unit: "ms", defaultMin: 0, defaultMax: 200, defaultCurve: "linear", defaultValue: 0 },
  ],
  
  
  
  "reverb-light": [
    { portId: "roomSize", paramName: "roomSize", label: "Room Size", unit: "", defaultMin: 0, defaultMax: 1, defaultCurve: "linear", defaultValue: 0.5 },
    { portId: "damping", paramName: "damping", label: "Damping", unit: "", defaultMin: 0, defaultMax: 1, defaultCurve: "linear", defaultValue: 0.5 },
    { portId: "wetLevel", paramName: "wetLevelDb", label: "Wet Level", unit: "dB", defaultMin: -96, defaultMax: 0, defaultCurve: "linear", defaultValue: -6 },
    { portId: "dryLevel", paramName: "dryLevelDb", label: "Dry Level", unit: "dB", defaultMin: -96, defaultMax: 0, defaultCurve: "linear", defaultValue: 0 },
  ],
};



function isReverbType(type) {
  return type === "reverb" || type === "reverb-light";
}

// Types whose "bypass" flows through the generic effectCvStates/
// makeEffectCvEngine machinery below (reverb/reverb-light already did;
// delay gets bypass the same way). Filter/CurtisFilter/Chorus/the ESP
// effects have their OWN state maps (filterStates/chorusStates/
// espEffectStates) and do NOT go through this engine - see hasBypass()
// below for the full cross-type set used by the shared toggle-switch UI.
function isEffectCvBypassType(type) {
  return isReverbType(type) || type === "delay";
}

// Every widget type that gets a Bypass toggle switch (see the port-row
// loop below and BYPASS_STATE_MAPS just after it) - reverb/reverb-light/
// delay share effectCvStates via isEffectCvBypassType(); filter/curtis-
// filter/chorus/the 5 ESP effects each keep their own state map. EspMixer
// deliberately excluded (2-input mixer, no single dry signal - see the
// backend plan).
function hasBypass(type) {
  return (
    isEffectCvBypassType(type) ||
    type === "filter" ||
    type === "curtis-filter" ||
    type === "chorus" ||
    type === "esp-eq" ||
    type === "esp-alc" ||
    type === "esp-channel-convert" ||
    type === "esp-sonic" ||
    type === "esp-drc" ||
    type === "esp-mbc"
  );
}

// Resolves the right per-widget state Map for a bypass-capable type (see
// hasBypass() above) - used by the shared toggle-switch UI so it doesn't
// need a type-by-type if/else at every call site.
function bypassStateMapFor(type) {
  if (isEffectCvBypassType(type)) return effectCvStates;
  if (type === "filter" || type === "curtis-filter") return filterStates;
  if (type === "chorus") return chorusStates;
  return espEffectStates;
}

const effectCvStates = new Map(); 

function effectCvParamDef(type, portId) {
  return (EFFECT_CV_PARAMS[type] || []).find((p) => p.portId === portId);
}

function defaultEffectCvState(type) {
  const state = {};
  for (const p of EFFECT_CV_PARAMS[type] || []) {
    state[p.portId] = {
      min: p.defaultMin,
      max: p.defaultMax,
      curve: p.defaultCurve,
      volts: rangeToCv(p.defaultValue, p.defaultMin, p.defaultMax, p.defaultCurve),
    };
  }
  if (isEffectCvBypassType(type)) state.bypass = false;
  return state;
}



function effectCvParamsToValues(type, state) {
  const values = {};
  for (const p of EFFECT_CV_PARAMS[type] || []) {
    const s = state[p.portId];
    values[p.paramName] = s ? cvToRange(s.volts, s.min, s.max, s.curve) : p.defaultValue;
  }
  if (isEffectCvBypassType(type)) values.bypass = state.bypass === true;
  return values;
}

function formatEffectCvValue(type, portId, paramState) {
  const mapped = cvToRange(paramState.volts, paramState.min, paramState.max, paramState.curve);
  const unit = effectCvParamDef(type, portId)?.unit || "";
  return `${formatNumber(paramState.volts)}V → ${formatNumber(mapped)}${unit ? " " + unit : ""}`;
}

function updateEffectCvVisual(widgetId) {
  const el = document.getElementById(widgetId);
  const state = effectCvStates.get(widgetId);
  if (!el || !state) return;
  const type = el.dataset.widgetType;
  for (const p of EFFECT_CV_PARAMS[type] || []) {
    const s = state[p.portId];
    const slider = el.querySelector(`[data-role="effect-cv-${p.portId}-slider"]`);
    if (slider) slider.value = String(s.volts);
    const valueSpan = el.querySelector(`[data-role="effect-cv-${p.portId}-value"]`);
    if (valueSpan) valueSpan.textContent = formatEffectCvValue(type, p.portId, s);
  }
  if (isEffectCvBypassType(type)) {
    const toggle = el.querySelector(`[data-role="${type}-bypass-switch"]`);
    if (toggle) toggle.checked = state.bypass === true;
  }
}

function makeEffectCvEngine(type) {
  return {
    getParams: (id) => {
      const state = effectCvStates.get(id);
      if (!state) return {};
      
      
      
      
      
      
      const out = {};
      const values = effectCvParamsToValues(type, state);
      for (const p of EFFECT_CV_PARAMS[type] || []) {
        const s = state[p.portId];
        out[p.paramName] = values[p.paramName];
        out[`${p.portId}Volts`] = s.volts;
        out[`${p.portId}Min`] = s.min;
        out[`${p.portId}Max`] = s.max;
        out[`${p.portId}Curve`] = s.curve;
      }
      if (isEffectCvBypassType(type)) out.bypass = state.bypass === true;
      return out;
    },
    applyParams: (id, params) => {
      const state = effectCvStates.get(id);
      if (!state || !params) return;
      for (const p of EFFECT_CV_PARAMS[type] || []) {
        const min = params[`${p.portId}Min`] ?? p.defaultMin;
        const max = params[`${p.portId}Max`] ?? p.defaultMax;
        const curve = params[`${p.portId}Curve`] ?? p.defaultCurve;
        
        
        
        const volts = params[`${p.portId}Volts`] ?? rangeToCv(params[p.paramName] ?? p.defaultValue, min, max, curve);
        state[p.portId] = { min, max, curve, volts };
      }
      if (isEffectCvBypassType(type)) state.bypass = params.bypass === true;
      const values = effectCvParamsToValues(type, state);
      for (const p of EFFECT_CV_PARAMS[type] || []) {
        audioEngine.setParam(id, p.paramName, values[p.paramName]);
      }
      if (isEffectCvBypassType(type)) audioEngine.setParam(id, "bypass", state.bypass);
      updateEffectCvVisual(id);
    },
  };
}



const effectCvSettingsOverlay = document.getElementById("effectCvSettingsOverlay");
const effectCvSettingsTitle = document.getElementById("effectCvSettingsTitle");
const effectCvSettingsFields = document.getElementById("effectCvSettingsFields");
let pendingEffectCvId = null;
let pendingEffectCvType = null;

function openEffectCvSettings(widgetId) {
  const el = document.getElementById(widgetId);
  const state = effectCvStates.get(widgetId);
  if (!el || !state) return;
  const type = el.dataset.widgetType;
  pendingEffectCvId = widgetId;
  pendingEffectCvType = type;
  const effectCvTitleLabel = type === "delay" ? "Delay" : type === "reverb-light" ? "Reverb Light" : "Reverb";
  effectCvSettingsTitle.textContent = `${effectCvTitleLabel} Settings`;
  effectCvSettingsFields.innerHTML = "";
  for (const p of EFFECT_CV_PARAMS[type] || []) {
    const s = state[p.portId];
    const row = document.createElement("div");
    row.className = "modal-field-row";
    row.innerHTML = `
      <label class="modal-field"><span>${p.label} Min${p.unit ? ` (${p.unit})` : ""}</span><input type="text" inputmode="decimal" data-effect-cv-field="${p.portId}-min" value="${s.min}"></label>
      <label class="modal-field"><span>${p.label} Max${p.unit ? ` (${p.unit})` : ""}</span><input type="text" inputmode="decimal" data-effect-cv-field="${p.portId}-max" value="${s.max}"></label>
      <label class="modal-field"><span>${p.label} Curve</span><select data-effect-cv-field="${p.portId}-curve"><option value="linear">Linear</option><option value="log">Log</option></select></label>
    `;
    row.querySelector(`[data-effect-cv-field="${p.portId}-curve"]`).value = s.curve;
    effectCvSettingsFields.appendChild(row);
  }
  effectCvSettingsOverlay.classList.remove("hidden");
}

function closeEffectCvSettings() {
  effectCvSettingsOverlay.classList.add("hidden");
  pendingEffectCvId = null;
  pendingEffectCvType = null;
}

effectCvSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === effectCvSettingsOverlay) closeEffectCvSettings();
});

document.getElementById("effectCvSettingsCancel").addEventListener("click", closeEffectCvSettings);
document.getElementById("effectCvSettingsSave").addEventListener("click", () => {
  if (!pendingEffectCvId || !pendingEffectCvType) return;
  const state = effectCvStates.get(pendingEffectCvId);
  if (!state) return;
  for (const p of EFFECT_CV_PARAMS[pendingEffectCvType] || []) {
    const minInput = effectCvSettingsFields.querySelector(`[data-effect-cv-field="${p.portId}-min"]`);
    const maxInput = effectCvSettingsFields.querySelector(`[data-effect-cv-field="${p.portId}-max"]`);
    const curveSelect = effectCvSettingsFields.querySelector(`[data-effect-cv-field="${p.portId}-curve"]`);
    const min = parseFloat(minInput.value);
    const max = parseFloat(maxInput.value);
    state[p.portId].min = Number.isNaN(min) ? p.defaultMin : min;
    state[p.portId].max = Number.isNaN(max) ? p.defaultMax : max;
    state[p.portId].curve = curveSelect.value === "log" ? "log" : "linear";
  }
  const values = effectCvParamsToValues(pendingEffectCvType, state);
  for (const p of EFFECT_CV_PARAMS[pendingEffectCvType] || []) {
    audioEngine.setParam(pendingEffectCvId, p.paramName, values[p.paramName]);
  }
  sendLiveParamUpdate(pendingEffectCvId, values);
  updateEffectCvVisual(pendingEffectCvId);
  closeEffectCvSettings();
});



const mixerStates = new Map();
const MIXER_MIN_INPUTS = 2;
const MIXER_MAX_INPUTS = 8;

function renderMixerPorts(widgetId, portsEl) {
  const state = mixerStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  for (let i = 1; i <= state.inputCount; i += 1) {
    portsEl.appendChild(createPortRow(widgetId, { id: `in${i}`, label: `In ${i}` }, "in", "mixer"));

    const gainRow = createPortRow(widgetId, { id: `gain${i}`, label: `Gain ${i}` }, "in", "mixer");

    const slider = document.createElement("input");
    slider.type = "range";
    slider.className = "port-slider";
    slider.min = "0";
    slider.max = "2";
    slider.step = "0.01";
    slider.value = String(state.gains[i - 1]);
    slider.dataset.role = `mixer-gain-${i}-slider`;
    slider.addEventListener("mousedown", (e) => e.stopPropagation());
    slider.addEventListener("click", (e) => e.stopPropagation());
    slider.addEventListener("input", () => {
      deliverToWidget(widgetId, `gain${i}`, parseFloat(slider.value));
    });
    gainRow.appendChild(slider);

    const valueSpan = document.createElement("span");
    valueSpan.className = "port-value";
    valueSpan.dataset.role = `mixer-gain-${i}-value`;
    valueSpan.textContent = formatNumber(state.gains[i - 1]);
    gainRow.appendChild(valueSpan);

    portsEl.appendChild(gainRow);
  }
  portsEl.appendChild(createPortRow(widgetId, { id: "out", label: "Out" }, "out", "mixer"));
  layoutPortsGrid(portsEl);
}

function updateMixerGainVisual(widgetId, index) {
  const state = mixerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const slider = el.querySelector(`[data-role="mixer-gain-${index}-slider"]`);
  if (slider) slider.value = String(state.gains[index - 1]);
  const valueSpan = el.querySelector(`[data-role="mixer-gain-${index}-value"]`);
  if (valueSpan) valueSpan.textContent = formatNumber(state.gains[index - 1]);
}

const multiplexerStates = new Map();
const MULTIPLEXER_MIN_INPUTS = 2;
const MULTIPLEXER_MAX_INPUTS = 16;

// Splits 0-10V into `inputCount` equal bands, in input order - mirrors
// nodes/MultiplexerNode.h's selectFromCv() 1:1 (same "band split" idea as
// Oscillator's waveTypeFromCv, generalized to a runtime band count). 10V
// itself must still resolve to the last input, not fall off the end.
function multiplexerSelectFromCv(volts, inputCount) {
  if (inputCount <= 1) return 0;
  let idx = Math.floor(volts / (CV_MAX_VOLTS / inputCount));
  if (idx < 0) idx = 0;
  if (idx > inputCount - 1) idx = inputCount - 1;
  return idx;
}

function setLfoManualRate(widgetId, hz) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.rate = clamp(Number(hz) || 0, 0, state.maxRate);
  audioEngine.setParam(widgetId, "rate", state.rate);
  updateLfoVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { rate: state.rate });
}

function multiplexerHasCvConnection(widgetId) {
  return connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "select");
}

function setMultiplexerManualIndex(widgetId, clientY) {
  const state = multiplexerStates.get(widgetId);
  const selector = document.getElementById(widgetId)?.querySelector(".multiplexer-switch");
  if (!state || !selector || multiplexerHasCvConnection(widgetId)) return;
  const rect = selector.getBoundingClientRect();
  if (rect.height <= 0) return;
  const fraction = clamp((clientY - rect.top) / rect.height, 0, 0.999999);
  const idx = clamp(Math.floor(fraction * state.inputCount), 0, state.inputCount - 1);
  if (idx === state.manualSelect && idx === state.activeIndex) return;
  state.manualSelect = idx;
  sendLiveParamUpdateThrottled(widgetId, { select: idx });
  multiplexerSetActiveIndex(widgetId, idx);
}

function renderMultiplexerPorts(widgetId, portsEl) {
  const state = multiplexerStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  portsEl.appendChild(createPortRow(widgetId, { id: "select", label: "Select (CV)" }, "in", "multiplexer"));

  for (let i = 1; i <= state.inputCount; i += 1) {
    const row = createPortRow(widgetId, { id: `in${i}`, label: `In ${i}` }, "in", "multiplexer");
    row.classList.add("mux-in-row");
    row.dataset.muxIndex = String(i - 1);
    if (i - 1 === state.activeIndex) row.classList.add("active");
    portsEl.appendChild(row);
  }
  portsEl.appendChild(createPortRow(widgetId, { id: "value", label: "" }, "out", "multiplexer"));
  layoutPortsGrid(portsEl);
}

// Re-publishes the currently active input's live value as this widget's
// "value" output - same control-rate passthrough idea as Switch's
// publishSwitchValue(), generalized from a fixed in1/in2 pick to an N-way one.
function publishMultiplexerValue(widgetId) {
  const state = multiplexerStates.get(widgetId);
  if (!state) return;
  propagate(widgetId, "value", state.values[state.activeIndex]);
}

// Sets live-active input index from connected "select" CV, refreshes the
// highlighted port row + selector, and re-publishes output.
function multiplexerSetActiveIndex(widgetId, idx) {
  const state = multiplexerStates.get(widgetId);
  if (!state) return;
  const nextIndex = clamp(Math.round(Number(idx) || 0), 0, Math.max(0, state.inputCount - 1));
  const changed = state.activeIndex !== nextIndex;
  state.activeIndex = nextIndex;
  if (changed) audioEngine.setParam(widgetId, "select", state.activeIndex);
  updateMultiplexerVisual(widgetId);
  publishMultiplexerValue(widgetId);
}

function updateMultiplexerVisual(widgetId) {
  const state = multiplexerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const cvConnected = multiplexerHasCvConnection(widgetId);
  const selector = el.querySelector(".multiplexer-switch");
  if (selector) selector.classList.toggle("cv-connected", cvConnected);
  if (cvConnected && Number.isFinite(state.cvVolts)) {
    state.activeIndex = multiplexerSelectFromCv(state.cvVolts, state.inputCount);
  }
  el.querySelectorAll(".mux-in-row").forEach((row) => {
    row.classList.toggle("active", parseInt(row.dataset.muxIndex, 10) === state.activeIndex);
  });

  const thumb = el.querySelector('[data-role="multiplexer-switch-thumb"]');
  const firstInputRow = el.querySelector(".mux-in-row");
  if (thumb && firstInputRow) {
    const rowHeight = firstInputRow.offsetHeight || 26;
    thumb.style.transform = `translateY(${state.activeIndex * rowHeight}px)`;
  }
  updateMultiplexerRouting(widgetId);
}

function updateMultiplexerRouting(widgetId) {
  const el = document.getElementById(widgetId);
  const content = el?.querySelector(".multiplexer-content");
  const path = el?.querySelector('[data-role="multiplexer-routing-path"]');
  const activeRow = el?.querySelector(".mux-in-row.active");
  const thumb = el?.querySelector('[data-role="multiplexer-switch-thumb"]');
  const outputDot = el?.querySelector('.port-row-out .port-dot');
  if (!content || !path || !activeRow || !thumb || !outputDot) return;
  if (content.clientWidth <= 0 || content.clientHeight <= 0) {
    path.removeAttribute("d");
    return;
  }

  const contentRect = content.getBoundingClientRect();
  const scaleX = content.offsetWidth ? contentRect.width / content.offsetWidth : 1;
  const scaleY = content.offsetHeight ? contentRect.height / content.offsetHeight : 1;
  const localPoint = (rect, xFraction, yFraction) => ({
    x: (rect.left + rect.width * xFraction - contentRect.left) / scaleX,
    y: (rect.top + rect.height * yFraction - contentRect.top) / scaleY,
  });
  const start = localPoint(thumb.getBoundingClientRect(), 1, 0.5);
  const output = localPoint(outputDot.getBoundingClientRect(), 0.5, 0.5);
  const bendX = Math.max(start.x + 12, output.x - 24);
  const direction = Math.sign(output.y - start.y) || 1;
  const radius = Math.min(8, Math.abs(output.y - start.y) / 2, Math.max(0, (output.x - start.x) / 4));
  const width = content.clientWidth || 1;
  const height = content.clientHeight || 1;

  const svg = path.closest("svg");
  if (svg) {
    svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  }
  path.setAttribute(
    "d",
    `M ${start.x} ${start.y} H ${bendX - radius} ` +
      `Q ${bendX} ${start.y} ${bendX} ${start.y + direction * radius} ` +
      `V ${output.y - direction * radius} ` +
      `Q ${bendX} ${output.y} ${bendX + radius} ${output.y} H ${output.x}`
  );
}

// Hammond drawbar sliders sit on the 0-10V CV domain (like every other port
// in this engine, including Oscillator's own "gain"), but the value label
// shows the friendlier 0-8 physical-register reading - same "volts -> mapped"
// shape as formatGainCvValue() elsewhere in this file.
// NOTE: HAMMOND_DRAWBAR_STEP_V (= CV_MAX_VOLTS / 8) is declared further down,
// right after CV_MAX_VOLTS itself - CV_MAX_VOLTS isn't initialized yet at
// this point in the module's top-to-bottom evaluation, and referencing it
// here directly would throw a TDZ ReferenceError that aborts the whole
// script's initialization (this is a plain top-level const, not inside a
// function body - unlike every other CV_MAX_VOLTS use in this file, which
// is safely inside a function only called after the module finishes loading).

function formatHammondDrawbarValue(volts) {
  const reg = Math.round(clamp(volts, 0, CV_MAX_VOLTS) / HAMMOND_DRAWBAR_STEP_V);
  return `${formatNumber(volts)}V → ${reg}`;
}

// Dedicated renderer (not the generic def.inputs.forEach loop) so the 4
// shared CV rows and the 9 drawbar rows all live in one readable place -
// mirrors renderMixerPorts' shape just above, but with a FIXED row count
// (no settings-modal input-count field needed, unlike Mixer).
function renderHammondPorts(widgetId, portsEl) {
  const state = hammondStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  WIDGET_TYPES.hammond.inputs
    .filter((port) => !port.id.startsWith("drawbar"))
    .forEach((port) => {
      const row = createPortRow(widgetId, port, "in", "hammond");
      if (port.id === "tune" || port.id === "gain") {
        const value = port.id === "tune" ? state.tuneVolts : state.gain;
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(value);
        slider.dataset.role = `hammond-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(widgetId, port.id, parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `hammond-${port.id}-value`;
        valueSpan.textContent = formatNumber(value);
        row.appendChild(valueSpan);

        if (port.id === "tune") {
          // Same double-click-to-recenter affordance as Oscillator's Tune row.
          const resetTune = () => {
            if (connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "tune")) return;
            deliverToWidget(widgetId, "tune", CV_MAX_VOLTS / 2);
          };
          const tuneLabel = row.querySelector(".port-label");
          if (tuneLabel) {
            tuneLabel.addEventListener("dblclick", (e) => {
              e.stopPropagation();
              resetTune();
            });
          }
          valueSpan.addEventListener("dblclick", (e) => {
            e.stopPropagation();
            resetTune();
          });
        }
      }
      portsEl.appendChild(row);
    });

  for (let k = 1; k <= 9; k += 1) {
    const portId = `drawbar${k}`;
    const row = createPortRow(widgetId, { id: portId, label: HAMMOND_FOOTAGES[k - 1] }, "in", "hammond");
    row.classList.add("hammond-drawbar-row");

    const slider = document.createElement("input");
    slider.type = "range";
    slider.className = "port-slider";
    slider.min = "0";
    slider.max = String(CV_MAX_VOLTS);
    slider.step = String(HAMMOND_DRAWBAR_STEP_V);
    slider.value = String(state.drawbars[k - 1]);
    slider.dataset.role = `hammond-drawbar-${k}-slider`;
    slider.addEventListener("mousedown", (e) => e.stopPropagation());
    slider.addEventListener("click", (e) => e.stopPropagation());
    slider.addEventListener("input", () => {
      deliverToWidget(widgetId, portId, parseFloat(slider.value));
    });
    row.appendChild(slider);

    const valueSpan = document.createElement("span");
    valueSpan.className = "port-value";
    valueSpan.dataset.role = `hammond-drawbar-${k}-value`;
    valueSpan.textContent = formatHammondDrawbarValue(state.drawbars[k - 1]);
    row.appendChild(valueSpan);

    portsEl.appendChild(row);
  }

  portsEl.appendChild(createPortRow(widgetId, { id: "value", label: "Value" }, "out", "hammond"));
  layoutPortsGrid(portsEl);
}

function recomputeHammondFreq(widgetId, voiceIndex) {
  const state = hammondStates.get(widgetId);
  if (!state) return;
  const center = CV_MAX_VOLTS / 2;
  const hz = hzFromPitchCv(state.pitchCv + (state.tuneVolts - center));
  state.freq = hz;
  if (voiceIndex !== undefined) {
    audioEngine.setVoiceParam(widgetId, voiceIndex, "frequency", hz, 0);
    audioEngine.setVoiceParam(widgetId, voiceIndex, "tuneVolts", state.tuneVolts, 0);
  } else {
    audioEngine.setParam(widgetId, "frequency", hz, 0);
    audioEngine.setParam(widgetId, "tuneVolts", state.tuneVolts, 0);
  }
  const el = document.getElementById(widgetId);
  const label = el ? el.querySelector('[data-role="hammond-freq-label"]') : null;
  if (label) label.textContent = `${formatNumber(hz)} Hz`;
}

function updateHammondDrawbarVisual(widgetId, index) {
  const state = hammondStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const portId = `drawbar${index}`;
  // Same predicate as updateOscillatorGainVisual: only a live audio-rate
  // cable disables the slider, a control-rate/no connection leaves it live.
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === portId &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = el.querySelector(`[data-role="hammond-drawbar-${index}-slider"]`);
  if (slider) {
    slider.value = String(state.drawbars[index - 1]);
    slider.disabled = audioConnected;
  }
  const valueSpan = el.querySelector(`[data-role="hammond-drawbar-${index}-value"]`);
  if (valueSpan) valueSpan.textContent = formatHammondDrawbarValue(state.drawbars[index - 1]);
}

function updateHammondScalarVisual(widgetId, portId, value) {
  const el = document.getElementById(widgetId);
  if (!el) return;
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === portId &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = el.querySelector(`[data-role="hammond-${portId}-slider"]`);
  if (slider) {
    slider.value = String(value);
    slider.disabled = audioConnected;
  }
  const valueSpan = el.querySelector(`[data-role="hammond-${portId}-value"]`);
  if (valueSpan) valueSpan.textContent = formatNumber(value);
}

// One resync for every control on the node - called after applyParams/
// preset load and from refreshAllDualModeWidgets on every connect/disconnect
// (the row COUNT is fixed here, unlike Mixer, so this never needs a
// full re-render, just value/disabled updates).
function updateHammondVisual(widgetId) {
  const state = hammondStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  updateHammondScalarVisual(widgetId, "tune", state.tuneVolts);
  updateHammondScalarVisual(widgetId, "gain", state.gain);
  for (let k = 1; k <= 9; k += 1) updateHammondDrawbarVisual(widgetId, k);
  const label = el.querySelector('[data-role="hammond-freq-label"]');
  if (label) label.textContent = `${formatNumber(state.freq)} Hz`;
}

// Settings dialog: per-drawbar harmonic ratio (independent of its CV level
// above) + a "Default" button restoring the classic Hammond footages.
const hammondSettingsOverlay = document.getElementById("hammondSettingsOverlay");
const hammondRatioFieldsEl = document.getElementById("hammondRatioFields");
const hammondModulationStrideSelect = document.getElementById("hammondModulationStrideSelect");
let pendingHammondId = null;

function openHammondSettings(widgetId) {
  const state = hammondStates.get(widgetId);
  if (!state) return;
  pendingHammondId = widgetId;
  hammondModulationStrideSelect.value = String(state.modulationStride ?? 4);
  hammondRatioFieldsEl.innerHTML = "";
  HAMMOND_FOOTAGES.forEach((label, k) => {
    const field = document.createElement("label");
    field.className = "modal-field";
    const span = document.createElement("span");
    span.textContent = label;
    const input = document.createElement("input");
    input.type = "number";
    input.step = "0.01";
    input.min = "0.01";
    input.max = "32";
    input.id = `hammondRatioInput${k + 1}`;
    input.value = String(state.ratios[k]);
    field.appendChild(span);
    field.appendChild(input);
    hammondRatioFieldsEl.appendChild(field);
  });
  hammondSettingsOverlay.classList.remove("hidden");
}

function closeHammondSettings() {
  hammondSettingsOverlay.classList.add("hidden");
  pendingHammondId = null;
}

hammondSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === hammondSettingsOverlay) closeHammondSettings();
});

document.getElementById("hammondSettingsCancel").addEventListener("click", closeHammondSettings);

// Resets the FORM fields only - applying/persisting still requires Save,
// same as every other field in this dialog (Cancel can still back out of
// the reset itself).
document.getElementById("hammondSettingsReset").addEventListener("click", () => {
  if (!confirm("Reset all drawbar ratios to the default Hammond registration?")) return;
  HAMMOND_DEFAULT_RATIOS.forEach((v, k) => {
    const input = document.getElementById(`hammondRatioInput${k + 1}`);
    if (input) input.value = String(v);
  });
});

document.getElementById("hammondSettingsSave").addEventListener("click", () => {
  if (!pendingHammondId) return;
  const state = hammondStates.get(pendingHammondId);
  if (state) {
    const modulationStride = parseInt(hammondModulationStrideSelect.value, 10);
    state.modulationStride = [1, 2, 4, 8, 16].includes(modulationStride) ? modulationStride : 4;
    audioEngine.setParam(pendingHammondId, "modulationStride", state.modulationStride);
    sendLiveParamUpdateThrottled(pendingHammondId, { modulationStride: state.modulationStride });
    for (let k = 0; k < 9; k += 1) {
      const input = document.getElementById(`hammondRatioInput${k + 1}`);
      const num = input ? parseFloat(input.value) : NaN;
      state.ratios[k] = Number.isNaN(num) ? HAMMOND_DEFAULT_RATIOS[k] : clamp(num, 0.01, 32);
      audioEngine.setParam(pendingHammondId, `ratio${k + 1}`, state.ratios[k]);
      sendLiveParamUpdateThrottled(pendingHammondId, { [`ratio${k + 1}`]: state.ratios[k] });
    }
    savePresetNow();
  }
  closeHammondSettings();
});

const mixerSettingsOverlay = document.getElementById("mixerSettingsOverlay");
const mixerCountInput = document.getElementById("mixerCountInput");
let pendingMixerId = null;

function openMixerSettings(widgetId) {
  const state = mixerStates.get(widgetId);
  if (!state) return;
  pendingMixerId = widgetId;
  mixerCountInput.value = state.inputCount;
  mixerSettingsOverlay.classList.remove("hidden");
}

function closeMixerSettings() {
  mixerSettingsOverlay.classList.add("hidden");
  pendingMixerId = null;
}

mixerSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === mixerSettingsOverlay) closeMixerSettings();
});

document.getElementById("mixerSettingsCancel").addEventListener("click", closeMixerSettings);

document.getElementById("mixerSettingsSave").addEventListener("click", () => {
  if (!pendingMixerId) return;
  const state = mixerStates.get(pendingMixerId);
  if (state) {
    let count = parseInt(mixerCountInput.value, 10);
    if (Number.isNaN(count)) count = MIXER_MIN_INPUTS;
    count = clamp(count, MIXER_MIN_INPUTS, MIXER_MAX_INPUTS);

    if (count < state.inputCount) {
      for (let i = count + 1; i <= state.inputCount; i += 1) {
        removeConnectionsForPort(pendingMixerId, `in${i}`, "in");
        removeConnectionsForPort(pendingMixerId, `gain${i}`, "in");
      }
    }
    state.inputCount = count;

    const el = document.getElementById(pendingMixerId);
    const portsEl = el ? el.querySelector(".widget-ports") : null;
    if (portsEl) renderMixerPorts(pendingMixerId, portsEl);

    drawConnections();
    savePresetNow();
  }
  closeMixerSettings();
});

const multiplexerSettingsOverlay = document.getElementById("multiplexerSettingsOverlay");
const multiplexerCountInput = document.getElementById("multiplexerCountInput");
const multiplexerSoftFadeInput = document.getElementById("multiplexerSoftFadeInput");
const MULTIPLEXER_DEFAULT_SOFT_FADE_MS = 5;
const MULTIPLEXER_MAX_SOFT_FADE_MS = 100;
let pendingMultiplexerId = null;

function openMultiplexerSettings(widgetId) {
  const state = multiplexerStates.get(widgetId);
  if (!state) return;
  pendingMultiplexerId = widgetId;
  multiplexerCountInput.value = state.inputCount;
  multiplexerSoftFadeInput.value = state.softFadeMs;
  multiplexerSettingsOverlay.classList.remove("hidden");
}

function closeMultiplexerSettings() {
  multiplexerSettingsOverlay.classList.add("hidden");
  pendingMultiplexerId = null;
}

multiplexerSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === multiplexerSettingsOverlay) closeMultiplexerSettings();
});

document.getElementById("multiplexerSettingsCancel").addEventListener("click", closeMultiplexerSettings);

document.getElementById("multiplexerSettingsSave").addEventListener("click", () => {
  if (!pendingMultiplexerId) return;
  const state = multiplexerStates.get(pendingMultiplexerId);
  if (state) {
    let count = parseInt(multiplexerCountInput.value, 10);
    if (Number.isNaN(count)) count = MULTIPLEXER_MIN_INPUTS;
    count = clamp(count, MULTIPLEXER_MIN_INPUTS, MULTIPLEXER_MAX_INPUTS);
    let softFadeMs = parseFloat(multiplexerSoftFadeInput.value);
    if (Number.isNaN(softFadeMs)) softFadeMs = MULTIPLEXER_DEFAULT_SOFT_FADE_MS;
    softFadeMs = clamp(softFadeMs, 0, MULTIPLEXER_MAX_SOFT_FADE_MS);

    if (count < state.inputCount) {
      for (let i = count + 1; i <= state.inputCount; i += 1) {
        removeConnectionsForPort(pendingMultiplexerId, `in${i}`, "in");
      }
    }
    state.inputCount = count;
    state.softFadeMs = softFadeMs;
    if (state.manualSelect > count - 1) state.manualSelect = count - 1;
    audioEngine.setParam(pendingMultiplexerId, "softFadeMs", softFadeMs);
    sendLiveParamUpdate(pendingMultiplexerId, { softFadeMs });

    const el = document.getElementById(pendingMultiplexerId);
    const portsEl = el ? el.querySelector(".widget-ports") : null;
    if (portsEl) renderMultiplexerPorts(pendingMultiplexerId, portsEl);

    multiplexerSetActiveIndex(pendingMultiplexerId, connections.some((c) => c.to.widgetId === pendingMultiplexerId && c.to.portId === "select")
      ? state.activeIndex
      : state.manualSelect);
    drawConnections();
    savePresetNow();
  }
  closeMultiplexerSettings();
});



const adsrStates = new Map();
const MAX_ENVELOPE_SECONDS = 4;



function adsrSliderVolts(state, portId) {
  if (portId === "sus") return state.sustain * CV_MAX_VOLTS;
  const paramName = { att: "attack", dec: "decay", rel: "release" }[portId];
  return secondsToCv(state[paramName], state[`${paramName}MaxSeconds`], state[`${paramName}Curve`]);
}




function formatAdsrValue(state, portId) {
  if (portId === "sus") return formatNumber(state.sustain);
  const paramName = { att: "attack", dec: "decay", rel: "release" }[portId];
  return `${formatNumber(state[paramName])} s`;
}




function updateAdsrSliders(widgetId) {
  const state = adsrStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  ["att", "dec", "sus", "rel"].forEach((portId) => {
    const slider = el.querySelector(`[data-role="adsr-slider-${portId}"]`);
    if (slider) slider.value = String(adsrSliderVolts(state, portId));
    const valueSpan = el.querySelector(`[data-role="adsr-${portId}-value"]`);
    if (valueSpan) valueSpan.textContent = formatAdsrValue(state, portId);
  });
}



const adsrSettingsOverlay = document.getElementById("adsrSettingsOverlay");
const adsrAttackMaxInput = document.getElementById("adsrAttackMaxInput");
const adsrAttackCurveSelect = document.getElementById("adsrAttackCurveSelect");
const adsrDecayMaxInput = document.getElementById("adsrDecayMaxInput");
const adsrDecayCurveSelect = document.getElementById("adsrDecayCurveSelect");
const adsrReleaseMaxInput = document.getElementById("adsrReleaseMaxInput");
const adsrReleaseCurveSelect = document.getElementById("adsrReleaseCurveSelect");
let pendingAdsrId = null;

function openAdsrSettings(widgetId) {
  const state = adsrStates.get(widgetId);
  if (!state) return;
  pendingAdsrId = widgetId;
  adsrAttackMaxInput.value = state.attackMaxSeconds;
  adsrAttackCurveSelect.value = state.attackCurve;
  adsrDecayMaxInput.value = state.decayMaxSeconds;
  adsrDecayCurveSelect.value = state.decayCurve;
  adsrReleaseMaxInput.value = state.releaseMaxSeconds;
  adsrReleaseCurveSelect.value = state.releaseCurve;
  adsrSettingsOverlay.classList.remove("hidden");
}

function closeAdsrSettings() {
  adsrSettingsOverlay.classList.add("hidden");
  pendingAdsrId = null;
}

adsrSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === adsrSettingsOverlay) closeAdsrSettings();
});

document.getElementById("adsrSettingsCancel").addEventListener("click", closeAdsrSettings);

document.getElementById("adsrSettingsSave").addEventListener("click", () => {
  if (!pendingAdsrId) return;
  const state = adsrStates.get(pendingAdsrId);
  if (state) {
    
    
    
    const attVolts = adsrSliderVolts(state, "att");
    const decVolts = adsrSliderVolts(state, "dec");
    const relVolts = adsrSliderVolts(state, "rel");

    const attackMax = parseFloatFlexible(adsrAttackMaxInput.value);
    const decayMax = parseFloatFlexible(adsrDecayMaxInput.value);
    const releaseMax = parseFloatFlexible(adsrReleaseMaxInput.value);
    state.attackMaxSeconds = Number.isNaN(attackMax) || attackMax <= 0 ? MAX_ENVELOPE_SECONDS : attackMax;
    state.decayMaxSeconds = Number.isNaN(decayMax) || decayMax <= 0 ? MAX_ENVELOPE_SECONDS : decayMax;
    state.releaseMaxSeconds = Number.isNaN(releaseMax) || releaseMax <= 0 ? MAX_ENVELOPE_SECONDS : releaseMax;
    state.attackCurve = adsrAttackCurveSelect.value === "log" ? "log" : "linear";
    state.decayCurve = adsrDecayCurveSelect.value === "log" ? "log" : "linear";
    state.releaseCurve = adsrReleaseCurveSelect.value === "log" ? "log" : "linear";

    deliverToWidget(pendingAdsrId, "att", attVolts);
    deliverToWidget(pendingAdsrId, "dec", decVolts);
    deliverToWidget(pendingAdsrId, "rel", relVolts);
    updateAdsrSliders(pendingAdsrId);
  }
  closeAdsrSettings();
});



// Generic double-click-to-edit popup for ANY .port-slider (every numeric
// range input across all widget types - ADSR times, LFO rate, filter
// cutoff/resonance, mixer gains, midi-in voices/glideTime, etc.). Rather
// than wiring a bespoke popup per field, this reuses each slider's own
// existing "input" listener: it sets slider.value then dispatches a real
// "input" event, so whatever field-specific setter/clamping/side-effect
// logic already runs on drag runs unchanged here too - zero duplication,
// and new sliders added later get this for free with no extra wiring.
const numericEditOverlay = document.getElementById("numericEditOverlay");
const numericEditLabel = document.getElementById("numericEditLabel");
const numericEditInput = document.getElementById("numericEditInput");
const numericEditUnit = document.getElementById("numericEditUnit");
let pendingNumericEditSlider = null;

function findSliderForNumericEdit(target) {
  const slider = target.closest('input[type="range"].port-slider');
  if (slider) return slider;
  const valueEl = target.closest(".port-value");
  if (!valueEl || !valueEl.parentElement) return null;
  return valueEl.parentElement.querySelector('input[type="range"].port-slider');
}

// Splits e.g. "0.40 s" -> unit "s", "10.0 V" -> "V", "0.7" (unitless) -> "".
function unitFromValueText(text) {
  const match = /^-?[\d.,]+\s*(.*)$/.exec((text || "").trim());
  return match ? match[1].trim() : "";
}

function openNumericEditPopup(slider) {
  pendingNumericEditSlider = slider;
  const row = slider.closest(".port-row, .audio-out-volume-row") || slider.parentElement;
  const label = row ? row.querySelector(".port-label") : null;
  numericEditLabel.textContent = label ? label.textContent : "Value";
  const valueEl = row ? row.querySelector(".port-value") : null;
  numericEditUnit.textContent = valueEl ? unitFromValueText(valueEl.textContent) : "";
  numericEditInput.value = slider.value;
  numericEditOverlay.classList.remove("hidden");
  numericEditInput.focus();
  numericEditInput.select();
}

function closeNumericEditPopup() {
  numericEditOverlay.classList.add("hidden");
  pendingNumericEditSlider = null;
}

function saveNumericEditPopup() {
  const slider = pendingNumericEditSlider;
  if (!slider) return;
  const parsed = parseFloatFlexible(numericEditInput.value);
  if (!Number.isNaN(parsed)) {
    const min = slider.min !== "" ? Number(slider.min) : -Infinity;
    const max = slider.max !== "" ? Number(slider.max) : Infinity;
    slider.value = String(clamp(parsed, min, max));
    slider.dispatchEvent(new Event("input", { bubbles: true }));
  }
  closeNumericEditPopup();
}

// Capture phase, not bubble: every widget-instance element has its own
// dblclick listener (opens that widget's settings dialog) that calls
// e.stopPropagation() - in the bubble phase that fires before a
// document-level listener ever sees the event. Capture runs top-down
// (document first), so this sees it first and can stop it from reaching
// that handler when it's actually a slider/value double-click.
document.addEventListener(
  "dblclick",
  (e) => {
    const slider = findSliderForNumericEdit(e.target);
    if (!slider) return;
    e.stopPropagation();
    openNumericEditPopup(slider);
  },
  true
);

numericEditOverlay.addEventListener("click", (e) => {
  if (e.target === numericEditOverlay) closeNumericEditPopup();
});
document.getElementById("numericEditCancel").addEventListener("click", closeNumericEditPopup);
document.getElementById("numericEditSave").addEventListener("click", saveNumericEditPopup);
numericEditInput.addEventListener("keydown", (e) => {
  if (e.key === "Enter") saveNumericEditPopup();
  else if (e.key === "Escape") closeNumericEditPopup();
});

const widgetPickerOverlay = document.getElementById("widgetPickerOverlay");
const widgetPickerList = document.getElementById("widgetPickerList");
let pendingWidgetPos = null;

function openWidgetPicker(x, y, clientX, clientY) {
  pendingWidgetPos = { x, y };
  widgetPickerList.innerHTML = "";
  const allowed = pages[currentPageName].allowedTypes;

  WIDGET_CATEGORIES.forEach((cat) => {
    const typesHere = cat.types.filter((t) => allowed.includes(t));
    if (typesHere.length === 0) return; 

    const column = document.createElement("div");
    column.className = "widget-picker-column";

    const heading = document.createElement("div");
    heading.className = "widget-picker-category";
    heading.textContent = cat.label;
    column.appendChild(heading);

    typesHere.forEach((type) => {
      const def = WIDGET_TYPES[type];
      const item = document.createElement("div");
      item.className = "widget-source";
      item.textContent = def.icon ? `${def.icon} ${def.label}` : def.label;
      const outputAlreadyPlaced = OUTPUT_SINGLETON_TYPES.includes(type) &&
        pages.fx.canvasContentEl.querySelector(`.widget-instance[data-widget-type="${type}"]`);
      if (outputAlreadyPlaced) {
        item.classList.add("disabled");
        item.setAttribute("aria-disabled", "true");
        item.title = `${def.label} is already placed`;
      } else {
        item.addEventListener("click", () => {
          pushHistorySnapshot();
          createWidget(type, def.label, pendingWidgetPos.x, pendingWidgetPos.y);
          closeWidgetPicker();
        });
      }
      column.appendChild(item);
    });

    widgetPickerList.appendChild(column);
  });

  const picker = widgetPickerOverlay.querySelector(".widget-picker");
  picker.style.left = "0px";
  picker.style.top = "0px";
  widgetPickerOverlay.classList.remove("hidden");
  const rect = picker.getBoundingClientRect();
  const margin = 8;
  picker.style.left = `${clamp(clientX, margin, window.innerWidth - rect.width - margin)}px`;
  picker.style.top = `${clamp(clientY, margin, window.innerHeight - rect.height - margin)}px`;
}

function closeWidgetPicker() {
  widgetPickerOverlay.classList.add("hidden");
  pendingWidgetPos = null;
}

document.getElementById("widgetPickerClose").addEventListener("click", closeWidgetPicker);

widgetPickerOverlay.addEventListener("contextmenu", (e) => {
  e.preventDefault();
});



let canvasDragState = null;
const CANVAS_DRAG_THRESHOLD = 4;





function handleCanvasMousedown(e) {
  if (e.target.closest(".widget-instance") || e.target.closest(".connection-path")) return;
  if (e.button !== 0 && e.button !== 1) return;
  e.preventDefault();

  canvasDragState = {
    mode: e.button === 1 ? "pan" : "lasso",
    startX: e.clientX,
    startY: e.clientY,
    originPanX: panX,
    originPanY: panY,
    moved: false,
  };

  if (canvasDragState.mode === "pan") {
    canvas.classList.add("panning");
  }

  document.addEventListener("mousemove", onCanvasDragMove);
  document.addEventListener("mouseup", onCanvasDragUp);
}

pages.generator.canvasEl.addEventListener("mousedown", handleCanvasMousedown);
pages.fx.canvasEl.addEventListener("mousedown", handleCanvasMousedown);

function handleCanvasContextMenu(e) {
  if (e.target.closest(".widget-instance") || e.target.closest(".connection-path")) return;
  e.preventDefault();
  clearSelection();
  const content = clientToContent(e.clientX, e.clientY);
  openWidgetPicker(snap(content.x), snap(content.y), e.clientX, e.clientY);
}

pages.generator.canvasEl.addEventListener("contextmenu", handleCanvasContextMenu);
pages.fx.canvasEl.addEventListener("contextmenu", handleCanvasContextMenu);

function onCanvasDragMove(e) {
  if (!canvasDragState) return;
  const dx = e.clientX - canvasDragState.startX;
  const dy = e.clientY - canvasDragState.startY;

  if (!canvasDragState.moved && Math.hypot(dx, dy) > CANVAS_DRAG_THRESHOLD) {
    canvasDragState.moved = true;
    if (canvasDragState.mode === "lasso") lassoBox.classList.remove("hidden");
  }
  if (!canvasDragState.moved) return;

  if (canvasDragState.mode === "pan") {
    panX = canvasDragState.originPanX + dx;
    panY = canvasDragState.originPanY + dy;
    applyTransform();
  } else {
    const rect = canvas.getBoundingClientRect();
    const x1 = canvasDragState.startX - rect.left;
    const y1 = canvasDragState.startY - rect.top;
    const x2 = e.clientX - rect.left;
    const y2 = e.clientY - rect.top;
    lassoBox.style.left = `${Math.min(x1, x2)}px`;
    lassoBox.style.top = `${Math.min(y1, y2)}px`;
    lassoBox.style.width = `${Math.abs(x2 - x1)}px`;
    lassoBox.style.height = `${Math.abs(y2 - y1)}px`;
  }
}

function onCanvasDragUp(e) {
  document.removeEventListener("mousemove", onCanvasDragMove);
  document.removeEventListener("mouseup", onCanvasDragUp);
  if (!canvasDragState) return;

  if (canvasDragState.mode === "pan") {
    canvas.classList.remove("panning");
  } else if (canvasDragState.moved) {
    const lassoRect = lassoBox.getBoundingClientRect();
    const newSelection = new Set();
    canvasContent.querySelectorAll(".widget-instance").forEach((w) => {
      const wr = w.getBoundingClientRect();
      const intersects = !(
        wr.right < lassoRect.left ||
        wr.left > lassoRect.right ||
        wr.bottom < lassoRect.top ||
        wr.top > lassoRect.bottom
      );
      if (intersects) newSelection.add(w.id);
    });
    selectedWidgetIds = newSelection;
    updateSelectionClasses();
    lassoBox.classList.add("hidden");
    lassoBox.style.width = "0px";
    lassoBox.style.height = "0px";
  } else {
    clearSelection();
  }

  canvasDragState = null;
}



function handleCanvasWheel(e) {
  e.preventDefault();
  const rect = canvas.getBoundingClientRect();
  const mouseX = e.clientX - rect.left;
  const mouseY = e.clientY - rect.top;
  const oldZoom = zoom;
  const direction = e.deltaY < 0 ? 1 : -1;
  const newZoom = clamp(
    Math.round((zoom + direction * ZOOM_STEP) * 100) / 100,
    ZOOM_MIN,
    ZOOM_MAX
  );
  if (newZoom === oldZoom) return;

  const contentX = (mouseX - panX) / oldZoom;
  const contentY = (mouseY - panY) / oldZoom;
  panX = mouseX - contentX * newZoom;
  panY = mouseY - contentY * newZoom;
  zoom = newZoom;
  applyTransform();
}

pages.generator.canvasEl.addEventListener("wheel", handleCanvasWheel, { passive: false });
pages.fx.canvasEl.addEventListener("wheel", handleCanvasWheel, { passive: false });

zoomIndicator.addEventListener("click", () => {
  zoom = 1;
  applyTransform();
});

applyTransform();



let undoStack = [];
let redoStack = [];
let suppressHistory = false;
const HISTORY_LIMIT = 60;

function pushHistorySnapshot() {
  if (suppressHistory) return;
  undoStack.push(serializePreset());
  if (undoStack.length > HISTORY_LIMIT) undoStack.shift();
  redoStack = [];
}

async function undoLastAction() {
  if (undoStack.length === 0 || suppressHistory) return;
  const snapshot = undoStack.pop();
  redoStack.push(serializePreset());
  suppressHistory = true;
  try {
    await applyPresetData(snapshot);
  } finally {
    suppressHistory = false;
  }
  savePresetNow();
}

async function redoLastAction() {
  if (redoStack.length === 0 || suppressHistory) return;
  const snapshot = redoStack.pop();
  undoStack.push(serializePreset());
  suppressHistory = true;
  try {
    await applyPresetData(snapshot);
  } finally {
    suppressHistory = false;
  }
  savePresetNow();
}



let pendingPaste = null; 




let lastMouseContent = { x: 0, y: 0 };
document.addEventListener("mousemove", (e) => {
  lastMouseContent = clientToContent(e.clientX, e.clientY);
});

function cancelPendingPaste() {
  if (!pendingPaste) return;
  pendingPaste.ghosts.forEach((g) => g.el.remove());
  document.removeEventListener("mousemove", pendingPaste.onMove);
  document.removeEventListener("click", pendingPaste.onClick, true);
  pendingPaste = null;
}

function startPendingPaste() {
  if (clipboard.widgets.length === 0) return;
  cancelPendingPaste();

  const minX = Math.min(...clipboard.widgets.map((it) => it.x));
  const minY = Math.min(...clipboard.widgets.map((it) => it.y));

  const ghosts = clipboard.widgets.map((item) => {
    const ghost = document.createElement("div");
    ghost.className = "widget-paste-ghost";
    ghost.textContent = item.label;
    canvasContent.appendChild(ghost);
    return { el: ghost, dx: item.x - minX, dy: item.y - minY, item };
  });

  function positionGhosts(pos) {
    ghosts.forEach((g) => {
      g.el.style.left = `${pos.x + g.dx}px`;
      g.el.style.top = `${pos.y + g.dy}px`;
    });
  }
  positionGhosts(lastMouseContent);

  function onMove(ev) {
    positionGhosts(clientToContent(ev.clientX, ev.clientY));
  }

  function onClick(ev) {
    ev.preventDefault();
    ev.stopPropagation();
    confirmPendingPaste(clientToContent(ev.clientX, ev.clientY), minX, minY);
  }

  document.addEventListener("mousemove", onMove);
  
  
  document.addEventListener("click", onClick, true);
  pendingPaste = { ghosts, onMove, onClick };
}

function confirmPendingPaste(pos, minX, minY) {
  if (!pendingPaste) return;
  const ghosts = pendingPaste.ghosts;
  cancelPendingPaste();

  pushHistorySnapshot();

  const idMap = new Map();
  const newIds = ghosts.map(({ item, dx, dy }) => {
    const widget = createWidget(item.type, item.label, snap(pos.x + dx), snap(pos.y + dy));
    if (item.params) applyWidgetParams(item.type, widget.id, item.params);
    idMap.set(item.id, widget.id);
    return widget.id;
  });

  clipboard.connections.forEach((c) => {
    const fromId = idMap.get(c.from.widgetId);
    const toId = idMap.get(c.to.widgetId);
    if (!fromId || !toId) return;
    if (connectionExists(fromId, c.from.portId, toId, c.to.portId)) return;
    connectionCounter += 1;
    connections.push({
      id: `conn-${connectionCounter}`,
      from: { widgetId: fromId, portId: c.from.portId },
      to: { widgetId: toId, portId: c.to.portId },
      color: getConnectionColorForOutput(fromId, c.from.portId),
    });
    const fromKind = getPortKind(fromId, c.from.portId, "out");
    const toKind = getPortKind(toId, c.to.portId, "in");
    if (fromKind === "audio" && toKind === "audio") {
      audioEngine.connect(fromId, c.from.portId, toId, c.to.portId);
    }
  });

  selectedWidgetIds = new Set(newIds);
  updateSelectionClasses();
  refreshControlGraph();
  refreshVoiceGroups();
  drawConnections();
  savePresetNow();
}



document.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") {
    e.preventDefault();
    handleExplicitSave();
    return;
  }

  if (e.key === "Escape" && pendingPaste) {
    e.preventDefault();
    cancelPendingPaste();
    return;
  }

  const activeElement = document.activeElement;
  const tag = activeElement ? activeElement.tagName : "";
  if (activeElement?.isContentEditable || activeElement?.closest?.('[contenteditable="true"]')) return;
  if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;

  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "z" && e.shiftKey) {
    e.preventDefault();
    redoLastAction();
  } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "z") {
    e.preventDefault();
    undoLastAction();
  } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "y") {
    e.preventDefault();
    redoLastAction();
  } else if ((e.key === "Delete" || e.key === "Backspace") && selectedWidgetIds.size > 0) {
    e.preventDefault();
    pushHistorySnapshot();
    Array.from(selectedWidgetIds).forEach(removeWidget);
    selectedWidgetIds.clear();
    drawConnections();
    savePresetNow(); 
  } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "c" && selectedWidgetIds.size > 0) {
    const copiedIds = new Set(selectedWidgetIds);
    clipboard = {
      widgets: [...copiedIds]
        .map((id) => document.getElementById(id))
        .filter(Boolean)
        .map((el) => ({
          id: el.id,
          type: el.dataset.widgetType,
          label: el.querySelector(".widget-instance-label")?.textContent ||
            (WIDGET_TYPES[el.dataset.widgetType] || {}).label || el.dataset.widgetType,
          x: parseInt(el.style.left, 10) || 0,
          y: parseInt(el.style.top, 10) || 0,
          ...(el.dataset.widgetType === "annotation"
            ? { params: JSON.parse(JSON.stringify(getWidgetParams(el.dataset.widgetType, el.id))) }
            : {}),
        })),
      
      
      
      connections: connections.filter((c) => copiedIds.has(c.from.widgetId) && copiedIds.has(c.to.widgetId)),
    };
  } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "v" && clipboard.widgets.length > 0) {
    startPendingPaste();
  }
});



function startWidgetDrag(e, widgetEl) {
  if (e.target.closest("button") || e.target.closest(".port-dot")) return;
  e.preventDefault();

  
  
  
  const isGroupDrag = selectedWidgetIds.has(widgetEl.id) && selectedWidgetIds.size > 1;
  const dragIds = isGroupDrag ? [...selectedWidgetIds] : [widgetEl.id];
  const dragTargets = dragIds
    .map((id) => document.getElementById(id))
    .filter(Boolean)
    .map((el) => ({ el, startLeft: el.offsetLeft, startTop: el.offsetTop, landingX: 0, landingY: 0, preview: null }));

  
  
  
  
  const preDragSnapshot = serializePreset();

  const startX = e.clientX;
  const startY = e.clientY;

  dragTargets.forEach((t) => {
    t.landingX = snap(t.startLeft);
    t.landingY = snap(t.startTop);
    const preview = document.createElement("div");
    preview.className = "widget-drop-preview";
    preview.style.width = `${t.el.offsetWidth}px`;
    preview.style.height = `${t.el.offsetHeight}px`;
    preview.style.left = `${t.landingX}px`;
    preview.style.top = `${t.landingY}px`;
    canvasContent.appendChild(preview);
    t.preview = preview;
    t.el.classList.add("dragging");
  });

  let moved = false;

  function onMove(ev) {
    moved = true;
    const dx = (ev.clientX - startX) / zoom;
    const dy = (ev.clientY - startY) / zoom;
    dragTargets.forEach((t) => {
      const rawX = t.startLeft + dx;
      const rawY = t.startTop + dy;
      t.el.style.left = `${rawX}px`;
      t.el.style.top = `${rawY}px`;

      t.landingX = snap(rawX);
      t.landingY = snap(rawY);
      t.preview.style.left = `${t.landingX}px`;
      t.preview.style.top = `${t.landingY}px`;
    });

    drawConnections();
  }

  function onUp() {
    document.removeEventListener("mousemove", onMove);
    document.removeEventListener("mouseup", onUp);
    dragTargets.forEach((t) => {
      t.el.classList.remove("dragging");
      t.el.style.left = `${t.landingX}px`;
      t.el.style.top = `${t.landingY}px`;
      t.preview.remove();
    });
    drawConnections();
    if (moved) {
      if (!suppressHistory) {
        undoStack.push(preDragSnapshot);
        if (undoStack.length > HISTORY_LIMIT) undoStack.shift();
        redoStack = [];
      }
      dragTargets.forEach((t) => {
        t.el.dataset.justDragged = "1";
        setTimeout(() => {
          delete t.el.dataset.justDragged;
        }, 0);
      });
      markDirty();
    }
  }

  document.addEventListener("mousemove", onMove);
  document.addEventListener("mouseup", onUp);
}



let connectionsLayer = pages.generator.connectionsLayerEl;


let connections = [];
let connectionCounter = 0;

function connectionExists(fromWidgetId, fromPortId, toWidgetId, toPortId) {
  return connections.some(
    (c) =>
      c.from.widgetId === fromWidgetId &&
      c.from.portId === fromPortId &&
      c.to.widgetId === toWidgetId &&
      c.to.portId === toPortId
  );
}

// Removes exact duplicate connections (same from-widget/port AND to-widget/port pair),
// keeping the first occurrence. A single logical cable should never appear twice - two
// identical entries would just double-sum the same signal into the target port. Called
// after loading a preset (a file saved before this dedup existed, or hand-edited, could
// already contain one) so a load always normalizes back to at most one cable per pair.
function dedupeConnections(list) {
  const seen = new Set();
  const deduped = [];
  let removedCount = 0;
  list.forEach((c) => {
    const key = `${c.from.widgetId} ${c.from.portId} ${c.to.widgetId} ${c.to.portId}`;
    if (seen.has(key)) {
      removedCount += 1;
      return;
    }
    seen.add(key);
    deduped.push(c);
  });
  if (removedCount > 0) {
    appendLog(`${removedCount} doppelte Verbindung(en) beim Laden entfernt.`);
  }
  return deduped;
}

function findPortDot(widgetId, portId, direction) {
  return canvas.querySelector(
    `.port-dot[data-widget-id="${widgetId}"][data-port-id="${portId}"][data-port-dir="${direction}"]`
  );
}

// The visible port circle is only 14x14px (.port-dot in style.css), and .port-row-in/-out
// pull it partway out of its row via a negative margin so it visually sits on the widget's
// edge - both make document.elementFromPoint() at the exact cursor pixel unreliable for
// dropping a cable: a neighboring element (the port label, a row background, ...) can be the
// topmost hit at that exact point even though the circle is right there, so only releasing
// on the very edge of the circle (where nothing else overlaps) ever registered. Search every
// visible "in" dot's real center (getBoundingClientRect(), same source portPosition() below
// uses to draw the cable) and snap to the nearest one within maxDistance instead of requiring
// an exact pixel hit - a generous but unambiguous target given ports are spaced well more
// than double that apart within a widget.
function findNearestInPortDot(clientX, clientY, maxDistance = 16) {
  let best = null;
  let bestDist = maxDistance;
  document.querySelectorAll('.port-dot[data-port-dir="in"]').forEach((dot) => {
    const rect = dot.getBoundingClientRect();
    if (rect.width === 0 && rect.height === 0) return; // not rendered (hidden page/widget)
    const cx = rect.left + rect.width / 2;
    const cy = rect.top + rect.height / 2;
    const dist = Math.hypot(clientX - cx, clientY - cy);
    if (dist <= bestDist) {
      bestDist = dist;
      best = dot;
    }
  });
  return best;
}

function portPosition(portEl) {
  const portRect = portEl.getBoundingClientRect();
  return clientToContent(
    portRect.left + portRect.width / 2,
    portRect.top + portRect.height / 2
  );
}

const CABLE_SAG_KEY = "espressynth.cableSag";
const ORTHOGONAL_CABLES_KEY = "espressynth.orthogonalCables";
let cableSagEnabled = localStorage.getItem(CABLE_SAG_KEY) === "1";
let orthogonalCablesEnabled = localStorage.getItem(ORTHOGONAL_CABLES_KEY) === "1";

function roundedOrthogonalPath(points, radius = 12) {
  const pathPoints = points.filter((point, index) =>
    index === 0 || point.x !== points[index - 1].x || point.y !== points[index - 1].y
  );
  if (pathPoints.length < 3) {
    return `M ${pathPoints[0].x} ${pathPoints[0].y} L ${pathPoints.at(-1).x} ${pathPoints.at(-1).y}`;
  }

  let path = `M ${pathPoints[0].x} ${pathPoints[0].y}`;
  for (let index = 1; index < pathPoints.length - 1; index += 1) {
    const previous = pathPoints[index - 1];
    const corner = pathPoints[index];
    const next = pathPoints[index + 1];
    const beforeLength = Math.hypot(corner.x - previous.x, corner.y - previous.y);
    const afterLength = Math.hypot(next.x - corner.x, next.y - corner.y);
    const cornerRadius = Math.min(radius, beforeLength / 2, afterLength / 2);
    const before = {
      x: corner.x + (previous.x - corner.x) * cornerRadius / beforeLength,
      y: corner.y + (previous.y - corner.y) * cornerRadius / beforeLength,
    };
    const after = {
      x: corner.x + (next.x - corner.x) * cornerRadius / afterLength,
      y: corner.y + (next.y - corner.y) * cornerRadius / afterLength,
    };
    path += ` L ${before.x} ${before.y} Q ${corner.x} ${corner.y}, ${after.x} ${after.y}`;
  }
  const last = pathPoints.at(-1);
  return `${path} L ${last.x} ${last.y}`;
}

function widgetBounds(widgetEl) {
  const rect = widgetEl.getBoundingClientRect();
  const topLeft = clientToContent(rect.left, rect.top);
  const bottomRight = clientToContent(rect.right, rect.bottom);
  return { left: topLeft.x, top: topLeft.y, right: bottomRight.x, bottom: bottomRight.y };
}

function orthogonalPath(p1, p2, fromWidgetEl, toWidgetEl) {
  if (fromWidgetEl && toWidgetEl) {
    const fromBounds = widgetBounds(fromWidgetEl);
    const toBounds = widgetBounds(toWidgetEl);
    const clearance = 20;
    const sourceExitX = Math.max(p1.x + clearance, fromBounds.right + clearance);
    const targetEntryX = Math.min(p2.x - clearance, toBounds.left - clearance);

    // Source is left of target: use free corridor between both widgets. This
    // is the normal audio/control flow and needs no detour above or below.
    if (sourceExitX <= targetEntryX) {
      const middleX = (sourceExitX + targetEntryX) / 2;
      return roundedOrthogonalPath([
        p1,
        { x: middleX, y: p1.y },
        { x: middleX, y: p2.y },
        p2,
      ]);
    }

    const targetBelowSource = p2.y >= p1.y;
    let routeY = targetBelowSource ? fromBounds.bottom + clearance : fromBounds.top - clearance;

    // Prefer gap between widgets. If they overlap vertically, go around their
    // shared outside edge so no horizontal cable segment crosses either widget.
    if (targetBelowSource && routeY >= toBounds.top - clearance) {
      routeY = Math.max(fromBounds.bottom, toBounds.bottom) + clearance;
    } else if (!targetBelowSource && routeY <= toBounds.bottom + clearance) {
      routeY = Math.min(fromBounds.top, toBounds.top) - clearance;
    }

    return roundedOrthogonalPath([
      p1,
      { x: sourceExitX, y: p1.y },
      { x: sourceExitX, y: routeY },
      { x: targetEntryX, y: routeY },
      { x: targetEntryX, y: p2.y },
      p2,
    ]);
  }

  const midX = (p1.x + p2.x) / 2;
  const horizontalDirection = Math.sign(p2.x - p1.x) || 1;
  const verticalDirection = Math.sign(p2.y - p1.y);
  const radius = Math.min(16, Math.abs(p2.x - p1.x) / 4, Math.abs(p2.y - p1.y) / 2);
  if (radius < 1) return `M ${p1.x} ${p1.y} L ${p2.x} ${p2.y}`;
  return `M ${p1.x} ${p1.y} L ${midX - horizontalDirection * radius} ${p1.y} Q ${midX} ${p1.y}, ${midX} ${p1.y + verticalDirection * radius} L ${midX} ${p2.y - verticalDirection * radius} Q ${midX} ${p2.y}, ${midX + horizontalDirection * radius} ${p2.y} L ${p2.x} ${p2.y}`;
}

function bezierPath(p1, p2, fromWidgetEl, toWidgetEl) {
  if (orthogonalCablesEnabled) return orthogonalPath(p1, p2, fromWidgetEl, toWidgetEl);
  const dx = Math.max(Math.abs(p2.x - p1.x) / 2, 40);
  if (cableSagEnabled) {
    const sagY = Math.max(p1.y, p2.y) + Math.min(120, 28 + Math.abs(p2.x - p1.x) * 0.08);
    return `M ${p1.x} ${p1.y} C ${p1.x + dx} ${sagY}, ${p2.x - dx} ${sagY}, ${p2.x} ${p2.y}`;
  }
  return `M ${p1.x} ${p1.y} C ${p1.x + dx} ${p1.y}, ${p2.x - dx} ${p2.y}, ${p2.x} ${p2.y}`;
}

function setCableSagEnabled(enabled) {
  cableSagEnabled = enabled;
  if (enabled) orthogonalCablesEnabled = false;
  localStorage.setItem(CABLE_SAG_KEY, enabled ? "1" : "0");
  localStorage.setItem(ORTHOGONAL_CABLES_KEY, orthogonalCablesEnabled ? "1" : "0");
  const checkbox = document.getElementById("menuCableSag");
  if (checkbox) checkbox.checked = enabled;
  const orthogonalCheckbox = document.getElementById("menuOrthogonalCables");
  if (orthogonalCheckbox) orthogonalCheckbox.checked = orthogonalCablesEnabled;
  drawConnections();
}

function setOrthogonalCablesEnabled(enabled) {
  orthogonalCablesEnabled = enabled;
  if (enabled) cableSagEnabled = false;
  localStorage.setItem(ORTHOGONAL_CABLES_KEY, enabled ? "1" : "0");
  localStorage.setItem(CABLE_SAG_KEY, cableSagEnabled ? "1" : "0");
  const checkbox = document.getElementById("menuOrthogonalCables");
  if (checkbox) checkbox.checked = enabled;
  const sagCheckbox = document.getElementById("menuCableSag");
  if (sagCheckbox) sagCheckbox.checked = cableSagEnabled;
  drawConnections();
}

function setGridSnapEnabled(enabled) {
  gridSnapEnabled = enabled;
  localStorage.setItem(GRID_SNAP_KEY, enabled ? "1" : "0");
  const checkbox = document.getElementById("menuGridSnap");
  if (checkbox) checkbox.checked = enabled;
}

const BACKGROUND_DOTS_KEY = "espressynth.backgroundDots";
let backgroundDotsEnabled = localStorage.getItem(BACKGROUND_DOTS_KEY) !== "0";

function setBackgroundDotsEnabled(enabled) {
  backgroundDotsEnabled = enabled;
  localStorage.setItem(BACKGROUND_DOTS_KEY, enabled ? "1" : "0");
  document.querySelectorAll(".canvas").forEach((canvasEl) => {
    canvasEl.classList.toggle("hide-background-dots", !enabled);
  });
  const checkbox = document.getElementById("menuBackgroundDots");
  if (checkbox) checkbox.checked = enabled;
}

let selectedConnectionId = null;

function drawConnections() {
  ensureConnectionColors();
  connectionsLayer.innerHTML = "";
  connections.forEach((conn) => {
    const fromDot = findPortDot(conn.from.widgetId, conn.from.portId, "out");
    const toDot = findPortDot(conn.to.widgetId, conn.to.portId, "in");
    if (!fromDot || !toDot) return;

    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("class", "connection-path");
    path.dataset.connId = conn.id;
    if (conn.id === selectedConnectionId) path.classList.add("selected");
    path.setAttribute(
      "d",
      bezierPath(
        portPosition(fromDot),
        portPosition(toDot),
        fromDot.closest(".widget-instance"),
        toDot.closest(".widget-instance")
      )
    );
    if (conn.color) path.style.stroke = conn.color;
    path.style.pointerEvents = "stroke";

    
    path.addEventListener("click", (e) => {
      e.stopPropagation();
      if (selectedConnectionId && selectedConnectionId !== conn.id) {
        const prev = connectionsLayer.querySelector(`[data-conn-id="${selectedConnectionId}"]`);
        if (prev) prev.classList.remove("selected");
      }
      selectedConnectionId = conn.id;
      path.classList.add("selected");
    });

    
    path.addEventListener("dblclick", (e) => {
      e.stopPropagation();
      e.preventDefault();
      pushHistorySnapshot();
      disconnectIfAudio(conn);
      connections = connections.filter((c) => c.id !== conn.id);
      if (selectedConnectionId === conn.id) selectedConnectionId = null;
      refreshControlGraph();
      refreshVoiceGroups();
      drawConnections();
      savePresetNow(); 
    });

    
    path.addEventListener("contextmenu", (e) => {
      e.preventDefault();
      e.stopPropagation();
      openConnectionColorPicker(conn.id);
    });

    connectionsLayer.appendChild(path);
  });
}



const CONNECTION_COLORS = [
  "#38bdf8",
  "#f87171",
  "#fb923c",
  "#fbbf24",
  "#a3e635",
  "#4ade80",
  "#2dd4bf",
  "#22d3ee",
  "#818cf8",
  "#c084fc",
  "#f472b6",
  "#e5e7eb",
];
const AUTO_CONNECTION_COLORS = CONNECTION_COLORS.filter(
  (color) => color !== "#38bdf8" && color !== "#fb923c"
);

let nextConnectionColorIndex = 0;

function allocateConnectionColor() {
  const color = AUTO_CONNECTION_COLORS[nextConnectionColorIndex % AUTO_CONNECTION_COLORS.length];
  nextConnectionColorIndex += 1;
  return color;
}

function getConnectionColorForOutput(fromWidgetId, fromPortId) {
  const existing = connections.find(
    (conn) => conn.from.widgetId === fromWidgetId && conn.from.portId === fromPortId && conn.color
  );
  return existing ? existing.color : allocateConnectionColor();
}

function ensureConnectionColors() {
  connections.forEach((conn) => {
    if (conn.color) return;
    const sibling = connections.find(
      (other) =>
        other !== conn &&
        other.from.widgetId === conn.from.widgetId &&
        other.from.portId === conn.from.portId &&
        other.color
    );
    conn.color = sibling ? sibling.color : allocateConnectionColor();
  });
}

const connectionColorOverlay = document.getElementById("connectionColorOverlay");
const colorSwatchGrid = document.getElementById("colorSwatchGrid");
let pendingColorConnId = null;

CONNECTION_COLORS.forEach((color) => {
  const swatch = document.createElement("div");
  swatch.className = "color-swatch";
  swatch.style.background = color;
  swatch.addEventListener("click", () => {
    const conn = connections.find((c) => c.id === pendingColorConnId);
    if (conn) conn.color = color;
    closeConnectionColorPicker();
    drawConnections();
    markDirty();
  });
  colorSwatchGrid.appendChild(swatch);
});

function openConnectionColorPicker(connId) {
  pendingColorConnId = connId;
  connectionColorOverlay.classList.remove("hidden");
}

function closeConnectionColorPicker() {
  connectionColorOverlay.classList.add("hidden");
  pendingColorConnId = null;
}

connectionColorOverlay.addEventListener("click", (e) => {
  if (e.target === connectionColorOverlay) closeConnectionColorPicker();
});

document.getElementById("connectionColorCancel").addEventListener("click", closeConnectionColorPicker);

function removeConnectionsForWidget(widgetId) {
  connections = connections.filter(
    (c) => c.from.widgetId !== widgetId && c.to.widgetId !== widgetId
  );
}

function disconnectIfAudio(conn) {
  const fromKind = getPortKind(conn.from.widgetId, conn.from.portId, "out");
  const toKind = getPortKind(conn.to.widgetId, conn.to.portId, "in");
  if (fromKind === "audio" && toKind === "audio") {
    audioEngine.disconnect(conn.from.widgetId, conn.from.portId, conn.to.widgetId, conn.to.portId);
  }
}

function removeConnectionsForPort(widgetId, portId, direction) {
  const toRemove = connections.filter((c) => {
    if (direction === "in") return c.to.widgetId === widgetId && c.to.portId === portId;
    return c.from.widgetId === widgetId && c.from.portId === portId;
  });
  toRemove.forEach(disconnectIfAudio);
  connections = connections.filter((c) => !toRemove.includes(c));
  refreshControlGraph();
  refreshVoiceGroups();
}

// Set while a connection drag (mousedown on an out-dot through mouseup on an in-dot) is in
// progress. If mouseup never reaches onUp below (e.g. released outside the window, focus
// lost mid-drag), a later drag could otherwise start a second onMove/onUp listener pair on
// top of the still-attached first one - two live listeners racing to add a connection from
// the same stale `fromDot` is exactly the kind of duplicate-cable bug connectionExists()
// alone can't fully rule out. Force-cleanup any stale drag before starting a new one.
let activeConnectionDragCleanup = null;

function startConnectionDrag(e, fromDot) {
  e.preventDefault();
  e.stopPropagation();

  if (activeConnectionDragCleanup) {
    activeConnectionDragCleanup();
    activeConnectionDragCleanup = null;
  }

  const start = portPosition(fromDot);
  const tempPath = document.createElementNS("http://www.w3.org/2000/svg", "path");
  tempPath.setAttribute("class", "connection-path-temp");
  connectionsLayer.appendChild(tempPath);

  function onMove(ev) {
    const end = clientToContent(ev.clientX, ev.clientY);
    tempPath.setAttribute("d", bezierPath(start, end));
  }

  function onUp(ev) {
    document.removeEventListener("mousemove", onMove);
    document.removeEventListener("mouseup", onUp);
    activeConnectionDragCleanup = null;
    tempPath.remove();

    const target = findNearestInPortDot(ev.clientX, ev.clientY);
    if (target) {
      const fromWidgetId = fromDot.dataset.widgetId;
      const fromPortId = fromDot.dataset.portId;
      const toWidgetId = target.dataset.widgetId;
      const toPortId = target.dataset.portId;

      if (fromWidgetId === toWidgetId) {
        appendLog("Verbindung abgelehnt: Ausgang und Eingang gehoeren zum selben Widget.");
        return;
      }

      if (connectionExists(fromWidgetId, fromPortId, toWidgetId, toPortId)) {
        appendLog("Verbindung abgelehnt: Diese Verbindung existiert bereits.");
        return;
      }

      pushHistorySnapshot();
      connectionCounter += 1;
      const toKind = getPortKind(toWidgetId, toPortId, "in");

      if (toKind !== "audio") {
        connections.filter(
          (c) => c.to.widgetId === toWidgetId && c.to.portId === toPortId
        ).forEach(disconnectIfAudio);
        connections = connections.filter(
          (c) => !(c.to.widgetId === toWidgetId && c.to.portId === toPortId)
        );
      }
      connections.push({
        id: `conn-${connectionCounter}`,
        from: { widgetId: fromWidgetId, portId: fromPortId },
        to: { widgetId: toWidgetId, portId: toPortId },
        color: getConnectionColorForOutput(fromWidgetId, fromPortId),
      });

      const fromKind = getPortKind(fromWidgetId, fromPortId, "out");
      if (fromKind === "audio" && toKind === "audio") {
        audioEngine.connect(fromWidgetId, fromPortId, toWidgetId, toPortId);
      }

      refreshControlGraph();
      refreshVoiceGroups();
      drawConnections();
      savePresetNow();
    }
  }
  activeConnectionDragCleanup = () => onUp({ clientX: -1, clientY: -1 });
  document.addEventListener("mousemove", onMove);
  document.addEventListener("mouseup", onUp);
}



function propagate(widgetId, portId, value, voiceIndex) {
  const sourceKind = getPortKind(widgetId, portId, "out");
  connections
    .filter((c) => c.from.widgetId === widgetId && c.from.portId === portId)
    .forEach((c) => {
      // Audio sources are sampled by tickDebugVoltmeterAudioSources(). Do not
      // also push the parallel control-domain value into these displays: an
      // LFO would then make the needle alternate between two independent
      // sample paths and appear to flicker.
      if (sourceKind === "audio" && c.to.portId === "in") {
        const target = document.getElementById(c.to.widgetId);
        if (target && (target.dataset.widgetType === "debug" || target.dataset.widgetType === "voltmeter")) {
          return;
        }
      }
      deliverToWidget(c.to.widgetId, c.to.portId, value, voiceIndex);
    });
}

function deliverToWidget(widgetId, portId, value, voiceIndex) {
  const el = document.getElementById(widgetId);
  if (!el) return;

  if (el.dataset.widgetType === "midi-in" && portId === "voices") {
    const normalized = clamp(Number(value) || 0, 0, CV_MAX_VOLTS) / CV_MAX_VOLTS;
    setMidiInVoices(widgetId, 1 + normalized * (midiInVoiceLimit(widgetId) - 1));
  } else if (el.dataset.widgetType === "midi-in" && portId === "glide") {
    setMidiInGlide(widgetId, Number(value) >= GATE_THRESHOLD_VOLTS);
  } else if (el.dataset.widgetType === "midi-in" && portId === "glideTime") {
    const normalized = clamp(Number(value) || 0, 0, CV_MAX_VOLTS) / CV_MAX_VOLTS;
    setMidiInGlideTime(widgetId, normalized * 2);
  }

  if (el.dataset.widgetType === "debug") {
    const valueSpan = el.querySelector('[data-role="debug-value"]');
    if (valueSpan) valueSpan.textContent = formatNumber(value);
    const nameSpan = el.querySelector(".widget-instance-label");
    const name = nameSpan ? nameSpan.textContent : widgetId;
    appendLog(`Debug (${name}): ${value}`);
  }

  if (el.dataset.widgetType === "voltmeter") {
    const state = voltmeterStates.get(widgetId);
    const num = Number(value);
    const volts = Number.isNaN(num) ? 0 : num;
    if (state) state.value = volts;
    const readout = el.querySelector('[data-role="voltmeter-readout"]');
    const displayVolts = clamp(volts, -CV_MAX_VOLTS, CV_MAX_VOLTS);
    if (readout) {
      const sign = displayVolts >= 0 ? "+" : "";
      readout.textContent = `${sign}${displayVolts.toFixed(2)} V`;
      readout.classList.toggle("voltmeter-negative", displayVolts < 0);
      readout.classList.toggle("voltmeter-positive", displayVolts > 0);
    }
    const needle = el.querySelector('[data-role="voltmeter-needle"]');
    if (needle) needle.style.transform = `rotate(${(displayVolts / CV_MAX_VOLTS) * 60}deg)`;
  }

  if (el.dataset.widgetType === "multiplier") {
    const state = multiplierStates.get(widgetId);
    if (state) {
      const idx = parseInt(String(portId).replace("in", ""), 10) - 1;
      if (idx >= 0 && idx < state.inputCount) {
        const num = Number(value);
        state.values[idx] = Number.isNaN(num) ? 0 : num;
        recomputeMultiplier(widgetId);
        markDirty();
      }
    }
  }

  if (el.dataset.widgetType === "adder") {
    const state = adderStates.get(widgetId);
    if (state) {
      const idx = parseInt(String(portId).replace("in", ""), 10) - 1;
      if (idx >= 0 && idx < state.inputCount) {
        const num = Number(value);
        state.values[idx] = Number.isNaN(num) ? 0 : num;
        recomputeAdder(widgetId);
        markDirty();
      }
    }
  }

  if (el.dataset.widgetType === "divide") {
    const state = divideStates.get(widgetId);
    if (state) {
      const idx = parseInt(String(portId).replace("in", ""), 10) - 1;
      if (idx === 0 || idx === 1) {
        const num = Number(value);
        const fallback = idx === 0 ? 0 : 1;
        state.values[idx] = Number.isNaN(num) ? fallback : num;
        recomputeDivide(widgetId);
        markDirty();
      }
    }
  }

  if (el.dataset.widgetType === "frequency-divider") {
    const state = frequencyDividerStates.get(widgetId);
    if (state) {
      const num = Number(value);
      if ((portId === "cv" || portId === "hz") && !Number.isNaN(num)) {
        state.cv = clamp(num, 0, CV_MAX_VOLTS);
        recomputeFrequencyDivider(widgetId, voiceIndex);
      } else if (portId === "divider") {
        state.divider = nearestFrequencyDivider(num);
        updateFrequencyDividerVisual(widgetId);
        sendLiveParamUpdateThrottled(widgetId, { divider: state.divider });
        
        
        
        if (state.audioMode) audioEngine.setParam(widgetId, "divider", state.divider, 0);
        recomputeFrequencyDivider(widgetId, voiceIndex);
      }
    }
  }

  if (el.dataset.widgetType === "abs" && portId === "in") {
    const state = absStates.get(widgetId);
    const num = Number(value);
    if (state && !Number.isNaN(num)) {
      state.value = num;
      recomputeAbs(widgetId, voiceIndex);
    }
  }

  if (el.dataset.widgetType === "value-mapper" && portId === "in") {
    const state = valueMapperStates.get(widgetId);
    const num = Number(value);
    if (state && !Number.isNaN(num)) {
      state.value = num;
      recomputeValueMapper(widgetId, voiceIndex);
    }
  }

  if (el.dataset.widgetType === "mixer" && /^gain\d+$/.test(portId)) {
    const state = mixerStates.get(widgetId);
    if (state) {
      const idx = parseInt(portId.slice(4), 10);
      if (idx >= 1 && idx <= state.inputCount) {
        const num = Number(value);
        const safe = clamp(Number.isNaN(num) ? 1 : num, 0, 2);
        state.gains[idx - 1] = safe;
        audioEngine.setParam(widgetId, portId, safe);
        updateMixerGainVisual(widgetId, idx);
        sendLiveParamUpdateThrottled(widgetId, { [portId]: safe });
      }
    }
  }

  // "select" is real connectable CV port (0-10V), delivered as volts via
  // propagate() when wired. CV voltage maps to one input band.
  if (el.dataset.widgetType === "multiplexer" && portId === "select") {
    const state = multiplexerStates.get(widgetId);
    if (state) {
      const wired = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "select");
      const num = Number(value);
      if (wired) {
        const volts = Number.isNaN(num) ? 0 : clamp(num, 0, CV_MAX_VOLTS);
        state.cvVolts = volts;
        multiplexerSetActiveIndex(widgetId, multiplexerSelectFromCv(volts, state.inputCount));
      } else {
        state.cvVolts = null;
        const idx = clamp(Number.isNaN(num) ? 0 : Math.round(num), 0, Math.max(0, state.inputCount - 1));
        state.manualSelect = idx;
        sendLiveParamUpdateThrottled(widgetId, { select: idx });
        multiplexerSetActiveIndex(widgetId, idx);
      }
    }
  }

  if (el.dataset.widgetType === "multiplexer" && /^in\d+$/.test(portId)) {
    const state = multiplexerStates.get(widgetId);
    if (state) {
      const idx = parseInt(portId.slice(2), 10) - 1;
      if (idx >= 0 && idx < state.inputCount) {
        const num = Number(value);
        state.values[idx] = Number.isNaN(num) ? 0 : num;
        if (idx === state.activeIndex) publishMultiplexerValue(widgetId);
      }
    }
  }

  if (el.dataset.widgetType === "clock" && portId === "tempo") {
    const state = clockStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.cvVolts = Number.isNaN(num) ? 0 : clamp(num, 0, CV_MAX_VOLTS);
      state.bpm = clockBpm(state.cvVolts, state.minBpm, state.maxBpm);
      restartClockTimer(widgetId);
      markDirty();
    }
  }

  if (el.dataset.widgetType === "lfo" && portId === "rate") {
    const state = lfoStates.get(widgetId);
    if (state) {
      const num = Number(value);
      // Connected CV is 0-10 V and maps linearly to the configured max rate.
      state.rate = Number.isNaN(num) ? 0 : clamp(num, 0, CV_MAX_VOLTS) * state.maxRate / CV_MAX_VOLTS;
      audioEngine.setParam(widgetId, "rate", state.rate);
      updateLfoVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { rate: state.rate });
    }
  }

  if (el.dataset.widgetType === "lfo" && portId === "amplitude") {
    const state = lfoStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.amplitude = Number.isNaN(num) ? CV_MAX_VOLTS : clamp(num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "amplitude", state.amplitude);
      updateLfoVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { amplitude: state.amplitude });
    }
  }

  if (el.dataset.widgetType === "lfo" && portId === "phase") {
    const state = lfoStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.phase = Number.isNaN(num) ? 0 : clamp(num, -180, 180);
      audioEngine.setParam(widgetId, "phase", state.phase);
      updateLfoVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { phase: state.phase });
    }
  }

  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  if (el.dataset.widgetType === "lfo" && portId === "sync") {
    const state = lfoStates.get(widgetId);
    if (state) {
      const above = Number(value) >= GATE_THRESHOLD_VOLTS;
      if (!state.syncPrevByVoice) state.syncPrevByVoice = new Map();
      const voiceKey = voiceIndex ?? "global";
      const wasAbove = state.syncPrevByVoice.get(voiceKey) === true;
      if (above && !wasAbove) {
        audioEngine.resetLfoPhase(widgetId, voiceIndex);
        state.cyclePos = 0;
        state.lastTick = performance.now(); 
        if (state.oneShot) {
          
          
          state.oneShotDone = false;
          state.oneShotAccum = 0;
        }
        tickLfo(widgetId); 
      }
      state.syncPrevByVoice.set(voiceKey, above);
    }
    const dot = el.querySelector('.port-row-in [data-port-id="sync"]');
    setLedActive(dot, Number(value) >= GATE_THRESHOLD_VOLTS);
  }

  
  
  
  
  if (el.dataset.widgetType === "lfo" && portId === "oneShot") {
    const state = lfoStates.get(widgetId);
    if (state) {
      state.oneShot = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "oneShot", state.oneShot);
      updateLfoVisual(widgetId);
      sendLiveParamUpdate(widgetId, { oneShot: state.oneShot });
    }
  }

  
  
  if (el.dataset.widgetType === "lfo" && portId === "global") {
    const state = lfoStates.get(widgetId);
    if (state) {
      state.global = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "global", state.global);
      updateLfoVisual(widgetId);
      sendLiveParamUpdate(widgetId, { global: state.global });
    }
  }

  
  
  
  
  
  if (el.dataset.widgetType === "lfo" && portId === "zeroPoint") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = lfoStates.get(widgetId);
      if (state) {
        state.zeroPoint = clamp(num, 0, CV_MAX_VOLTS);
        audioEngine.setParam(widgetId, "zeroPoint", state.zeroPoint);
        updateLfoVisual(widgetId);
        sendLiveParamUpdate(widgetId, { zeroPoint: state.zeroPoint });
      }
    }
  }


  if (el.dataset.widgetType === "oscillator" && portId === "pitchCv") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = oscillatorStates.get(widgetId);
      if (state) {
        
        
        
        
        state.pitchCv = num;
        recomputeOscillatorFreq(widgetId, voiceIndex);
      }
    }
  }

  if (el.dataset.widgetType === "oscillator" && portId === "tune") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = oscillatorStates.get(widgetId);
      if (state) {
        
        
        
        state.tuneVolts = clamp(num, 0, CV_MAX_VOLTS);
        recomputeOscillatorFreq(widgetId, voiceIndex);
        updateOscillatorTuneVisual(widgetId);
        
        
        
        if (voiceIndex === undefined) {
          sendLiveParamUpdateThrottled(widgetId, { tune: state.tuneVolts });
        }
      }
    }
  }

  if (el.dataset.widgetType === "oscillator" && portId === "gain") {
    const state = oscillatorStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.gain = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "gain", state.gain);
      updateOscillatorGainVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { gain: state.gain });
    }
  }

  if (el.dataset.widgetType === "detune" && portId === "cv") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      recomputeDetune(widgetId, num, voiceIndex);
    }
  }

  if (el.dataset.widgetType === "switch" && (portId === "in1" || portId === "in2")) {
    const state = switchStates.get(widgetId);
    const num = Number(value);
    if (state && !Number.isNaN(num)) {
      state[portId] = num;
      publishSwitchValue(widgetId);
    }
  }

  if (el.dataset.widgetType === "detune" && portId === "amount") {
    const state = detuneStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.amount = Number.isNaN(num) ? 0 : clamp(num, -1, 1);
      updateDetuneVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { amount: state.amount });
      if (state.audioMode) audioEngine.setParam(widgetId, "amount", state.amount, 0);
      recomputeDetune(widgetId, undefined, undefined);
    }
  }

  if (el.dataset.widgetType === "oscillator" && portId === "amplitude") {
    const num = Number(value);
    const safe = Number.isNaN(num) ? 1 : num;
    if (voiceIndex !== undefined) {
      audioEngine.setVoiceParam(widgetId, voiceIndex, "amplitude", safe);
    } else {
      const state = oscillatorStates.get(widgetId);
      if (state) {
        state.amplitude = safe;
        audioEngine.setParam(widgetId, "amplitude", state.amplitude);
        sendLiveParamUpdateThrottled(widgetId, { amplitude: state.amplitude });
      }
    }
  }

  if (el.dataset.widgetType === "oscillator" && portId === "gate") {
    if (voiceIndex !== undefined) {
      audioEngine.setVoiceParam(widgetId, voiceIndex, "gate", Number(value));
    } else {
      audioEngine.setParam(widgetId, "gate", Number(value));
    }
    const dot = el.querySelector('.port-row-in [data-port-id="gate"]');
    setLedActive(dot, Number(value) > GATE_THRESHOLD_VOLTS);
  }

  if (el.dataset.widgetType === "hammond" && portId === "pitchCv") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = hammondStates.get(widgetId);
      if (state) {
        state.pitchCv = num;
        recomputeHammondFreq(widgetId, voiceIndex);
      }
    }
  }

  if (el.dataset.widgetType === "hammond" && portId === "tune") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = hammondStates.get(widgetId);
      if (state) {
        state.tuneVolts = clamp(num, 0, CV_MAX_VOLTS);
        recomputeHammondFreq(widgetId, voiceIndex);
        updateHammondVisual(widgetId);
        if (voiceIndex === undefined) {
          sendLiveParamUpdateThrottled(widgetId, { tune: state.tuneVolts });
        }
      }
    }
  }

  if (el.dataset.widgetType === "hammond" && portId === "gain") {
    const state = hammondStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.gain = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "gain", state.gain);
      updateHammondVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { gain: state.gain });
    }
  }

  if (el.dataset.widgetType === "hammond" && portId === "amplitude") {
    const num = Number(value);
    const safe = Number.isNaN(num) ? 1 : num;
    if (voiceIndex !== undefined) {
      audioEngine.setVoiceParam(widgetId, voiceIndex, "amplitude", safe);
    } else {
      const state = hammondStates.get(widgetId);
      if (state) {
        state.amplitude = safe;
        audioEngine.setParam(widgetId, "amplitude", state.amplitude);
        sendLiveParamUpdateThrottled(widgetId, { amplitude: state.amplitude });
      }
    }
  }

  // The nine drawbar registers - real audio-rate CV ports here (unlike
  // Mixer's control-only "gainN"): a slider drag AND a live cable both land
  // here, exactly like Oscillator's own "gain" case above.
  if (el.dataset.widgetType === "hammond" && /^drawbar[1-9]$/.test(portId)) {
    const state = hammondStates.get(widgetId);
    if (state) {
      const k = Number(portId.slice(7));
      const num = Number(value);
      state.drawbars[k - 1] = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, portId, state.drawbars[k - 1]);
      updateHammondDrawbarVisual(widgetId, k);
      sendLiveParamUpdateThrottled(widgetId, { [portId]: state.drawbars[k - 1] });
    }
  }

  if (el.dataset.widgetType === "wave-player" && portId === "note") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      if (voiceIndex !== undefined) {
        audioEngine.setVoiceParam(widgetId, voiceIndex, "note", num, 0);
      } else {
        audioEngine.setParam(widgetId, "note", num, 0);
      }
    }
  }

  if (el.dataset.widgetType === "wave-player" && portId === "gate") {
    if (voiceIndex !== undefined) {
      audioEngine.setVoiceParam(widgetId, voiceIndex, "gate", Number(value));
    } else {
      audioEngine.setParam(widgetId, "gate", Number(value));
    }
  }

  if (el.dataset.widgetType === "wave-player" && portId === "amplitude") {
    const num = Number(value);
    const safe = Number.isNaN(num) ? 1 : num;
    if (voiceIndex !== undefined) {
      audioEngine.setVoiceParam(widgetId, voiceIndex, "amplitude", safe);
    } else {
      audioEngine.setParam(widgetId, "amplitude", safe);
    }
  }

  if (el.dataset.widgetType === "soundfont-player" && (portId === "bank" || portId === "program")) {
    const state = soundFontPlayerStates.get(widgetId);
    if (state) {
      const max = portId === "bank" ? (state.maxBank ?? SOUNDFONT_DEFAULT_MAX_BANK) : 127;
      state[portId] = clamp(Math.round(Number(value) || 0), 0, max);
      audioEngine.setSoundFontParams(widgetId, { bank: state.bank, program: state.program });
      refreshSoundFontPlayerMeta(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
    }
  }

  if (el.dataset.widgetType === "soundfont-player" && ["enableCc", "enableModWheel", "enablePitchWheel"].includes(portId)) {
    const state = soundFontPlayerStates.get(widgetId);
    if (state) {
      state[portId] = Number(value) >= 0.5;
      audioEngine.setSoundFontParams(widgetId, { [portId]: state[portId] });
      updateSoundFontPlayerControls(widgetId);
      sendLiveParamUpdate(widgetId, { [portId]: state[portId] });
    }
  }

  if (el.dataset.widgetType === "soundfont-player" && portId === "gain") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = soundFontPlayerStates.get(widgetId);
      if (state) {
        state.gain = clamp(num, 0, CV_MAX_VOLTS);
        audioEngine.setSoundFontParams(widgetId, { gain: state.gain });
        updateSoundFontPlayerControls(widgetId);
        sendLiveParamUpdate(widgetId, { gain: state.gain });
      }
    }
  }

  if (el.dataset.widgetType === "oscillator" && portId === "waveform") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      setOscillatorWaveType(widgetId, mapWaveformValue(num));
    }
  }

  if (el.dataset.widgetType === "audio-out" || el.dataset.widgetType === "usb-out" ||
      el.dataset.widgetType === "es8311-out" || el.dataset.widgetType === "wm8960-out") {
    const state = audioOutStates.get(widgetId);
    if (state) {
      const num = Number(value);
      if (portId === "gain") {
        setAudioOutParams(widgetId, { gain: Number.isNaN(num) ? 0 : clamp(num, 0, CV_MAX_VOLTS) });
      } else if (portId === "muted") {
        setAudioOutParams(widgetId, { muted: !Number.isNaN(num) && num >= GATE_THRESHOLD_VOLTS });
      } else {
        const clamped = Number.isNaN(num) ? 0 : clamp(num, -2, 2);
        audioEngine.setParam(widgetId, portId, clamped);
      }
      
      
      
    }
  }

  if (el.dataset.widgetType === "audio-input-link" && /^gain[1-8]$/.test(portId)) {
    setAudioInputLinkGain(widgetId, Number(portId.slice(4)) - 1, value);
  }

  if (el.dataset.widgetType === "gain" && portId === "amount") {
    const state = gainStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.amount = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "amount", state.amount);
      updateGainVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { amount: state.amount });
    }
  }

  
  
  if (el.dataset.widgetType === "gain" && portId === "global") {
    const state = gainStates.get(widgetId);
    if (state) {
      state.global = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "global", state.global);
      updateGainVisual(widgetId);
      sendLiveParamUpdate(widgetId, { global: state.global });
    }
  }

  if ((el.dataset.widgetType === "filter" || el.dataset.widgetType === "curtis-filter") && (portId === "cutoff" || portId === "resonance")) {
    const state = filterStates.get(widgetId);
    if (state) {
      const num = Number(value);
      const safe = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      state[portId] = safe;
      audioEngine.setParam(widgetId, portId, safe);
      updateFilterVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { [portId]: safe });
    }
  }

  
  
  if ((el.dataset.widgetType === "filter" || el.dataset.widgetType === "curtis-filter") && portId === "global") {
    const state = filterStates.get(widgetId);
    if (state) {
      state.global = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "global", state.global);
      updateFilterVisual(widgetId);
      sendLiveParamUpdate(widgetId, { global: state.global });
    }
  }

  if (
    el.dataset.widgetType === "chorus" &&
    (portId === "rate" || portId === "depth" || portId === "offset")
  ) {
    const state = chorusStates.get(widgetId);
    if (state) {
      const num = Number(value);
      const safe = Number.isNaN(num) ? 0 : num;
      state[portId] = safe;
      audioEngine.setParam(widgetId, portId, safe);
      updateChorusVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { [portId]: safe });
    }
  }

  if (isEffectCvBypassType(el.dataset.widgetType) && portId === "bypass") {
    const state = effectCvStates.get(widgetId);
    if (state) {
      state.bypass = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "bypass", state.bypass);
      updateEffectCvVisual(widgetId);
      sendLiveParamUpdate(widgetId, { bypass: state.bypass });
    }
  }

  if ((el.dataset.widgetType === "filter" || el.dataset.widgetType === "curtis-filter") && portId === "bypass") {
    const state = filterStates.get(widgetId);
    if (state) {
      state.bypass = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "bypass", state.bypass);
      updateFilterVisual(widgetId);
      sendLiveParamUpdate(widgetId, { bypass: state.bypass });
    }
  }

  if (el.dataset.widgetType === "chorus" && portId === "bypass") {
    const state = chorusStates.get(widgetId);
    if (state) {
      state.bypass = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "bypass", state.bypass);
      updateChorusVisual(widgetId);
      sendLiveParamUpdate(widgetId, { bypass: state.bypass });
    }
  }

  // esp-mixer deliberately excluded - see hasBypass()'s comment (no single
  // dry signal for a 2-input mixer, no bypass toggle exposed for it).
  if (hasBypass(el.dataset.widgetType) && el.dataset.widgetType !== "esp-mixer" &&
      el.dataset.widgetType.startsWith("esp-") && portId === "bypass") {
    const state = espEffectStates.get(widgetId);
    if (state) {
      state.bypass = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "bypass", state.bypass);
      updateEspEffectVisual(widgetId);
      sendLiveParamUpdate(widgetId, { bypass: state.bypass });
    }
  }

  {
    const paramDef = effectCvParamDef(el.dataset.widgetType, portId);
    if (paramDef) {
      const state = effectCvStates.get(widgetId);
      if (state) {
        const num = Number(value);
        const volts = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
        const s = state[portId];
        s.volts = volts;
        const mapped = cvToRange(volts, s.min, s.max, s.curve);
        audioEngine.setParam(widgetId, paramDef.paramName, mapped);
        updateEffectCvVisual(widgetId);
        sendLiveParamUpdateThrottled(widgetId, { [paramDef.paramName]: mapped });
      }
    }
  }

  if (ESP_EFFECT_DEFAULTS[el.dataset.widgetType] && Object.hasOwn(ESP_EFFECT_DEFAULTS[el.dataset.widgetType], portId)) {
    const state = espEffectStates.get(widgetId);
    const num = Number(value);
    if (state && !Number.isNaN(num)) {
      state[portId] = num;
      audioEngine.setParam(widgetId, portId, num);
      updateEspEffectVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { [portId]: num });
    }
  }

  if (el.dataset.widgetType === "adsr") {
    const state = adsrStates.get(widgetId);
    if (state) {
      if (portId === "gate") {
        if (voiceIndex !== undefined) {
          audioEngine.setVoiceParam(widgetId, voiceIndex, "gate", Number(value));
        } else {
          audioEngine.setParam(widgetId, "gate", Number(value));
        }
        const dot = el.querySelector('.port-row-in [data-port-id="gate"]');
        setLedActive(dot, Number(value) > GATE_THRESHOLD_VOLTS);
      } else if (portId === "velocity") {
        
        
        
        const vel = Number.isFinite(Number(value)) ? Number(value) : CV_MAX_VOLTS;
        if (voiceIndex !== undefined) {
          audioEngine.setVoiceParam(widgetId, voiceIndex, "velocity", vel);
        } else {
          audioEngine.setParam(widgetId, "velocity", vel);
        }
      } else if (portId === "att" || portId === "dec" || portId === "rel") {
        const paramName = { att: "attack", dec: "decay", rel: "release" }[portId];
        const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
        const seconds = cvToSeconds(volts, state[`${paramName}MaxSeconds`], state[`${paramName}Curve`]);
        state[paramName] = seconds;
        audioEngine.setParam(widgetId, paramName, seconds);
        updateAdsrSliders(widgetId);
        sendLiveParamUpdateThrottled(widgetId, { [paramName]: seconds });
      } else if (portId === "sus") {
        const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
        const level = volts / CV_MAX_VOLTS;
        state.sustain = level;
        audioEngine.setParam(widgetId, "sustain", level);
        updateAdsrSliders(widgetId);
        sendLiveParamUpdateThrottled(widgetId, { sustain: level });
      }
    }
  }
}

function noteToFrequency(note) {
  return 440 * 2 ** ((note - 69) / 12);
}





const CV_MAX_VOLTS = 10;
const AUDIO_PEAK_VOLTS = 5; // mirrors cv::kAudioPeakVolts / audio-engine.js's AUDIO_PEAK_VOLTS
const HAMMOND_DRAWBAR_STEP_V = CV_MAX_VOLTS / 8; // 1.25V per detent, 9 positions 0-8
const GATE_ON_VOLTS = 10;
const GATE_OFF_VOLTS = 0;
const GATE_THRESHOLD_VOLTS = 5; 



const PITCH_CV_ANCHOR_NOTE = 60;
const PITCH_CV_VOLTS_PER_OCTAVE = 1;

function noteToVoltage(note) {
  const volts = ((note - PITCH_CV_ANCHOR_NOTE) / 12) * PITCH_CV_VOLTS_PER_OCTAVE + CV_MAX_VOLTS / 2;
  return clamp(volts, 0, CV_MAX_VOLTS);
}






function hzFromPitchCv(volts) {
  const note = ((volts - CV_MAX_VOLTS / 2) / PITCH_CV_VOLTS_PER_OCTAVE) * 12 + PITCH_CV_ANCHOR_NOTE;
  return noteToFrequency(note);
}

function velocityToVoltage(velocity127) {
  return roundFloat((velocity127 / 127) * CV_MAX_VOLTS, 4);
}








function cvToRange(volts, minVal, maxVal, curve) {
  const v = clamp(volts / CV_MAX_VOLTS, 0, 1);
  if (curve === "log") {
    const lo = minVal <= 0 ? 0.0001 : minVal;
    return lo * (maxVal / lo) ** v;
  }
  return minVal + v * (maxVal - minVal);
}




function rangeToCv(value, minVal, maxVal, curve) {
  if (curve === "log") {
    const lo = minVal <= 0 ? 0.0001 : minVal;
    const ratio = Math.max(value / lo, 1e-6);
    const denom = Math.log(maxVal / lo);
    const v = denom === 0 ? 0 : Math.log(ratio) / denom;
    return clamp(v * CV_MAX_VOLTS, 0, CV_MAX_VOLTS);
  }
  const v = maxVal === minVal ? 0 : (value - minVal) / (maxVal - minVal);
  return clamp(v * CV_MAX_VOLTS, 0, CV_MAX_VOLTS);
}







function cvToSecondsLog(volts, maxSeconds) {
  const v = clamp(volts / CV_MAX_VOLTS, 0, 1);
  return maxSeconds * 10 ** (3 * (v - 1));
}




function secondsToCv(seconds, maxSeconds, curve) {
  const s = clamp(seconds, 0, maxSeconds);
  if (curve === "log") {
    const ratio = Math.max(s / maxSeconds, 1e-6);
    return clamp(CV_MAX_VOLTS * (1 + Math.log10(ratio) / 3), 0, CV_MAX_VOLTS);
  }
  return clamp((s / maxSeconds) * CV_MAX_VOLTS, 0, CV_MAX_VOLTS);
}

function cvToSeconds(volts, maxSeconds, curve) {
  if (curve === "log") return cvToSecondsLog(volts, maxSeconds);
  return clamp(volts / CV_MAX_VOLTS, 0, 1) * maxSeconds;
}






function allocateVoice(state, slotState, note, stillSounding) {
  const slots = slotState.slots;

  const sameNoteIdx = slots.findIndex((s) => s.note === note);
  if (sameNoteIdx !== -1) {
    slotState.holdCounter += 1;
    slots[sameNoteIdx].heldSince = slotState.holdCounter;
    slots[sameNoteIdx].held = false;
    return sameNoteIdx;
  }

  // First truly idle slot (freed and its release has actually decayed to
  // silence, or never used). `note === null` alone doesn't mean that -
  // stillSounding(i) catches a slot whose ADSR release is still ringing out,
  // so a different note doesn't chop off its tail just because it happens
  // to sit at a lower slot index (see isVoiceStillSounding() in
  // audio-engine.js and the identical fix in VoiceAllocator::noteOn).
  const freeIdx = slots.findIndex((s, i) => s.note === null && !(stillSounding && stillSounding(i)));
  if (freeIdx !== -1) {
    slotState.holdCounter += 1;
    slots[freeIdx] = { note, heldSince: slotState.holdCounter, held: false };
    return freeIdx;
  }

  // No fully-idle slot: reuse the oldest slot that's freed but still
  // ringing out its release, rather than jumping straight to
  // overflow-stealing an actively-held/sustained voice.
  if (stillSounding) {
    let oldestReleasing = -1;
    slots.forEach((s, i) => {
      if (s.note === null && (oldestReleasing === -1 || s.heldSince < slots[oldestReleasing].heldSince)) {
        oldestReleasing = i;
      }
    });
    if (oldestReleasing !== -1) {
      slotState.holdCounter += 1;
      slots[oldestReleasing] = { note, heldSince: slotState.holdCounter, held: false };
      return oldestReleasing;
    }
  }

  if (state.overflowMode === "ignore") return null;

  
  
  
  const heldIndices = [];
  for (let i = 0; i < slots.length; i += 1) {
    if (slots[i].held) heldIndices.push(i);
  }
  const candidates = heldIndices.length > 0 ? heldIndices : slots.map((_, i) => i);
  let oldestIdx = candidates[0];
  for (const i of candidates) {
    if (slots[i].heldSince < slots[oldestIdx].heldSince) oldestIdx = i;
  }
  slotState.holdCounter += 1;
  slots[oldestIdx] = { note, heldSince: slotState.holdCounter, held: false };
  return oldestIdx;
}






function findVoiceForNote(slotState, note) {
  const idx = slotState.slots.findIndex((s) => s.note === note);
  return idx === -1 ? null : idx;
}

function mostRecentHeldMonoNote(slotState) {
  let recentNote = null;
  let recentOrder = -1;
  for (const [note, held] of slotState.monoHeldNotes) {
    if (held.order > recentOrder) {
      recentNote = note;
      recentOrder = held.order;
    }
  }
  return recentNote;
}

const midiPitchGlideStates = new Map();

function propagateMidiPitchCv(widgetId, targetCv, voiceIndex) {
  const state = midiInStates.get(widgetId);
  if (!state) return;

  const key = `${widgetId}:${voiceIndex ?? 0}`;
  const glideState = midiPitchGlideStates.get(key) || { value: null, frame: null };
  if (glideState.frame !== null) cancelAnimationFrame(glideState.frame);

  const startCv = glideState.value ?? targetCv;
  const durationMs = state.voices === 1 && state.glide ? state.glideTime * 1000 : 0;
  if (durationMs <= 0 || startCv === targetCv) {
    glideState.value = targetCv;
    glideState.frame = null;
    midiPitchGlideStates.set(key, glideState);
    propagate(widgetId, "pitchCv", targetCv, voiceIndex);
    return;
  }

  const startedAt = performance.now();
  const tick = (now) => {
    const progress = clamp((now - startedAt) / durationMs, 0, 1);
    glideState.value = startCv + (targetCv - startCv) * progress;
    propagate(widgetId, "pitchCv", glideState.value, voiceIndex);
    if (progress < 1) {
      glideState.frame = requestAnimationFrame(tick);
    } else {
      glideState.frame = null;
    }
  };
  midiPitchGlideStates.set(key, glideState);
  glideState.frame = requestAnimationFrame(tick);
}

function sendMidiTestEvent(widgetId, midiType, note, velocity = 100) {
  const state = midiInStates.get(widgetId);
  if (!state) return;
  const data = { type: "midi", midiType, channel: state.channel ?? 0, note, velocity };
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${window.location.host}/ws`);
  socket.addEventListener("open", () => {
    socket.send(JSON.stringify({ type: "midiTest", midiType, channel: data.channel, note, velocity }));
  });
  socket.addEventListener("message", (event) => {
    try {
      const response = JSON.parse(event.data);
      if (response.type === "midi") routeMidiToWidgets(response);
    } catch (err) {
      // Ignore malformed test responses.
    }
    socket.close();
  });
  socket.addEventListener("error", () => socket.close());
}

function testMidiInGlide(widgetId) {
  const state = midiInStates.get(widgetId);
  if (!state) return;
  for (const [key, glideState] of midiPitchGlideStates) {
    if (!key.startsWith(`${widgetId}:`)) continue;
    if (glideState.frame !== null) cancelAnimationFrame(glideState.frame);
    midiPitchGlideStates.delete(key);
  }
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${window.location.host}/ws`);
  const channel = state.channel ?? 0;
  const send = (midiType, note, velocity) => {
    socket.send(JSON.stringify({ type: "midiTest", midiType, channel, note, velocity }));
  };
  socket.addEventListener("open", () => {
    send("note_on", 60, 100);
    setTimeout(() => send("note_on", 72, 100), 150);
    setTimeout(() => {
      send("note_off", 72, 0);
      socket.close();
    }, Math.max(500, state.glideTime * 1000 + 400));
  });
  socket.addEventListener("message", (event) => {
    try {
      const response = JSON.parse(event.data);
      if (response.type === "midi") routeMidiToWidgets(response);
    } catch (err) {
      // Ignore malformed test responses.
    }
  });
  socket.addEventListener("error", () => socket.close());
}

function routeMidiToWidgets(data) {
  if (data.type !== "midi") return;
  if (data.midiType !== "note_on" && data.midiType !== "note_off" && data.midiType !== "control_change") return;

  
  
  
  
  document.querySelectorAll('.widget-instance[data-widget-type="midi-in"]').forEach((el) => {
    const id = el.id;
    const state = midiInStates.get(id);
    if (state && state.channel !== null && data.channel !== state.channel) return;

    const slotState = midiVoiceSlotStates.get(id);
    if (!state || !slotState) return;

    if (data.midiType === "note_on") {
      if (state.voices <= 1) {
        if (isFxMidiIn(id) && slotState.monoCurrentNote !== null) return;
        slotState.pendingRelease = false;
        slotState.holdCounter += 1;
        slotState.monoHeldNotes.set(data.note, { order: slotState.holdCounter, velocity: data.velocity });
        slotState.monoCurrentNote = data.note;
        propagateMidiPitchCv(id, noteToVoltage(data.note));
        propagate(id, "note", data.note);
        propagate(id, "velocity", velocityToVoltage(data.velocity));
        propagate(id, "gate", GATE_ON_VOLTS);
        deliverMidiSplitZones(id, data.note, data.velocity, true, undefined);
        return;
      }

      const voiceIndex = allocateVoice(state, slotState, data.note, (i) => audioEngine.isVoiceStillSounding(i));
      if (voiceIndex === null) return; 

      audioEngine.setMonitorVoice(voiceIndex);
      propagate(id, "pitchCv", noteToVoltage(data.note), voiceIndex);
      propagate(id, "note", data.note, voiceIndex);
      propagate(id, "velocity", velocityToVoltage(data.velocity), voiceIndex);
      propagate(id, "gate", GATE_ON_VOLTS, voiceIndex);
      deliverMidiSplitZones(id, data.note, data.velocity, true, voiceIndex);
    } else if (data.midiType === "note_off") {
      if (state.voices <= 1) {
        const wasCurrentNote = slotState.monoCurrentNote === data.note;
        slotState.monoHeldNotes.delete(data.note);
        if (slotState.pedalDown) {
          slotState.pendingRelease = slotState.monoHeldNotes.size === 0;
          return;
        }
        const fallbackNote = mostRecentHeldMonoNote(slotState);
        if (wasCurrentNote && fallbackNote !== null) {
          const fallback = slotState.monoHeldNotes.get(fallbackNote);
          slotState.monoCurrentNote = fallbackNote;
          propagateMidiPitchCv(id, noteToVoltage(fallbackNote));
          propagate(id, "note", fallbackNote);
          propagate(id, "velocity", velocityToVoltage(fallback.velocity));
          propagate(id, "gate", GATE_ON_VOLTS);
          deliverMidiSplitZones(id, data.note, data.velocity, false, undefined);
          deliverMidiSplitZones(id, fallbackNote, fallback.velocity, true, undefined);
          return;
        }
        if (!wasCurrentNote) return;
        slotState.monoCurrentNote = null;
        propagate(id, "gate", GATE_OFF_VOLTS);
        deliverMidiSplitZones(id, data.note, data.velocity, false, undefined);
        return;
      }

      const voiceIndex = findVoiceForNote(slotState, data.note);
      if (voiceIndex === null) return; 

      if (slotState.pedalDown) {
        slotState.slots[voiceIndex].held = true; 
        return;
      }
      slotState.slots[voiceIndex].note = null;
      propagate(id, "gate", GATE_OFF_VOLTS, voiceIndex);
      deliverMidiSplitZones(id, data.note, data.velocity, false, voiceIndex);
    } else if (data.midiType === "control_change") {
      if (data.control !== 64) return; 

      const wasDown = slotState.pedalDown;
      const nowDown = data.value >= 64; 
      slotState.pedalDown = nowDown;
      if (!wasDown || nowDown) return; 

      if (state.voices <= 1) {
        if (slotState.pendingRelease) {
          propagate(id, "gate", GATE_OFF_VOLTS);
          slotState.pendingRelease = false;
        }
        return;
      }

      slotState.slots.forEach((slot, voiceIndex) => {
        if (!slot.held) return;
        const releasedNote = slot.note;
        slot.note = null;
        slot.held = false;
        propagate(id, "gate", GATE_OFF_VOLTS, voiceIndex);
        if (releasedNote !== null) deliverMidiSplitZones(id, releasedNote, 0, false, voiceIndex);
      });
    }
  });
}

let warnedNoSf2MidiWire = false;






function routeMidiToSoundFontPlayer(data) {
  if (data.type !== "midi") return;
  const wires = connections.filter(
    (c) =>
      c.from.portId === "midi" &&
      c.to.portId === "midi" &&
      document.getElementById(c.to.widgetId)?.dataset.widgetType === "soundfont-player" &&
      document.getElementById(c.from.widgetId)?.dataset.widgetType === "midi-in"
  );
  if (
    wires.length === 0 &&
    !warnedNoSf2MidiWire &&
    document.querySelector('.widget-instance[data-widget-type="soundfont-player"]')
  ) {
    warnedNoSf2MidiWire = true;
    console.warn(
      'SoundFont Player: no "MIDI" wire from a Midi-In widget\'s MIDI output to the SoundFont Player\'s MIDI ' +
        "input - it needs this dedicated wire (separate from Note/Gate/Velocity) to receive notes at all."
    );
  }
  wires.forEach((c) => {
    const midiInState = midiInStates.get(c.from.widgetId);
    if (midiInState && midiInState.channel !== null && data.channel !== midiInState.channel) return;
    audioEngine.handleSoundFontMidi(c.to.widgetId, data);
  });
}



function applyFilterWidgetParams(id, params, curtis = false) {
  const state = filterStates.get(id);
  if (!state || !params) return;
  state.filterType = params.filterType || (curtis ? "lp24" : "lowpass");
  state.cutoff = params.cutoff ?? 5;
  state.cutoffMinHz = params.cutoffMinHz ?? 20;
  state.cutoffMaxHz = params.cutoffMaxHz ?? 20000;
  state.cutoffCurve = params.cutoffCurve === "linear" ? "linear" : "log";
  state.resonance = params.resonance ?? 5;
  state.resonanceMinQ = params.resonanceMinQ ?? 0.1;
  state.resonanceMaxQ = params.resonanceMaxQ ?? 20;
  state.resonanceCurve = params.resonanceCurve === "linear" ? "linear" : "log";
  state.coeffStride = clamp(Math.round(params.coeffStride ?? 4), 1, 16);
  state.global = params.global === true;
  state.bypass = params.bypass === true;
  audioEngine.setParam(id, "filterType", state.filterType);
  audioEngine.setParam(id, "cutoffMinHz", state.cutoffMinHz);
  audioEngine.setParam(id, "cutoffMaxHz", state.cutoffMaxHz);
  audioEngine.setParam(id, "cutoffCurve", state.cutoffCurve);
  audioEngine.setParam(id, "resonanceMinQ", state.resonanceMinQ);
  audioEngine.setParam(id, "resonanceMaxQ", state.resonanceMaxQ);
  audioEngine.setParam(id, "resonanceCurve", state.resonanceCurve);
  audioEngine.setParam(id, "coeffStride", state.coeffStride);
  audioEngine.setParam(id, "cutoff", state.cutoff);
  audioEngine.setParam(id, "resonance", state.resonance);
  audioEngine.setParam(id, "global", state.global);
  audioEngine.setParam(id, "bypass", state.bypass);
  updateFilterVisual(id);
}

const WIDGET_ENGINES = {
  annotation: {
    getParams: (id) => getAnnotationParams(id),
    applyParams: (id, params) => applyAnnotationParams(id, params),
  },
  debug: {
    getParams: () => ({}),
    applyParams: () => {},
  },
  spectrum: {
    getParams: (id) => ({ rangeVolts: audioMonitorStates.get(id)?.rangeVolts ?? AUDIO_PEAK_VOLTS }),
    applyParams: (id, params) => setAudioMonitorRange(id, params?.rangeVolts, false),
  },
  "midi-in": {
    getParams: (id) => {
      const state = midiInStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = midiInStates.get(id);
      if (!state || !params) return;
      Object.assign(state, params);
      state.voices = clamp(Math.round(state.voices ?? 1), MIDI_IN_MIN_VOICES, midiInVoiceLimit(id));
      if (isFxMidiIn(id)) state.overflowMode = "ignore";
      state.glide = state.glide === true;
      state.glideTime = clamp(Number(state.glideTime) || 0, 0, 2);
      resizeVoiceSlots(id, state.voices);
      updateMidiInControls(id);
    },
  },
  "midi-split": {
    getParams: (id) => {
      const state = midiSplitStates.get(id);
      return state
        ? { zoneCount: state.zoneCount, zones: state.zones.map((z) => ({ from: z.from, to: z.to, octave: z.octave })) }
        : {};
    },
    applyParams: (id, params) => {
      const state = midiSplitStates.get(id);
      if (!state || !params) return;
      state.zoneCount = clamp(params.zoneCount ?? 0, 0, MIDI_SPLIT_MAX_ZONES);
      const loadedZones = Array.isArray(params.zones) ? params.zones : [];
      for (let z = 0; z < MIDI_SPLIT_MAX_ZONES; z += 1) {
        const loaded = loadedZones[z];
        state.zones[z] = {
          from: clamp(loaded && loaded.from !== undefined ? loaded.from : 0, 0, 127),
          to: clamp(loaded && loaded.to !== undefined ? loaded.to : 127, 0, 127),
          octave: clamp(loaded && loaded.octave !== undefined ? loaded.octave : 0, -4, 4),
        };
      }
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderMidiSplitPorts(id, portsEl);
    },
  },
  knob: {
    getParams: (id) => {
      const state = knobStates.get(id);
      if (!state) return {};
      
      
      return { ...state, valueList: parseValueList(state.listText) || [] };
    },
    applyParams: (id, params) => {
      const state = knobStates.get(id);
      if (state && params) {
        Object.assign(state, params);
        audioEngine.setParam(id, "value", mapKnobValue(state));
        updateKnobVisual(id);
      }
    },
  },
  button: {
    getParams: (id) => {
      const state = buttonStates.get(id);
      if (!state) return {};
      return { ...state, value0: clampVoltage(state.value0), value127: clampVoltage(state.value127, CV_MAX_VOLTS), valueList: parseVoltageValueList(state.listText) || [] };
    },
    applyParams: (id, params) => {
      const state = buttonStates.get(id);
      if (state && params) {
        Object.assign(state, params);
        state.value0 = clampVoltage(state.value0);
        state.value127 = clampVoltage(state.value127, CV_MAX_VOLTS);
        const list = parseVoltageValueList(state.listText);
        if (list) state.listText = list.join(", ");
        audioEngine.setParam(id, "value", buttonCurrentOutput(state));
        updateButtonVisual(id);
      }
    },
  },
  "up-down-button": {
    getParams: (id) => {
      const state = upDownButtonStates.get(id);
      if (!state) return {};
      return { ...state, min: clampVoltage(state.min), max: clampVoltage(state.max, CV_MAX_VOLTS), step: clampVoltage(state.step, 1), valueList: parseVoltageValueList(state.listText) || [] };
    },
    applyParams: (id, params) => {
      const state = upDownButtonStates.get(id);
      if (state && params) {
        Object.assign(state, params);
        state.min = clampVoltage(state.min);
        state.max = clampVoltage(state.max, CV_MAX_VOLTS);
        if (state.max < state.min) [state.min, state.max] = [state.max, state.min];
        state.step = clampVoltage(state.step, 1);
        const list = parseVoltageValueList(state.listText);
        if (list) state.listText = list.join(", ");
        audioEngine.setParam(id, "value", upDownButtonCurrentOutput(state));
        updateUpDownButtonVisual(id);
      }
    },
  },
  switch: {
    getParams: (id) => {
      const state = switchStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = switchStates.get(id);
      if (state && params) {
        Object.assign(state, params);
        state.in1Default = state.in1Default ?? 0;
        state.in2Default = state.in2Default ?? 0;
        audioEngine.setParam(id, "selectIn2", state.selectIn2);
        audioEngine.setParam(id, "in1Default", state.in1Default);
        audioEngine.setParam(id, "in2Default", state.in2Default);
        updateSwitchVisual(id);
      }
    },
  },
  multiplier: {
    getParams: (id) => {
      const state = multiplierStates.get(id);
      return state ? { inputCount: state.inputCount, values: [...state.values] } : {};
    },
    applyParams: (id, params) => {
      const state = multiplierStates.get(id);
      if (!state || !params) return;
      state.inputCount = clamp(
        params.inputCount || 2,
        MULTIPLIER_MIN_INPUTS,
        MULTIPLIER_MAX_INPUTS
      );
      if (Array.isArray(params.values)) state.values = params.values.slice();
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderMultiplierPorts(id, portsEl);
    },
  },
  adder: {
    getParams: (id) => {
      const state = adderStates.get(id);
      return state ? { inputCount: state.inputCount, values: [...state.values] } : {};
    },
    applyParams: (id, params) => {
      const state = adderStates.get(id);
      if (!state || !params) return;
      state.inputCount = clamp(params.inputCount || 2, ADDER_MIN_INPUTS, ADDER_MAX_INPUTS);
      if (Array.isArray(params.values)) state.values = params.values.slice();
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderAdderPorts(id, portsEl);
    },
  },
  divide: {
    getParams: (id) => {
      const state = divideStates.get(id);
      return state ? { values: [...state.values] } : {};
    },
    applyParams: (id, params) => {
      const state = divideStates.get(id);
      if (!state || !params) return;
      if (Array.isArray(params.values)) state.values = params.values.slice();
      recomputeDivide(id);
    },
  },
  "frequency-divider": {
    getParams: (id) => {
      const state = frequencyDividerStates.get(id);
      return state ? { divider: state.divider } : {};
    },
    applyParams: (id, params) => {
      const state = frequencyDividerStates.get(id);
      if (!state || !params) return;
      state.divider = nearestFrequencyDivider(params.divider ?? 2);
      updateFrequencyDividerVisual(id);
      recomputeFrequencyDivider(id);
    },
  },
  abs: {
    getParams: () => ({}),
    applyParams: () => {},
  },
  "value-mapper": {
    getParams: (id) => {
      const state = valueMapperStates.get(id);
      if (!state) return {};
      const { point1In, point1Out, point2In, point2Out, constraintMin, constraintMax, listText } = state;
      return { point1In, point1Out, point2In, point2Out, constraintMin, constraintMax, listText };
    },
    applyParams: (id, params) => {
      const state = valueMapperStates.get(id);
      if (!state || !params) return;
      state.point1In = params.point1In ?? 0;
      state.point1Out = params.point1Out ?? 0;
      state.point2In = params.point2In ?? 127;
      state.point2Out = params.point2Out ?? CV_MAX_VOLTS;
      state.constraintMin = params.constraintMin ?? null;
      state.constraintMax = params.constraintMax ?? null;
      state.listText = params.listText || "";
      updateValueMapperVisual(id);
    },
  },
  clock: {
    getParams: (id) => {
      const state = clockStates.get(id);
      return state
        ? {
            cvVolts: state.cvVolts,
            minBpm: state.minBpm,
            maxBpm: state.maxBpm,
            pulseMs: state.pulseMs,
          }
        : {};
    },
    applyParams: (id, params) => {
      const state = clockStates.get(id);
      if (!state || !params) return;
      state.cvVolts = params.cvVolts ?? (params.raw != null ? (params.raw / 127) * CV_MAX_VOLTS : 0);
      state.cvVolts = clamp(state.cvVolts, 0, CV_MAX_VOLTS);
      state.minBpm = params.minBpm ?? 0;
      state.maxBpm = params.maxBpm ?? 240;
      state.pulseMs = params.pulseMs ?? 100;
      state.bpm = clockBpm(state.cvVolts, state.minBpm, state.maxBpm);
      restartClockTimer(id);
    },
  },
  lfo: {
    getParams: (id) => {
      const state = lfoStates.get(id);
      return state
        ? {
            waveType: state.waveType,
            rate: state.rate,
            maxRate: state.maxRate,
            amplitude: state.amplitude,
            phase: state.phase,
            zeroPointFixed: state.zeroPointFixed,
            zeroPoint: state.zeroPoint,
            bipolar: state.bipolar,
            oneShot: state.oneShot,
            global: state.global,
          }
        : {};
    },
    applyParams: (id, params) => {
      const state = lfoStates.get(id);
      if (!state || !params) return;
      state.waveType = params.waveType || "sine";
      state.maxRate = clamp(params.maxRate ?? LFO_MAX_RATE_HZ, 0, LFO_MAX_RATE_HZ);
      state.rate = clamp(params.rate ?? 2, 0, state.maxRate);
      state.amplitude = params.amplitude ?? CV_MAX_VOLTS;
      state.phase = clamp(params.phase ?? 0, -180, 180);
      state.zeroPointFixed = params.zeroPointFixed === true;
      state.zeroPoint = params.zeroPoint ?? 5;
      state.bipolar = params.bipolar === true;
      state.oneShot = params.oneShot === true;
      state.global = params.global === true;
      
      
      state.oneShotDone = true;
      state.oneShotAccum = 0;
      audioEngine.setParam(id, "waveType", state.waveType);
      audioEngine.setParam(id, "maxRate", state.maxRate);
      audioEngine.setParam(id, "rate", state.rate);
      audioEngine.setParam(id, "amplitude", state.amplitude);
      audioEngine.setParam(id, "phase", state.phase);
      audioEngine.setParam(id, "zeroPointFixed", state.zeroPointFixed);
      audioEngine.setParam(id, "zeroPoint", state.zeroPoint);
      audioEngine.setParam(id, "bipolar", state.bipolar);
      audioEngine.setParam(id, "oneShot", state.oneShot);
      audioEngine.setParam(id, "global", state.global);
      updateLfoWaveButtons(id);
      updateLfoVisual(id);
    },
  },
  oscillator: {
    getParams: (id) => {
      const state = oscillatorStates.get(id);
      if (!state) return {};
      return {
        waveType: state.waveType,
        amplitude: state.amplitude,
        tune: state.tuneVolts,
        gain: state.gain,
      };
    },
    applyParams: (id, params) => {
      const state = oscillatorStates.get(id);
      if (!state || !params) return;
      const center = CV_MAX_VOLTS / 2;
      state.waveType = params.waveType || "sine";
      state.amplitude = params.amplitude ?? 1;
      state.tuneVolts = params.tune ?? center;
      state.gain = params.gain ?? CV_MAX_VOLTS;
      state.pitchCv = center;
      audioEngine.setParam(id, "waveType", state.waveType);
      audioEngine.setParam(id, "amplitude", 0, 0);
      audioEngine.setParam(id, "gain", state.gain);
      recomputeOscillatorFreq(id, undefined);
      updateOscillatorTuneVisual(id);
      updateOscillatorGainVisual(id);
      updateOscillatorWaveButtons(id);
    },
  },
  hammond: {
    getParams: (id) => {
      const state = hammondStates.get(id);
      if (!state) return {};
      return {
        amplitude: state.amplitude,
        tune: state.tuneVolts,
        gain: state.gain,
        drawbars: [...state.drawbars],
        ratios: [...state.ratios],
        modulationStride: state.modulationStride,
      };
    },
    applyParams: (id, params) => {
      const state = hammondStates.get(id);
      if (!state || !params) return;
      const center = CV_MAX_VOLTS / 2;
      state.amplitude = params.amplitude ?? 1;
      state.tuneVolts = params.tune ?? center;
      state.gain = params.gain ?? CV_MAX_VOLTS;
      state.pitchCv = center;
      state.drawbars =
        Array.isArray(params.drawbars) && params.drawbars.length === 9
          ? params.drawbars.map((v) => clamp(Number(v) || 0, 0, CV_MAX_VOLTS))
          : [...HAMMOND_DEFAULT_DRAWBARS];
      state.ratios =
        Array.isArray(params.ratios) && params.ratios.length === 9
          ? params.ratios.map((v) => clamp(Number(v) || 0.01, 0.01, 32))
          : [...HAMMOND_DEFAULT_RATIOS];
      const modulationStride = Math.round(params.modulationStride ?? 4);
      state.modulationStride = [1, 2, 4, 8, 16].includes(modulationStride) ? modulationStride : 4;
      audioEngine.setParam(id, "amplitude", 0, 0);
      audioEngine.setParam(id, "gain", state.gain);
      audioEngine.setParam(id, "modulationStride", state.modulationStride);
      state.drawbars.forEach((v, i) => audioEngine.setParam(id, `drawbar${i + 1}`, v));
      state.ratios.forEach((v, i) => audioEngine.setParam(id, `ratio${i + 1}`, v));
      recomputeHammondFreq(id, undefined);
      updateHammondVisual(id);
    },
  },
  "wave-player": {
    getParams: (id) => {
      const state = wavePlayerStates.get(id);
      return state ? { sampleFile: state.sampleFile, rootNoteMidi: state.rootNoteMidi } : {};
    },
    applyParams: (id, params) => {
      const state = wavePlayerStates.get(id);
      if (!state || !params) return;
      state.sampleFile = typeof params.sampleFile === "string" ? params.sampleFile : "";
      state.rootNoteMidi = params.rootNoteMidi ?? 60;
      audioEngine.setParam(id, "sampleFile", state.sampleFile);
      audioEngine.setParam(id, "rootNoteMidi", state.rootNoteMidi);
      updateWavePlayerLabel(id);
    },
  },
  "soundfont-player": {
    getParams: (id) => {
      const state = soundFontPlayerStates.get(id) || {};
      return {
        volume: state.volume || "flash",
        path: state.path || "",
        bank: state.bank ?? 0,
        program: state.program ?? 0,
        maxVoices: state.maxVoices ?? 32,
        enableCc: state.enableCc === true,
        enableModWheel: state.enableModWheel === true,
        enablePitchWheel: state.enablePitchWheel === true,
        gain: state.gain ?? CV_MAX_VOLTS,
      };
    },
    applyParams: (id, params) => {
      const state = soundFontPlayerStates.get(id);
      if (!state || !params) return;
      state.volume = params.volume === "sd" ? "sd" : "flash";
      state.path = typeof params.path === "string" ? params.path : "";
      state.bank = clamp(params.bank ?? 0, 0, SOUNDFONT_DEFAULT_MAX_BANK);
      state.program = clamp(params.program ?? 0, 0, 127);
      state.maxVoices = clamp(params.maxVoices ?? 32, 4, 128);
      state.enableCc = params.enableCc === true;
      state.enableModWheel = params.enableModWheel === true;
      state.enablePitchWheel = params.enablePitchWheel === true;
      state.gain = clamp(params.gain ?? CV_MAX_VOLTS, 0, CV_MAX_VOLTS);
      audioEngine.setSoundFontParams(id, {
        volume: state.volume, path: state.path, bank: state.bank, program: state.program,
        maxVoices: state.maxVoices, enableCc: state.enableCc, enableModWheel: state.enableModWheel,
        enablePitchWheel: state.enablePitchWheel, gain: state.gain,
      });
      refreshSoundFontPlayerMeta(id);
      updateSoundFontPlayerControls(id);
    },
  },
  "audio-out": {
    getParams: (id) => ({ ...audioOutStates.get(id) }),
    applyParams: (id, params) => setAudioOutParams(id, params || {}, false),
  },
  "usb-out": {
    getParams: (id) => ({ ...audioOutStates.get(id) }),
    applyParams: (id, params) => setAudioOutParams(id, params || {}, false),
  },
  "es8311-out": {
    getParams: (id) => ({ ...audioOutStates.get(id) }),
    applyParams: (id, params) => setAudioOutParams(id, params || {}, false),
  },
  "wm8960-out": {
    getParams: (id) => ({ ...audioOutStates.get(id) }),
    applyParams: (id, params) => setAudioOutParams(id, params || {}, false),
  },
  "audio-output-link": {
    getParams: () => ({}),
    applyParams: () => {},
  },
  "audio-input-link": {
    getParams: (id) => {
      const state = audioInputLinkStates.get(id);
      return state ? { gains: [...state.gains] } : {};
    },
    applyParams: (id, params) => {
      const state = audioInputLinkStates.get(id);
      if (!state) return;
      if (Array.isArray(params?.gains)) {
        for (let channel = 0; channel < LINK_BUS_COUNT; channel += 1) {
          state.gains[channel] = clamp(Number(params.gains[channel] ?? CV_MAX_VOLTS), 0, CV_MAX_VOLTS);
          audioEngine.setParam(id, `gain${channel + 1}`, state.gains[channel]);
        }
      }
      updateAudioInputLinkVisual(id);
    },
  },
  detune: {
    getParams: (id) => {
      const state = detuneStates.get(id);
      return state ? { amount: state.amount } : {};
    },
    applyParams: (id, params) => {
      const state = detuneStates.get(id);
      if (!state || !params) return;
      state.amount = clamp(params.amount ?? 0, -1, 1);
      updateDetuneVisual(id);
    },
  },
  gain: {
    getParams: (id) => {
      const state = gainStates.get(id);
      return state ? { amount: state.amount, curve: state.curve, global: state.global } : {};
    },
    applyParams: (id, params) => {
      const state = gainStates.get(id);
      if (!state || !params || typeof params.amount !== "number") return;
      
      
      
      
      const isLegacy = params.curve === undefined;
      state.amount = clamp(isLegacy ? params.amount * CV_MAX_VOLTS : params.amount, 0, CV_MAX_VOLTS);
      state.curve = params.curve === "exponential" ? "exponential" : "linear";
      state.global = params.global === true;
      audioEngine.setParam(id, "amount", state.amount);
      audioEngine.setParam(id, "curve", state.curve);
      audioEngine.setParam(id, "global", state.global);
      updateGainVisual(id);
    },
  },
  filter: {
    getParams: (id) => {
      const state = filterStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => applyFilterWidgetParams(id, params, false),
  },
  "curtis-filter": {
    getParams: (id) => {
      const state = filterStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => applyFilterWidgetParams(id, params, true),
  },
  ...Object.fromEntries(Object.keys(ESP_EFFECT_DEFAULTS).map((type) => [type, {
    getParams: (id) => ({ ...(espEffectStates.get(id) || ESP_EFFECT_DEFAULTS[type]) }),
    applyParams: (id, params) => {
      const state = espEffectStates.get(id);
      if (!state || !params) return;
      Object.assign(state, params);
      Object.entries(state).forEach(([name, value]) => audioEngine.setParam(id, name, value));
      updateEspEffectVisual(id);
    },
  }])),
  mixer: {
    getParams: (id) => {
      const state = mixerStates.get(id);
      return state ? { inputCount: state.inputCount, gains: [...state.gains] } : {};
    },
    applyParams: (id, params) => {
      const state = mixerStates.get(id);
      if (!state || !params) return;
      state.inputCount = clamp(params.inputCount || 2, MIXER_MIN_INPUTS, MIXER_MAX_INPUTS);
      state.gains =
        Array.isArray(params.gains) && params.gains.length === MIXER_MAX_INPUTS
          ? params.gains.slice()
          : new Array(MIXER_MAX_INPUTS).fill(1);
      for (let i = 1; i <= state.inputCount; i += 1) {
        audioEngine.setParam(id, `gain${i}`, state.gains[i - 1]);
      }
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderMixerPorts(id, portsEl);
    },
  },
  multiplexer: {
    getParams: (id) => {
      const state = multiplexerStates.get(id);
      return state
        ? { inputCount: state.inputCount, select: state.manualSelect, softFadeMs: state.softFadeMs }
        : {};
    },
    applyParams: (id, params) => {
      const state = multiplexerStates.get(id);
      if (!state || !params) return;
      state.inputCount = clamp(params.inputCount || 2, MULTIPLEXER_MIN_INPUTS, MULTIPLEXER_MAX_INPUTS);
      const softFadeMs = Number(params.softFadeMs ?? MULTIPLEXER_DEFAULT_SOFT_FADE_MS);
      state.softFadeMs = Number.isFinite(softFadeMs)
        ? clamp(softFadeMs, 0, MULTIPLEXER_MAX_SOFT_FADE_MS)
        : MULTIPLEXER_DEFAULT_SOFT_FADE_MS;
      state.manualSelect = clamp(parseInt(params.select, 10) || 0, 0, state.inputCount - 1);
      audioEngine.setParam(id, "select", state.manualSelect);
      audioEngine.setParam(id, "softFadeMs", state.softFadeMs);
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderMultiplexerPorts(id, portsEl);
      multiplexerSetActiveIndex(id, state.manualSelect);
    },
  },
  adsr: {
    getParams: (id) => {
      const state = adsrStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = adsrStates.get(id);
      if (!state || !params) return;
      state.attack = params.attack ?? 0.4;
      state.decay = params.decay ?? 0.4;
      state.sustain = params.sustain ?? 0.7;
      state.release = params.release ?? 0.6;
      state.attackMaxSeconds = params.attackMaxSeconds ?? MAX_ENVELOPE_SECONDS;
      state.attackCurve = params.attackCurve === "log" ? "log" : "linear";
      state.decayMaxSeconds = params.decayMaxSeconds ?? MAX_ENVELOPE_SECONDS;
      state.decayCurve = params.decayCurve === "log" ? "log" : "linear";
      state.releaseMaxSeconds = params.releaseMaxSeconds ?? MAX_ENVELOPE_SECONDS;
      state.releaseCurve = params.releaseCurve === "log" ? "log" : "linear";
      audioEngine.setParam(id, "attack", state.attack);
      audioEngine.setParam(id, "decay", state.decay);
      audioEngine.setParam(id, "sustain", state.sustain);
      audioEngine.setParam(id, "release", state.release);
      updateAdsrSliders(id);
    },
  },
  const: {
    getParams: (id) => {
      const state = constStates.get(id);
      return state ? { value: state.value } : {};
    },
    applyParams: (id, params) => {
      const state = constStates.get(id);
      if (!state || !params) return;
      state.value = typeof params.value === "number" ? params.value : 0;
      audioEngine.setParam(id, "value", state.value);
      updateConstInput(id);
    },
  },
  chorus: {
    getParams: (id) => {
      const state = chorusStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = chorusStates.get(id);
      if (!state || !params) return;
      state.rate = params.rate ?? 1.5;
      state.depth = params.depth ?? 0.003;
      state.offset = params.offset ?? 0.02;
      state.bypass = params.bypass === true;
      audioEngine.setParam(id, "rate", state.rate);
      audioEngine.setParam(id, "depth", state.depth);
      audioEngine.setParam(id, "offset", state.offset);
      audioEngine.setParam(id, "bypass", state.bypass);
      updateChorusVisual(id);
    },
  },
  delay: makeEffectCvEngine("delay"),
  reverb: makeEffectCvEngine("reverb"),
  "reverb-light": makeEffectCvEngine("reverb-light"),
};

function getWidgetParams(type, id) {
  const engine = WIDGET_ENGINES[type];
  return engine ? engine.getParams(id) : {};
}

function applyWidgetParams(type, id, params) {
  const engine = WIDGET_ENGINES[type];
  if (engine) engine.applyParams(id, params);
}



const SCHEMA_VERSION = 2;
let restoringPreset = false;

function serializePreset() {
  
  
  
  
  const serializePage = (name) => {
    const p = pages[name];
    const widgetIds = new Set();
    const widgets = [];
    p.canvasContentEl.querySelectorAll(".widget-instance").forEach((el) => {
      widgetIds.add(el.id);
      const labelSpan = el.querySelector(".widget-instance-label");
      const type = el.dataset.widgetType;
      
      
      
      
      const x = parseInt(el.style.left, 10) || 0;
      const y = parseInt(el.style.top, 10) || 0;
      widgets.push({
        id: el.id,
        type,
        label: labelSpan ? labelSpan.textContent : type,
        ui: { x, y },
        params: getWidgetParams(type, el.id),
      });
    });
    const pageConnections = connections.filter((c) => widgetIds.has(c.from.widgetId));
    return { widgets, connections: pageConnections };
  };

  return {
    schemaVersion: SCHEMA_VERSION,
    pages: { generator: serializePage("generator"), fx: serializePage("fx") },
  };
}


let currentPresetName = null;

async function saveToServer(name) {
  try {
    const url = name ? `/api/preset?name=${encodeURIComponent(name)}` : "/api/preset";
    const res = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(serializePreset()),
    });
    if (!res.ok) {
      appendLog(`Preset could not be saved: HTTP ${res.status}`);
      return false;
    }
    return true;
  } catch (err) {
    appendLog(`Preset could not be saved: ${err.message}`);
    return false;
  }
}

async function savePresetNow() {
  
  
  
  
  
  if (restoringPreset) return;
  await saveToServer(currentPresetName);
}







async function fetchWithRetry(url, options, retries = 2, delayMs = 400) {
  for (let attempt = 0; ; attempt += 1) {
    try {
      const res = await fetch(url, options);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return res;
    } catch (err) {
      if (attempt >= retries) throw err;
      await new Promise((resolve) => setTimeout(resolve, delayMs));
    }
  }
}

async function fetchPresetList() {
  try {
    const res = await fetchWithRetry("/api/presets");
    const data = await res.json();
    return Array.isArray(data.presets) ? data.presets : [];
  } catch (err) {
    appendLog(`Preset list could not be loaded: ${err.message}`);
    showToast("Could not load preset list");
    return [];
  }
}

async function refreshPresetMenu() {
  const names = await fetchPresetList();
  presetSelect.innerHTML = "";

  const unnamedOption = document.createElement("option");
  unnamedOption.value = "";
  unnamedOption.textContent = "New / unsaved";
  presetSelect.appendChild(unnamedOption);

  names.forEach((name) => {
    const opt = document.createElement("option");
    opt.value = name;
    opt.textContent = name;
    presetSelect.appendChild(opt);
  });
  presetSelect.value = currentPresetName || "";
}

async function handleExplicitSave() {
  if (currentPresetName === null) {
    handleSaveAs();
    return;
  }
  const ok = await saveToServer(currentPresetName);
  if (ok) {
    showToast("Preset saved");
  } else {
    appendLog("Preset could not be saved.");
  }
}

function handleNewPreset() {
  const hasWidgets =
    pages.generator.canvasContentEl.querySelectorAll(".widget-instance").length > 1 ||
    pages.fx.canvasContentEl.querySelectorAll(".widget-instance").length > 1;
  if (currentPresetName === null && hasWidgets) {
    if (!confirm("Discard unsaved preset?")) return;
  }
  
  
  
  currentPresetName = null;
  loadInitTemplate();
  refreshPresetMenu();
}










function markDirty() {}






function wsSend(obj) {
  if (currentSocket && currentSocket.readyState === WebSocket.OPEN) {
    currentSocket.send(JSON.stringify(obj));
  }
}

document.getElementById("panicBtn").addEventListener("click", () => {
  wsSend({ type: "panic" });
  showToast("Panic: MIDI reset and preset reload requested");
});

function sendLiveParamUpdate(widgetId, params) {
  wsSend({ type: "paramUpdate", widgetId, params });
}





const pendingLiveParams = new Map(); 
let liveParamFlushScheduled = false;
function sendLiveParamUpdateThrottled(widgetId, params) {
  const existing = pendingLiveParams.get(widgetId) || {};
  pendingLiveParams.set(widgetId, Object.assign(existing, params));
  if (!liveParamFlushScheduled) {
    liveParamFlushScheduled = true;
    requestAnimationFrame(() => {
      liveParamFlushScheduled = false;
      pendingLiveParams.forEach((p, id) => sendLiveParamUpdate(id, p));
      pendingLiveParams.clear();
    });
  }
}

const LEGACY_PARAM_KEYS = {
  knob: "knob",
  "midi-in": "midiIn",
  multiplier: "multiplier",
  clock: "clock",
  oscillator: "oscillator",
};

function widgetParamsFromLegacy(w) {
  if (w.params) return w.params;
  const legacyKey = LEGACY_PARAM_KEYS[w.type];
  return (legacyKey && w[legacyKey]) || {};
}








function normalizePresetData(data) {
  if (data && data.pages && data.pages.generator && data.pages.fx) {
    return {
      generator: {
        widgets: Array.isArray(data.pages.generator.widgets) ? data.pages.generator.widgets : [],
        connections: Array.isArray(data.pages.generator.connections)
          ? data.pages.generator.connections
          : [],
      },
      fx: {
        widgets: Array.isArray(data.pages.fx.widgets) ? data.pages.fx.widgets : [],
        connections: Array.isArray(data.pages.fx.connections) ? data.pages.fx.connections : [],
      },
    };
  }

  const widgets = Array.isArray(data.widgets) ? data.widgets : [];
  const oldConnections = Array.isArray(data.connections) ? data.connections : [];

  const generatorWidgets = [];
  const fxWidgets = [];
  const pageOfWidgetId = new Map();

  widgets.forEach((w) => {
    const targetPage = GENERATOR_PAGE_TYPES.includes(w.type) ? "generator" : "fx";
    pageOfWidgetId.set(w.id, targetPage);
    (targetPage === "generator" ? generatorWidgets : fxWidgets).push(w);
  });

  const generatorConnections = [];
  const fxConnections = [];
  let droppedCount = 0;

  oldConnections.forEach((c) => {
    const fromPage = pageOfWidgetId.get(c.from.widgetId);
    const toPage = pageOfWidgetId.get(c.to.widgetId);
    if (fromPage && fromPage === toPage) {
      (fromPage === "generator" ? generatorConnections : fxConnections).push(c);
    } else {
      droppedCount += 1;
    }
  });

  if (widgets.length > 0) {
    let msg =
      "Migrated to two pages: existing preset was automatically split by type into " +
      "\"Sound Generation\"/\"FX & Output\".";
    if (droppedCount > 0) {
      msg += ` ${droppedCount} connection(s) would have bridged both pages and were removed - please rewire them via Audio-Output-Link/Audio-Input-Link.`;
    }
    appendLog(msg);
  }

  return {
    generator: { widgets: generatorWidgets, connections: generatorConnections },
    fx: { widgets: fxWidgets, connections: fxConnections },
  };
}

function ensureSingletonLinkWidget(pageName) {
  const type = pageName === "generator" ? "audio-output-link" : "audio-input-link";
  const exists = pages[pageName].canvasContentEl.querySelector(`[data-widget-type="${type}"]`);
  if (!exists) {
    createWidget(type, WIDGET_TYPES[type].label, 40, 40);
  }
}









function fitPageToView() {
  const widgetEls = Array.from(canvasContent.querySelectorAll(".widget-instance"));
  if (widgetEls.length === 0) return;

  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  widgetEls.forEach((el) => {
    const x0 = el.offsetLeft;
    const y0 = el.offsetTop;
    minX = Math.min(minX, x0);
    minY = Math.min(minY, y0);
    maxX = Math.max(maxX, x0 + el.offsetWidth);
    maxY = Math.max(maxY, y0 + el.offsetHeight);
  });

  const boxWidth = maxX - minX;
  const boxHeight = maxY - minY;
  const viewportWidth = canvas.clientWidth;
  const viewportHeight = canvas.clientHeight;
  if (boxWidth <= 0 || boxHeight <= 0 || viewportWidth <= 0 || viewportHeight <= 0) return;

  const FIT_PADDING = 0.9; 
  const fitZoom = Math.min((viewportWidth * FIT_PADDING) / boxWidth, (viewportHeight * FIT_PADDING) / boxHeight);
  zoom = clamp(fitZoom, ZOOM_MIN, 1);

  const centerX = (minX + maxX) / 2;
  const centerY = (minY + maxY) / 2;
  panX = viewportWidth / 2 - zoom * centerX;
  panY = viewportHeight / 2 - zoom * centerY;

  applyTransform();
}

async function applyPresetData(data, { fitToView = false } = {}) {
  const pageData = normalizePresetData(data);
  let removedAudioToValueWidgets = false;
  ["generator", "fx"].forEach((name) => {
    const page = pageData[name];
    const removedIds = new Set();
    page.widgets = page.widgets.filter((widget) => {
      if (widget.type === "audio-to-value") {
        removedIds.add(widget.id);
        removedAudioToValueWidgets = true;
        return false;
      }
      return true;
    });
    page.connections = page.connections.filter(
      (connection) => !removedIds.has(connection.from.widgetId) && !removedIds.has(connection.to.widgetId)
    );
  });
  let migratedLfoAudioConnections = false;
  let migratedKnobAudioConnections = false;
  let migratedButtonAudioConnections = false;
  let migratedConstAudioConnections = false;
  let migratedFrequencyDividerCvConnections = false;
  let migratedLinkBusConnections = false;
  let migratedLinkBusWidgets = false;
  let migratedSoundFontMidiConnections = false;
  let migratedKnobValueConnections = false;
  let migratedOscilloscopeInConnections = false;

  restoringPreset = true;
  audioEngine.resetLoadTracking();
  showPresetLoadProgressIndeterminate("Loading preset…");

  
  
  
  
  switchPage("generator");
  clearCanvas();
  switchPage("fx");
  clearCanvas();

  
  
  
  
  
  ["generator", "fx"].forEach((name) => {
    pageData[name].widgets.forEach((w) => {
      if (w.type === "audio-output-link" && w.label !== "To Master") {
        w.label = "To Master";
        migratedLinkBusWidgets = true;
      }
      if (w.type === "audio-input-link" && w.label !== "Audio Outputs") {
        w.label = "Audio Outputs";
        migratedLinkBusWidgets = true;
      }
      const num = parseInt(String(w.id).replace("widget-", ""), 10);
      if (!Number.isNaN(num)) widgetCounter = Math.max(widgetCounter, num);
    });
  });

  
  ["generator", "fx"].forEach((name) => {
    switchPage(name);
    const loadedLinkTypes = new Set();
    pageData[name].widgets.forEach((w) => {
      if (LINK_WIDGET_TYPES.includes(w.type)) {
        if (loadedLinkTypes.has(w.type)) return;
        loadedLinkTypes.add(w.type);
      }
      const label = w.label || (WIDGET_TYPES[w.type] || {}).label || w.type;
      const x = w.ui ? w.ui.x : w.x;
      const y = w.ui ? w.ui.y : w.y;
      createWidget(w.type, label, x, y, w.id);
      applyWidgetParams(w.type, w.id, widgetParamsFromLegacy(w));
    });
    ensureSingletonLinkWidget(name);
    if (fitToView) fitPageToView();
  });

  
  
  connections = dedupeConnections([
    ...(Array.isArray(pageData.generator.connections) ? pageData.generator.connections : []),
    ...(Array.isArray(pageData.fx.connections) ? pageData.fx.connections : []),
  ]).filter((c) => {
    const source = document.getElementById(c.from.widgetId);
    return !(source?.dataset.widgetType === "knob" && c.from.portId === "cc");
  });
  connections.forEach((c) => {
    const num = parseInt(String(c.id).replace("conn-", ""), 10);
    if (!Number.isNaN(num)) connectionCounter = Math.max(connectionCounter, num);

    
    
    
    
    
    
    const fromEl = document.getElementById(c.from.widgetId);
    if (fromEl && fromEl.dataset.widgetType === "lfo" && c.from.portId === "audio") {
      c.from.portId = "value";
      migratedLfoAudioConnections = true;
    }
  });

  
  
  document.querySelectorAll('.widget-instance[data-widget-type="soundfont-player"]').forEach((sfEl) => {
    const old = connections.filter((c) => c.to.widgetId === sfEl.id && ["note", "gate", "amplitude"].includes(c.to.portId));
    if (!old.length) return;
    const source = old.map((c) => c.from.widgetId).find((id) => document.getElementById(id)?.dataset.widgetType === "midi-in");
    connections = connections.filter((c) => !old.includes(c));
    if (source && !connections.some((c) => c.from.widgetId === source && c.from.portId === "midi" && c.to.widgetId === sfEl.id)) {
      connectionCounter += 1;
      connections.push({
        id: `conn-${connectionCounter}`,
        from: { widgetId: source, portId: "midi" },
        to: { widgetId: sfEl.id, portId: "midi" },
        color: getConnectionColorForOutput(source, "midi"),
      });
    }
    migratedSoundFontMidiConnections = true;
  });

  
  
  
  
  connections.forEach((c) => {
    const fromEl = document.getElementById(c.from.widgetId);
    if (fromEl && fromEl.dataset.widgetType === "knob" && c.from.portId === "audio") {
      c.from.portId = "value";
      migratedKnobAudioConnections = true;
    }
  });

  
  
  
  
  connections.forEach((c) => {
    const fromEl = document.getElementById(c.from.widgetId);
    if (fromEl && fromEl.dataset.widgetType === "knob" && c.from.portId === "value") {
      c.from.portId = "voltage";
      migratedKnobValueConnections = true;
    }
  });

  connections.forEach((c) => {
    const fromEl = document.getElementById(c.from.widgetId);
    if (fromEl && fromEl.dataset.widgetType === "button" && c.from.portId === "audio") {
      c.from.portId = "value";
      migratedButtonAudioConnections = true;
    }
  });

  // Stereo page links became eight numbered mono buses. Preserve old presets
  // by moving their L/R pair onto buses 1/2 before ports are reconnected.
  connections.forEach((c) => {
    const fromEl = document.getElementById(c.from.widgetId);
    const toEl = document.getElementById(c.to.widgetId);
    if (fromEl?.dataset.widgetType === "audio-input-link") {
      if (c.from.portId === "outL") {
        c.from.portId = "out1";
        migratedLinkBusConnections = true;
      } else if (c.from.portId === "outR") {
        c.from.portId = "out2";
        migratedLinkBusConnections = true;
      }
    }
    if (toEl?.dataset.widgetType === "audio-output-link") {
      if (c.to.portId === "inL") {
        c.to.portId = "in1";
        migratedLinkBusConnections = true;
      } else if (c.to.portId === "inR") {
        c.to.portId = "in2";
        migratedLinkBusConnections = true;
      }
    }
  });

  connections.forEach((c) => {
    const fromEl = document.getElementById(c.from.widgetId);
    if (fromEl && fromEl.dataset.widgetType === "const" && c.from.portId === "audio") {
      c.from.portId = "value";
      migratedConstAudioConnections = true;
    }
  });

  connections.forEach((c) => {
    const fromEl = document.getElementById(c.from.widgetId);
    const toEl = document.getElementById(c.to.widgetId);
    if (fromEl?.dataset.widgetType === "frequency-divider" && c.from.portId === "hz") {
      c.from.portId = "cv";
      migratedFrequencyDividerCvConnections = true;
    }
    if (toEl?.dataset.widgetType === "frequency-divider" && c.to.portId === "hz") {
      c.to.portId = "cv";
      migratedFrequencyDividerCvConnections = true;
    }
  });

  
  
  
  connections.forEach((c) => {
    const toEl = document.getElementById(c.to.widgetId);
    if (toEl && toEl.dataset.widgetType === "oscilloscope" && c.to.portId === "in") {
      c.to.portId = "in1";
      migratedOscilloscopeInConnections = true;
    }
  });

  connections.forEach((c) => {
    const fromKind = getPortKind(c.from.widgetId, c.from.portId, "out");
    const toKind = getPortKind(c.to.widgetId, c.to.portId, "in");
    if (fromKind === "audio" && toKind === "audio") {
      audioEngine.connect(c.from.widgetId, c.from.portId, c.to.widgetId, c.to.portId);
    }
  });
  refreshControlGraph();
  refreshVoiceGroups();

  switchPage("generator");
  restoringPreset = false;
  
  
  
  
  pollAssetLoadProgress();
  if (removedAudioToValueWidgets || migratedLfoAudioConnections || migratedKnobAudioConnections || migratedButtonAudioConnections || migratedConstAudioConnections || migratedFrequencyDividerCvConnections || migratedLinkBusConnections || migratedLinkBusWidgets || migratedSoundFontMidiConnections || migratedKnobValueConnections || migratedOscilloscopeInConnections) {
    await saveToServer(currentPresetName);
  }
}

async function loadPresetOnStartup() {
  try {
    const res = await fetchWithRetry("/api/preset");
    const headerName = res.headers.get("X-Preset-Name") || "";
    const data = await res.json();
    currentPresetName = headerName || null;
    await applyPresetData(data, { fitToView: true });
    await refreshPresetMenu();
  } catch (err) {
    appendLog(`Preset could not be loaded: ${err.message}`);
    showToast("Preset could not be loaded — try reloading again");
    restoringPreset = false;
  }
  audioStartBtn.disabled = false;
}

async function loadPresetByName(name, { activate = true } = {}) {
  try {
    if (activate) {
      const selectUrl = name
        ? `/api/preset/select?name=${encodeURIComponent(name)}`
        : "/api/preset/select";
      const selectRes = await fetch(selectUrl, { method: "POST" });
      if (!selectRes.ok) throw new Error(`Select HTTP ${selectRes.status}`);
    }
    const url = name ? `/api/preset?name=${encodeURIComponent(name)}` : "/api/preset";
    const res = await fetch(url);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const headerName = res.headers.get("X-Preset-Name") || "";
    const data = await res.json();
    currentPresetName = headerName || null;
    await applyPresetData(data);
    await refreshPresetMenu();
  } catch (err) {
    appendLog(`Preset could not be loaded: ${err.message}`);
    restoringPreset = false;
  }
}

loadPresetOnStartup();



const RECONNECT_DELAY = 1000;



const cpuLoadCore0ValueEl = document.getElementById("cpuLoadCore0Value");
const cpuLoadCore1ValueEl = document.getElementById("cpuLoadCore1Value");
const cpuLoadCore0FillEl = document.getElementById("cpuLoadCore0Fill");
const cpuLoadCore1FillEl = document.getElementById("cpuLoadCore1Fill");
const cpuLoadUnderrunsValueEl = document.getElementById("cpuLoadUnderrunsValue");

function setCpuLoadValue(valueEl, fillEl, percent) {
  valueEl.textContent = `${Math.round(percent)}%`;
  const warn = percent >= 70 && percent < 90;
  const danger = percent >= 90;
  valueEl.classList.toggle("warn", warn);
  valueEl.classList.toggle("danger", danger);
  if (fillEl) {
    
    
    
    fillEl.style.width = `${clamp(percent, 0, 100)}%`;
    fillEl.classList.toggle("warn", warn);
    fillEl.classList.toggle("danger", danger);
  }
}







let lastUnderrunCount = null;
function updateCpuLoadDisplay(core0, core1, underruns) {
  if (typeof core0 === "number" && Number.isFinite(core0)) setCpuLoadValue(cpuLoadCore0ValueEl, cpuLoadCore0FillEl, core0);
  if (typeof core1 === "number" && Number.isFinite(core1)) setCpuLoadValue(cpuLoadCore1ValueEl, cpuLoadCore1FillEl, core1);
  if (cpuLoadUnderrunsValueEl && typeof underruns === "number" && Number.isFinite(underruns)) {
    cpuLoadUnderrunsValueEl.textContent = String(underruns);
    const rising = lastUnderrunCount !== null && underruns > lastUnderrunCount;
    cpuLoadUnderrunsValueEl.classList.toggle("danger", rising);
    lastUnderrunCount = underruns;
  }
}



const NODE_CPU_VISIBLE_KEY = "espressynth.nodeCpuVisible";
let nodeCpuVisible = localStorage.getItem(NODE_CPU_VISIBLE_KEY) === "1";

function setNodeCpuVisible(visible) {
  nodeCpuVisible = visible;
  document.body.classList.toggle("show-node-cpu", visible);
  localStorage.setItem(NODE_CPU_VISIBLE_KEY, visible ? "1" : "0");
  const menuBtn = document.getElementById("menuToggleNodeCpu");
  if (menuBtn) menuBtn.textContent = visible ? "Hide Node CPU %" : "Show Node CPU %";
}

function updateNodeCpuDisplay(pairs) {
  if (!Array.isArray(pairs)) return;
  pairs.forEach((pair) => {
    const widgetId = pair && pair[0];
    const percent = Number(pair && pair[1]) || 0;
    const el = widgetId ? document.getElementById(widgetId) : null;
    const header = el ? el.querySelector(".widget-instance-header") : null;
    if (!header) return;
    let badge = header.querySelector('[data-role="cpu-percent"]');
    if (!badge) {
      badge = document.createElement("span");
      badge.className = "widget-cpu-badge";
      badge.dataset.role = "cpu-percent";
      const closeBtn = header.querySelector(".widget-instance-close");
      if (closeBtn) header.insertBefore(badge, closeBtn);
      else header.appendChild(badge);
    }
    badge.textContent = `${percent.toFixed(1)}%`;
    badge.classList.toggle("warn", percent >= 5 && percent < 15);
    badge.classList.toggle("danger", percent >= 15);
  });
}



const psMemValueEl = document.getElementById("psMemValue");
const sysMemValueEl = document.getElementById("sysMemValue");

function setMemStatValue(el, freeKb, totalKb) {
  if (!el || typeof freeKb !== "number" || typeof totalKb !== "number" || !Number.isFinite(freeKb) ||
      !Number.isFinite(totalKb) || totalKb <= 0) {
    return;
  }
  const freePercent = (freeKb / totalKb) * 100;
  el.textContent = `${Math.round(freeKb)}kB/${Math.round(totalKb)}kB (${Math.round(freePercent)}%)`;
  el.classList.toggle("warn", freePercent <= 20 && freePercent > 10);
  el.classList.toggle("danger", freePercent <= 10);
}



function setMemStatValueMb(el, freeKb, totalKb) {
  if (!el || typeof freeKb !== "number" || typeof totalKb !== "number" || !Number.isFinite(freeKb) ||
      !Number.isFinite(totalKb) || totalKb <= 0) {
    return;
  }
  const freePercent = (freeKb / totalKb) * 100;
  el.textContent = `${(freeKb / 1024).toFixed(1)}/${(totalKb / 1024).toFixed(1)} MB`;
  el.classList.toggle("warn", freePercent <= 20 && freePercent > 10);
  el.classList.toggle("danger", freePercent <= 10);
}

function updateMemStatsDisplay(psFreeKb, psTotalKb, sysFreeKb, sysTotalKb) {
  setMemStatValueMb(psMemValueEl, psFreeKb, psTotalKb);
  setMemStatValue(sysMemValueEl, sysFreeKb, sysTotalKb);
}

let currentSocket = null;
let reconnectTimer = null;
let wsHasBeenDisconnected = false;
const connectionLostOverlay = document.getElementById("connectionLostOverlay");

// A graceful server-side close (WS close/RST/FIN) fires "close" right away and
// the reconnect logic below already handles that. But if the device just
// vanishes (power loss, Wi-Fi AP drop, hard crash) with no packet ever sent,
// plain TCP gives the browser nothing to notice that with for a long time (no
// FIN/RST arrives). Watch for that instead by tracking the last time ANY
// message arrived on this socket - broadcast_status_tick() (main.cpp) pushes
// a "cpuLoad" message every second whenever the synth engine + web server are
// enabled (this project's normal config), so silence past a couple of
// broadcasts means the peer is actually gone. Gated on having seen at least
// one message this connection so a build without that periodic broadcast
// just never triggers this path (falls back to relying on "close"/"error"
// alone, same as before).
//
// Once declared stale, do NOT wait for that dead socket's own "close" event
// before reacting: calling socket.close() still starts a normal WS closing
// handshake, and since the peer never acks it, the browser only gives up
// after its own OS-level TCP retransmission timeout (commonly ~20s) - same
// dead end we're trying to route around. Instead, mark it lost and start a
// fresh connection attempt immediately; the old socket is abandoned (its
// eventual native "close"/"error", whenever that fires, becomes a no-op via
// the currentSocket check below since a newer socket has already taken over).
const WS_STALE_TIMEOUT_MS = 2500;
const WS_WATCHDOG_INTERVAL_MS = 500;
let lastWsMessageAt = 0;
let wsWatchdogTimer = null;

// Shared by the "close" handler and the watchdog below: flips the UI to
// "lost" exactly once per socket and hands the socket off. Returns false if
// this socket has already been superseded (e.g. the watchdog already
// declared it dead and moved on), so the caller can skip scheduling a
// redundant reconnect.
function markWsLost(socket) {
  if (currentSocket !== socket) return false;
  clearInterval(wsWatchdogTimer);
  wsHasBeenDisconnected = true;
  connectionLostOverlay?.classList.remove("hidden");
  currentSocket = null;
  return true;
}

function startWsWatchdog(socket) {
  clearInterval(wsWatchdogTimer);
  wsWatchdogTimer = setInterval(() => {
    if (currentSocket !== socket || socket.readyState !== WebSocket.OPEN) return;
    if (lastWsMessageAt === 0) return;
    if (Date.now() - lastWsMessageAt > WS_STALE_TIMEOUT_MS) {
      if (markWsLost(socket)) {
        clearTimeout(reconnectTimer);
        connectWs();
      }
      socket.close(); // best-effort cleanup only, nothing waits on it anymore
    }
  }, WS_WATCHDOG_INTERVAL_MS);
}












const pendingFsInfoRequests = new Map(); 
const FS_INFO_TIMEOUT_MS = 5000;

function requestFsInfo(volume) {
  return new Promise((resolve) => {
    if (!currentSocket || currentSocket.readyState !== WebSocket.OPEN) {
      resolve(null);
      return;
    }
    const existing = pendingFsInfoRequests.get(volume);
    if (existing) {
      existing.resolvers.push(resolve);
      return;
    }
    const timeoutId = setTimeout(() => {
      const pending = pendingFsInfoRequests.get(volume);
      if (pending) {
        pendingFsInfoRequests.delete(volume);
        pending.resolvers.forEach((r) => r(null));
      }
    }, FS_INFO_TIMEOUT_MS);
    pendingFsInfoRequests.set(volume, { resolvers: [resolve], timeoutId });
    currentSocket.send(JSON.stringify({ type: "filesInfo", volume }));
  });
}

function connectWs() {
  clearTimeout(reconnectTimer);
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${protocol}//${window.location.host}/ws`);
  currentSocket = socket;

  socket.addEventListener("message", (event) => {
    lastWsMessageAt = Date.now();
    let data;
    try {
      data = JSON.parse(event.data);
    } catch (err) {
      return;
    }
    if (data.type === "presetChanged") {
      const incomingName = data.name || "";
      if (incomingName !== (currentPresetName || "")) {
        loadPresetByName(incomingName, { activate: false });
      }
    } else if (data.type === "midi") {
      appendLog(`[${data.time}] ${data.port}: ${data.message}`);
      routeMidiToWidgets(data);
      routeMidiToKnobs(data);
      routeMidiToButtons(data);
      routeMidiToUpDownButtons(data);
      routeMidiToSwitches(data);
      routeMidiToSoundFontPlayer(data);
      flashMidiActivity();
    } else if (data.type === "midi-error") {
      appendLog(`MIDI error: ${data.error}`);
    } else if (data.type === "cpuLoad") {
      updateCpuLoadDisplay(data.core0, data.core1, data.underruns);
      updateMemStatsDisplay(data.psFreeKb, data.psTotalKb, data.sysFreeKb, data.sysTotalKb);
    } else if (data.type === "nodeCpu") {
      updateNodeCpuDisplay(data.w);
    } else if (data.type === "voicesCapped") {
      handleVoicesCapped(data.requested, data.effective, data.reason);
    } else if (data.type === "midiDevices") {
      handleMidiDevicesPush(data);
    } else if (data.type === "filesInfo") {
      const pending = pendingFsInfoRequests.get(data.volume);
      if (pending) {
        clearTimeout(pending.timeoutId);
        pendingFsInfoRequests.delete(data.volume);
        pending.resolvers.forEach((r) => r(data));
      }
    }
  });

  socket.addEventListener("open", () => {
    if (currentSocket !== socket) return;
    lastWsMessageAt = 0;
    startWsWatchdog(socket);
    // Only a genuine reconnect (we've been through at least one "close" before)
    // warrants a reload - the very first connectWs() call on page load also
    // fires "open" and must NOT reload, or the page would reload forever.
    if (wsHasBeenDisconnected) {
      location.reload();
      return;
    }
    connectionLostOverlay?.classList.add("hidden");
  });

  socket.addEventListener("close", () => {
    if (!markWsLost(socket)) return;
    reconnectTimer = setTimeout(connectWs, RECONNECT_DELAY);
  });

  socket.addEventListener("error", () => {
    socket.close();
  });
}

connectWs();








// Firmware exposes network configuration through its native configuration surface, not this
// legacy web API. Hide controls whose /api/settings/network and /api/ble/* routes do not exist.
document.getElementById("menuBleConnect")?.remove();
document.getElementById("menuSettings")?.remove();



const logWindow = document.getElementById("logWindow");
const logWindowHeader = document.getElementById("logWindowHeader");
const logWindowBody = document.getElementById("logWindowBody");
const MAX_LOG_LINES = 300;

function appendLog(text) {
  const line = document.createElement("div");
  line.textContent = text;
  logWindowBody.appendChild(line);
  while (logWindowBody.childElementCount > MAX_LOG_LINES) {
    logWindowBody.removeChild(logWindowBody.firstChild);
  }
  logWindowBody.scrollTop = logWindowBody.scrollHeight;
}

function makeDraggable(handle, panel, onDragEnd) {
  handle.addEventListener("mousedown", (e) => {
    if (e.target.closest("button")) return;
    e.preventDefault();
    const rect = panel.getBoundingClientRect();
    panel.style.left = `${rect.left}px`;
    panel.style.top = `${rect.top}px`;
    panel.style.right = "auto";
    const startX = e.clientX;
    const startY = e.clientY;
    const origLeft = rect.left;
    const origTop = rect.top;

    function onMove(ev) {
      panel.style.left = `${origLeft + ev.clientX - startX}px`;
      panel.style.top = `${origTop + ev.clientY - startY}px`;
    }
    function onUp() {
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp);
      if (onDragEnd) onDragEnd();
    }
    document.addEventListener("mousemove", onMove);
    document.addEventListener("mouseup", onUp);
  });
}



const LOG_WINDOW_GEOMETRY_KEY = "logWindowGeometry";
const LOG_WINDOW_FALLBACK_POS = { left: 24, top: 90 };

function saveLogWindowGeometry() {
  const rect = logWindow.getBoundingClientRect();
  localStorage.setItem(
    LOG_WINDOW_GEOMETRY_KEY,
    JSON.stringify({ left: rect.left, top: rect.top, width: rect.width, height: rect.height })
  );
}

function isSufficientlyVisible(rect) {
  const visibleWidth = Math.max(
    0,
    Math.min(rect.left + rect.width, window.innerWidth) - Math.max(rect.left, 0)
  );
  const visibleHeight = Math.max(
    0,
    Math.min(rect.top + rect.height, window.innerHeight) - Math.max(rect.top, 0)
  );
  const totalArea = rect.width * rect.height;
  if (totalArea <= 0) return false;
  return (visibleWidth * visibleHeight) / totalArea >= 0.5;
}

function restoreLogWindowGeometry() {
  const raw = localStorage.getItem(LOG_WINDOW_GEOMETRY_KEY);
  if (!raw) return;
  let geo;
  try {
    geo = JSON.parse(raw);
  } catch (err) {
    return;
  }
  if (
    !geo ||
    typeof geo.left !== "number" ||
    typeof geo.top !== "number" ||
    typeof geo.width !== "number" ||
    typeof geo.height !== "number"
  ) {
    return;
  }

  const width = Math.min(geo.width, window.innerWidth - 20);
  const height = Math.min(geo.height, window.innerHeight - 20);
  const pos = isSufficientlyVisible({ left: geo.left, top: geo.top, width, height })
    ? { left: geo.left, top: geo.top }
    : LOG_WINDOW_FALLBACK_POS;

  logWindow.style.left = `${pos.left}px`;
  logWindow.style.top = `${pos.top}px`;
  logWindow.style.right = "auto";
  logWindow.style.width = `${width}px`;
  logWindow.style.height = `${height}px`;
}

restoreLogWindowGeometry();
makeDraggable(logWindowHeader, logWindow, saveLogWindowGeometry);

let logWindowResizeSaveTimer = null;
new ResizeObserver(() => {
  clearTimeout(logWindowResizeSaveTimer);
  logWindowResizeSaveTimer = setTimeout(saveLogWindowGeometry, 300);
}).observe(logWindow);

const LOG_WINDOW_VISIBLE_KEY = "logWindowVisible";

function setLogWindowVisible(visible) {
  logWindow.classList.toggle("hidden", !visible);
  localStorage.setItem(LOG_WINDOW_VISIBLE_KEY, visible ? "1" : "0");
}

if (localStorage.getItem(LOG_WINDOW_VISIBLE_KEY) === "1") {
  setLogWindowVisible(true);
}

document.getElementById("menuShowLog").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  setLogWindowVisible(true);
});

document.getElementById("menuToggleNodeCpu").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  setNodeCpuVisible(!nodeCpuVisible);
});
setNodeCpuVisible(nodeCpuVisible); 

const menuCableSag = document.getElementById("menuCableSag");
menuCableSag.checked = cableSagEnabled;
menuCableSag.closest(".dropdown-check").addEventListener("click", (e) => e.stopPropagation());
menuCableSag.addEventListener("change", () => setCableSagEnabled(menuCableSag.checked));

const menuOrthogonalCables = document.getElementById("menuOrthogonalCables");
menuOrthogonalCables.checked = orthogonalCablesEnabled;
menuOrthogonalCables.closest(".dropdown-check").addEventListener("click", (e) => e.stopPropagation());
menuOrthogonalCables.addEventListener("change", () => setOrthogonalCablesEnabled(menuOrthogonalCables.checked));

const menuGridSnap = document.getElementById("menuGridSnap");
menuGridSnap.checked = gridSnapEnabled;
menuGridSnap.closest(".dropdown-check").addEventListener("click", (e) => e.stopPropagation());
menuGridSnap.addEventListener("change", () => setGridSnapEnabled(menuGridSnap.checked));

const menuBackgroundDots = document.getElementById("menuBackgroundDots");
menuBackgroundDots.checked = backgroundDotsEnabled;
menuBackgroundDots.closest(".dropdown-check").addEventListener("click", (e) => e.stopPropagation());
menuBackgroundDots.addEventListener("change", () => setBackgroundDotsEnabled(menuBackgroundDots.checked));
setBackgroundDotsEnabled(backgroundDotsEnabled);

const WIDGET_OPACITY_KEY = "espressynth.widgetOpacity";
const menuWidgetOpacity = document.getElementById("menuWidgetOpacity");
const menuWidgetOpacityValue = document.getElementById("menuWidgetOpacityValue");

function setWidgetOpacity(percent, persist = true) {
  const value = Math.min(100, Math.max(50, Number(percent) || 100));
  const roundedValue = Math.round(value);
  document.documentElement.style.setProperty("--widget-opacity", String(roundedValue / 100));
  menuWidgetOpacity.value = String(roundedValue);
  menuWidgetOpacityValue.textContent = `${roundedValue}%`;
  if (persist) localStorage.setItem(WIDGET_OPACITY_KEY, String(roundedValue));
}

const savedWidgetOpacity = Number(localStorage.getItem(WIDGET_OPACITY_KEY));
setWidgetOpacity(Number.isFinite(savedWidgetOpacity) ? savedWidgetOpacity : 100, false);
menuWidgetOpacity.closest(".dropdown-range").addEventListener("click", (e) => e.stopPropagation());
menuWidgetOpacity.addEventListener("input", () => setWidgetOpacity(menuWidgetOpacity.value));

document.getElementById("logWindowClose").addEventListener("click", () => {
  setLogWindowVisible(false);
});



const midiNoDevice = document.getElementById("midiNoDevice");
const midiDeviceList = document.getElementById("midiDeviceList");
const midiActivity = document.getElementById("midiActivity");
let midiFlashTimer = null;

function flashMidiActivity() {
  midiActivity.classList.add("flash");
  clearTimeout(midiFlashTimer);
  midiFlashTimer = setTimeout(() => midiActivity.classList.remove("flash"), 300);
}



function renderMidiDevices(devices) {
  midiNoDevice.hidden = devices.length > 0;
  midiDeviceList.replaceChildren();
  for (const name of devices) {
    const isBle = name.startsWith("BLE-MIDI ");
    const chip = document.createElement("span");
    chip.className = isBle ? "midi-device-chip midi-device-chip-ble" : "midi-device-chip";
    chip.textContent = name;
    midiDeviceList.appendChild(chip);
  }
}

function handleMidiDevicesPush(data) {
  const devices = data.devices || [];
  renderMidiDevices(devices);
}

async function loadMidiDevices() {
  try {
    const res = await fetch("/api/midi/devices");
    handleMidiDevicesPush(await res.json());
  } catch (err) {
    appendLog(`MIDI device list could not be loaded: ${err.message}`);
  }
}

loadMidiDevices();
