import { COMB_FILTER_PORTS, COMB_FILTER_DEFAULTS, combFilterCv } from "./comb-filter-core.js?v=20260927-drum-pulse-1";
import { createMidiFilterState, acceptsMidiFilter, midiFilterCriterion } from "./midi-filter-core.js?v=20260927-drum-pulse-1";
import { AudioEngine } from "./audio-engine.js?v=20260927-drum-pulse-1";
import { getPresetName, getMaxBank } from "./sf2-engine.js";
import { createDefaultBank, decodeBank, encodeBank, sampleBank } from "./wavetable-core.js?v=20260926-knob-2";
import { openWavetableEditor, drawWavetablePreview } from "./wavetable-editor.js?v=20260926-knob-2";

const GRID = 20;
const LINK_BUS_COUNT = 8;
const SOUNDFONT_DEFAULT_MAX_BANK = 128;
const audioEngine = new AudioEngine();
window.audioEngine = audioEngine; // debug: inspect from DevTools console (module scope isn't global)

// Browser-only setting (localStorage, never sent to the device): whether the
// preview engine downloads samples (Wave Player / SoundFont Player / MDA
// Piano) on page load. Off = faster start; the skipped downloads can be
// fetched later with the blinking "Load Samples" button in the top bar.
const LOAD_SAMPLES_KEY = "espressynth.loadSamples";
function readLoadSamplesSetting() {
  try {
    return localStorage.getItem(LOAD_SAMPLES_KEY) !== "0";
  } catch (err) {
    return true;
  }
}
audioEngine.setSampleLoadingEnabled(readLoadSamplesSetting());

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

document.addEventListener("keydown", (e) => {
  if (e.key !== "Escape") return;
  if (cancelKnobMidiLearn() || cancelButtonMidiLearn()) {
    e.preventDefault();
    e.stopImmediatePropagation();
    return;
  }
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
const infoGitVersionValue = document.getElementById("infoGitVersionValue");
const infoI2CDevices = document.getElementById("infoI2CDevices");

async function openInfo() {
  infoVersionValue.textContent = "–";
  infoGitVersionValue.textContent = "–";
  infoI2CDevices.textContent = "–";
  infoOverlay.classList.remove("hidden");
  try {
    const res = await fetch("/api/version");
    const data = await res.json();
    infoVersionValue.textContent = data.version || "unknown";
    infoGitVersionValue.textContent = data.gitVersion || "unknown";
  } catch (err) {
    infoVersionValue.textContent = "unknown";
    infoGitVersionValue.textContent = "unknown";
  }
  try {
    const res = await fetch("/api/system/i2c");
    const data = await res.json();
    const devices = data.devices || [];
    infoI2CDevices.textContent = devices.length === 0
      ? "None found"
      : devices.map((d) => `${d.name} — I2C${d.bus} @ ${d.address}`).join(", ");
  } catch (err) {
    infoI2CDevices.textContent = "unknown";
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
    const cat = WIDGET_CATEGORIES.find((c) => c.label === "External Audio");
    if (cat) {
      const idx = cat.types.indexOf("wm8960-out");
      if (idx >= 0) cat.types.splice(idx, 1);
    }
    globalThis.__wm8960Enabled = false;
  } else {
    globalThis.__wm8960Enabled = true;
  }
}

// Populated by applyCvIoAvailability() below - the Settings dialog's GPIO section reuses
// these (via cvIoAvailabilityReady) instead of a second /api/version round trip, so its
// Direction picker only ever offers a direction the firmware was actually built with.
let gpioInputAvailable = false;
let gpioOutputsAvailable = false;

async function applyCvIoAvailability() {
  let ads1115 = false;
  let gp8413 = false;
  let gpioInput = false;
  let gpioOutputs = false;
  try {
    const res = await fetch("/api/version");
    const data = await res.json();
    ads1115 = data.ads1115 === true;
    gp8413 = data.gp8413 === true;
    gpioInput = data.gpioInput === true;
    gpioOutputs = data.gpioOutputs === true;
  } catch (err) {
    ads1115 = false;
    gp8413 = false;
    gpioInput = false;
    gpioOutputs = false;
  }
  gpioInputAvailable = gpioInput;
  gpioOutputsAvailable = gpioOutputs;
  const cat = WIDGET_CATEGORIES.find((c) => c.label === "External Hardware");
  if (cat) {
    if (!ads1115) {
      const idx = cat.types.indexOf("cv-in");
      if (idx >= 0) cat.types.splice(idx, 1);
    }
    if (!gp8413) {
      const idx = cat.types.indexOf("cv-out");
      if (idx >= 0) cat.types.splice(idx, 1);
    }
    if (!gpioInput) {
      const idx = cat.types.indexOf("digital-in");
      if (idx >= 0) cat.types.splice(idx, 1);
    }
    if (!gpioOutputs) {
      const idx = cat.types.indexOf("digital-out");
      if (idx >= 0) cat.types.splice(idx, 1);
    }
  }
}

applyWm8960Availability();
const cvIoAvailabilityReady = applyCvIoAvailability();

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

const backupRestoreInput = document.getElementById("backupRestoreInput");

backupRestoreInput.addEventListener("change", () => {
  const file = backupRestoreInput.files[0];
  if (file) restoreSystemFromFile(file);
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



const loadSamplesBtn = document.getElementById("loadSamplesBtn");
function updateLoadSamplesButton(pendingCount = audioEngine.getDeferredSampleCount()) {
  loadSamplesBtn.hidden = pendingCount === 0;
  loadSamplesBtn.textContent = pendingCount > 1 ? `Load Samples (${pendingCount})` : "Load Samples";
}
audioEngine.onDeferredSamplesChange = updateLoadSamplesButton;
loadSamplesBtn.addEventListener("click", () => {
  audioEngine.loadDeferredSamples();
  updateLoadSamplesButton();
});
updateLoadSamplesButton();

const audioStartBtn = document.getElementById("audioStartBtn");
audioStartBtn.addEventListener("click", () => {





  if (!audioEngine.isRunning()) audioEngine.resume();
  const muted = !audioEngine.isMasterMuted();
  audioEngine.setMasterMuted(muted);
  audioStartBtn.textContent = muted ? "🔇" : "🔊";
  audioStartBtn.classList.toggle("active", !muted);
  audioStartBtn.title = muted ? "Start audio" : "Mute audio";
});

// Browsers refuse to run an AudioContext until a real user gesture happens
// on the page (autoplay policy) - can't be forced from page-load code alone.
// Without this, every local-preview reading that depends on the WebAudio
// graph actually running (Debug/Voltmeter, tickDebugVoltmeterAudioSources)
// silently stays at 0 until someone happens to click audioStartBtn
// specifically. Resume on the page's first click/touch/keypress ANYWHERE
// instead - still a genuine gesture (satisfies the policy), just not tied to
// that one small icon. Deliberately does NOT touch mute state - this only
// gets the context calculating again, it must not make the browser start
// playing audible sound out of nowhere.
function resumeAudioContextOnFirstGesture() {
  if (!audioEngine.isRunning()) audioEngine.resume();
  document.removeEventListener("pointerdown", resumeAudioContextOnFirstGesture);
  document.removeEventListener("keydown", resumeAudioContextOnFirstGesture);
}
document.addEventListener("pointerdown", resumeAudioContextOnFirstGesture);
document.addEventListener("keydown", resumeAudioContextOnFirstGesture);



const settingsOverlay = document.getElementById("settingsOverlay");
const gpioSettingsOverlay = document.getElementById("gpioSettingsOverlay");
const networkSettingsOverlay = document.getElementById("networkSettingsOverlay");
const gpioSummaryText = document.getElementById("gpioSummaryText");
const networkSummaryText = document.getElementById("networkSummaryText");
const ethDhcpInput = document.getElementById("ethDhcpInput");
const ethStaticFields = document.getElementById("ethStaticFields");
const ethIpInput = document.getElementById("ethIpInput");
const ethMaskInput = document.getElementById("ethMaskInput");
const ethGwInput = document.getElementById("ethGwInput");
const ethDnsInput = document.getElementById("ethDnsInput");
const wifiEnabledInput = document.getElementById("wifiEnabledInput");
const wifiSsidInput = document.getElementById("wifiSsidInput");
const wifiPasswordInput = document.getElementById("wifiPasswordInput");
const wifiDhcpInput = document.getElementById("wifiDhcpInput");
const wifiStaticFields = document.getElementById("wifiStaticFields");
const wifiIpInput = document.getElementById("wifiIpInput");
const wifiMaskInput = document.getElementById("wifiMaskInput");
const wifiGwInput = document.getElementById("wifiGwInput");
const wifiDnsInput = document.getElementById("wifiDnsInput");
const wifiScanButton = document.getElementById("wifiScan");
const wifiStatus = document.getElementById("wifiStatus");
const wifiNetworks = document.getElementById("wifiNetworks");
const deviceNameInput = document.getElementById("deviceNameInput");
const gpioEntryList = document.getElementById("gpioEntryList");
const gpioAddPinSelect = document.getElementById("gpioAddPinSelect");
const gpioAddDirectionSelect = document.getElementById("gpioAddDirectionSelect");
const gpioAddPullSelect = document.getElementById("gpioAddPullSelect");
const gpioAddInvertInput = document.getElementById("gpioAddInvertInput");
const gpioAddBtn = document.getElementById("gpioAddBtn");
let pendingGpioEntries = [];
let loadedGpioEntries = [];
let gpioReservedPins = [];

async function fetchGpioInfo() {
  try {
    const res = await fetch("/api/system/gpio");
    if (!res.ok) return { minPin: 0, maxPin: -1, reserved: [] };
    return await res.json();
  } catch (err) {
    return { minPin: 0, maxPin: -1, reserved: [] };
  }
}

// WS2812 string (Settings -> LED String). pin/count are the saved settings, activeCount what
// the running firmware actually drives (settings only apply after a reboot). Carried by
// /api/settings/device, see synth_web_api.hpp's device_settings_handler.
const ledStripPinSelect = document.getElementById("ledStripPinSelect");
const ledStripCountInput = document.getElementById("ledStripCountInput");
const ledStripStatusText = document.getElementById("ledStripStatusText");
let ledStripInfo = null;

async function fetchLedStripInfo() {
  try {
    const res = await fetch("/api/settings/device");
    if (!res.ok) return null;
    const data = await res.json();
    if (typeof data.ledPin !== "number") return null;
    return {
      pin: data.ledPin,
      count: data.ledCount || 0,
      activeCount: data.ledActiveCount || 0,
      maxCount: data.ledMaxCount || 256,
    };
  } catch (err) {
    return null;
  }
}

// Free pins = valid range minus reserved (which already includes the saved LED pin itself -
// re-added here) minus pins used in Settings -> GPIO.
function renderLedStripSettings(gpioInfo) {
  ledStripPinSelect.innerHTML = "";
  const none = document.createElement("option");
  none.value = "-1";
  none.textContent = "Not connected";
  ledStripPinSelect.appendChild(none);
  if (!ledStripInfo) {
    ledStripPinSelect.disabled = true;
    ledStripCountInput.disabled = true;
    ledStripStatusText.textContent = "LED string settings unavailable.";
    return;
  }
  ledStripPinSelect.disabled = false;
  ledStripCountInput.disabled = false;
  const taken = new Set([
    ...(Array.isArray(gpioInfo.reserved) ? gpioInfo.reserved : []),
    ...(Array.isArray(gpioInfo.configured) ? gpioInfo.configured.map((e) => e.pin) : []),
  ]);
  taken.delete(ledStripInfo.pin);
  for (let pin = gpioInfo.minPin || 0; pin <= (gpioInfo.maxPin ?? -1); ++pin) {
    if (taken.has(pin)) continue;
    const opt = document.createElement("option");
    opt.value = String(pin);
    opt.textContent = `GPIO${pin}`;
    ledStripPinSelect.appendChild(opt);
  }
  ledStripPinSelect.value = String(ledStripInfo.pin);
  if (ledStripPinSelect.value !== String(ledStripInfo.pin)) ledStripPinSelect.value = "-1";
  ledStripCountInput.max = String(ledStripInfo.maxCount);
  ledStripCountInput.value = String(ledStripInfo.count);
  ledStripStatusText.textContent = ledStripInfo.activeCount > 0
    ? `Running: ${ledStripInfo.activeCount} LEDs. Changes take effect after a reboot.`
    : "Not running. Changes take effect after a reboot.";
}

const gpioAddRow = document.getElementById("gpioAddRow");
const gpioAddDisabledHint = document.getElementById("gpioAddDisabledHint");
const gpioAddDirectionIn = document.getElementById("gpioAddDirectionIn");
const gpioAddDirectionOut = document.getElementById("gpioAddDirectionOut");

// Only offers a direction the firmware was actually built with (CONFIG_APP_ENABLE_GPIO_INPUT/
// _OUTPUTS, see gpioInputAvailable/gpioOutputsAvailable above) - matches how the canvas
// widget picker hides Digital In/Out under the same two flags (applyCvIoAvailability).
function updateGpioDirectionOptions() {
  gpioAddDirectionIn.hidden = !gpioInputAvailable;
  gpioAddDirectionOut.hidden = !gpioOutputsAvailable;
  const bothDisabled = !gpioInputAvailable && !gpioOutputsAvailable;
  gpioAddRow.hidden = bothDisabled;
  gpioAddDisabledHint.hidden = !bothDisabled;
  if (gpioAddDirectionSelect.value === "in" && !gpioInputAvailable) gpioAddDirectionSelect.value = "out";
  else if (gpioAddDirectionSelect.value === "out" && !gpioOutputsAvailable) gpioAddDirectionSelect.value = "in";
}

function renderGpioAddPinOptions() {
  const taken = new Set([...gpioReservedPins, ...pendingGpioEntries.map((e) => e.pin)]);
  const info = gpioAddPinSelect.dataset.range ? JSON.parse(gpioAddPinSelect.dataset.range) : { minPin: 0, maxPin: -1 };
  gpioAddPinSelect.innerHTML = "";
  for (let pin = info.minPin; pin <= info.maxPin; ++pin) {
    if (taken.has(pin)) continue;
    const opt = document.createElement("option");
    opt.value = String(pin);
    opt.textContent = `GPIO${pin}`;
    gpioAddPinSelect.appendChild(opt);
  }
  if (gpioAddPinSelect.options.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "No free pins";
    gpioAddPinSelect.appendChild(opt);
  }
}

function renderGpioEntryList() {
  gpioEntryList.replaceChildren();
  pendingGpioEntries.forEach((entry, idx) => {
    const row = document.createElement("div");
    row.className = "gpio-entry-row";
    const label = document.createElement("span");
    const dir = entry.direction === "out" ? "Output" : "Input";
    const pull = entry.direction === "in" && entry.pull && entry.pull !== "none" ? `, ${entry.pull === "up" ? "pull-up" : "pull-down"}` : "";
    const invert = entry.invert ? ", inverted" : "";
    label.textContent = `GPIO${entry.pin} - ${dir}${pull}${invert}`;
    const removeBtn = document.createElement("button");
    removeBtn.type = "button";
    removeBtn.className = "btn-secondary";
    removeBtn.textContent = "Remove";
    removeBtn.addEventListener("click", () => {
      pendingGpioEntries.splice(idx, 1);
      renderGpioEntryList();
      renderGpioAddPinOptions();
    });
    row.appendChild(label);
    row.appendChild(removeBtn);
    gpioEntryList.appendChild(row);
  });
}

gpioAddBtn.addEventListener("click", () => {
  const pin = parseInt(gpioAddPinSelect.value, 10);
  if (!Number.isFinite(pin)) {
    showToast("No free GPIO pin available");
    return;
  }
  pendingGpioEntries.push({
    pin,
    direction: gpioAddDirectionSelect.value,
    pull: gpioAddPullSelect.value,
    invert: gpioAddInvertInput.checked,
  });
  gpioAddInvertInput.checked = false;
  renderGpioEntryList();
  renderGpioAddPinOptions();
});

function updateStaticFieldsVisibility() {
  ethStaticFields.hidden = ethDhcpInput.checked;
}

function updateWifiStaticFieldsVisibility() {
  wifiStaticFields.hidden = wifiDhcpInput.checked;
}

function renderWifiStatus(data) {
  if (!data.wifiAvailable && !data.available) wifiStatus.textContent = "Wi-Fi unavailable on this board";
  else if (data.wifiConnected || data.connected) wifiStatus.textContent = `Connected${data.wifiIp || data.ip ? ` — ${data.wifiIp || data.ip}` : ""}`;
  else if (data.wifiScanning || data.scanning) wifiStatus.textContent = "Scanning…";
  else if (data.wifiError || data.error) wifiStatus.textContent = `Error: ${data.wifiError || data.error}`;
  else wifiStatus.textContent = data.wifiEnabled || data.enabled ? "Connecting…" : "Wi-Fi disabled";
}

ethDhcpInput.addEventListener("change", updateStaticFieldsVisibility);
wifiDhcpInput.addEventListener("change", updateWifiStaticFieldsVisibility);

let loadedNetworkSnapshot = null;
let loadedNetworkData = {};

// Populates every Ethernet/Wi-Fi field from a fetched (or previously loaded)
// settings object - shared by openSettings' initial load and the Network
// modal's Cancel (revert to what was last loaded/saved).
function applyNetworkData(data) {
  ethDhcpInput.checked = data.ethDhcp !== false;
  ethIpInput.value = data.ethIp || "";
  ethMaskInput.value = data.ethMask || "";
  ethGwInput.value = data.ethGw || "";
  ethDnsInput.value = data.ethDns || "";
  wifiEnabledInput.checked = data.wifiEnabled === true;
  wifiSsidInput.value = data.wifiSsid || "";
  wifiPasswordInput.value = "";
  wifiDhcpInput.checked = data.wifiDhcp !== false;
  wifiIpInput.value = data.wifiIp || "";
  wifiMaskInput.value = data.wifiMask || "";
  wifiGwInput.value = data.wifiGw || "";
  wifiDnsInput.value = data.wifiDns || "";
  renderWifiStatus(data);
  updateStaticFieldsVisibility();
  updateWifiStaticFieldsVisibility();
}

function currentNetworkSnapshot() {
  return JSON.stringify({
    ethDhcp: ethDhcpInput.checked,
    ethIp: ethIpInput.value.trim(),
    ethMask: ethMaskInput.value.trim(),
    ethGw: ethGwInput.value.trim(),
    ethDns: ethDnsInput.value.trim(),
    wifiEnabled: wifiEnabledInput.checked,
    wifiSsid: wifiSsidInput.value.trim(),
    wifiPassword: wifiPasswordInput.value,
    wifiDhcp: wifiDhcpInput.checked,
    wifiIp: wifiIpInput.value.trim(),
    wifiMask: wifiMaskInput.value.trim(),
    wifiGw: wifiGwInput.value.trim(),
    wifiDns: wifiDnsInput.value.trim(),
  });
}

function updateGpioSummary() {
  const count = pendingGpioEntries.length;
  gpioSummaryText.textContent = count === 0 ? "No pins configured" : `${count} pin${count === 1 ? "" : "s"} configured`;
}

function updateNetworkSummary() {
  const ethPart = ethDhcpInput.checked ? "Ethernet: DHCP" : "Ethernet: Static IP";
  const wifiPart = !wifiEnabledInput.checked
    ? "Wi-Fi disabled"
    : `Wi-Fi: ${wifiSsidInput.value.trim() || "(no SSID)"}${wifiDhcpInput.checked ? "" : " (static IP)"}`;
  networkSummaryText.textContent = `${ethPart} · ${wifiPart}`;
}

async function openSettings() {
  settingsOverlay.classList.remove("hidden");
  loadSamplesSetting.checked = audioEngine.isSampleLoadingEnabled();
  try {
    const res = await fetch("/api/settings/network");
    const data = await res.json();
    loadedNetworkData = data;
    applyNetworkData(data);
  } catch (err) {
    appendLog(`Could not load network settings: ${err.message}`);
  }
  // Baseline to detect whether the user actually touched Ethernet/Wi-Fi before Save - there is
  // no /api/settings/network backend yet (GET above always fails, silently kept at whatever
  // the fields already showed), so without this check every Save would try (and fail) to POST
  // network settings even when this section was never opened/touched, showing a confusing
  // error for an unrelated Device/GPIO save.
  loadedNetworkSnapshot = currentNetworkSnapshot();
  updateNetworkSummary();
  try {
    const res = await fetch("/api/settings/device");
    const data = await res.json();
    deviceNameInput.value = data.name || "";
    presetPrevCc = typeof data.prevCc === "number" && data.prevCc >= 0 ? data.prevCc : null;
    presetPrevChannel = typeof data.prevChannel === "number" && data.prevChannel >= 0 ? data.prevChannel : null;
    presetNextCc = typeof data.nextCc === "number" && data.nextCc >= 0 ? data.nextCc : null;
    presetNextChannel = typeof data.nextChannel === "number" && data.nextChannel >= 0 ? data.nextChannel : null;
    updatePresetNavLearnUi();
  } catch (err) {
    appendLog(`Could not load device settings: ${err.message}`);
  }
  try {
    await cvIoAvailabilityReady;
    updateGpioDirectionOptions();
    const gpioRes = await fetch("/api/settings/gpio");
    const gpioData = await gpioRes.json();
    pendingGpioEntries = Array.isArray(gpioData.entries) ? gpioData.entries.map((e) => ({ ...e })) : [];
    loadedGpioEntries = pendingGpioEntries.map((e) => ({ ...e }));
    const info = await fetchGpioInfo();
    gpioReservedPins = Array.isArray(info.reserved) ? info.reserved : [];
    gpioAddPinSelect.dataset.range = JSON.stringify({ minPin: info.minPin || 0, maxPin: info.maxPin ?? -1 });
    renderGpioEntryList();
    renderGpioAddPinOptions();
    updateGpioSummary();
  } catch (err) {
    appendLog(`Could not load GPIO settings: ${err.message}`);
  }
  try {
    ledStripInfo = await fetchLedStripInfo();
    renderLedStripSettings(await fetchGpioInfo());
  } catch (err) {
    appendLog(`Could not load LED string settings: ${err.message}`);
  }
  deviceNameInput.focus();
}

const loadSamplesSetting = document.getElementById("loadSamplesSetting");

function closeSettings() {
  settingsOverlay.classList.add("hidden");
  presetPrevLearnActive = false;
  presetNextLearnActive = false;
  updatePresetNavLearnUi();
}

document.getElementById("menuSettings").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  openSettings();
});

document.getElementById("settingsCancel").addEventListener("click", closeSettings);

document.getElementById("settingsBackupBtn").addEventListener("click", () => backupSystemToFile());
document.getElementById("settingsRestoreBtn").addEventListener("click", () => {
  backupRestoreInput.value = "";
  backupRestoreInput.click();
});

// GPIO now lives in its own popped-out modal (opened from Settings) so its
// own Save/Cancel apply or discard just the GPIO edits immediately, without
// touching Device/Network state sitting behind it.
document.getElementById("openGpioSettings").addEventListener("click", () => {
  gpioSettingsOverlay.classList.remove("hidden");
});

function closeGpioSettings({ save } = {}) {
  if (save) {
    sendLiveParamUpdate("@gpioSettings", { entries: pendingGpioEntries });
    loadedGpioEntries = pendingGpioEntries.map((e) => ({ ...e }));
  } else {
    pendingGpioEntries = loadedGpioEntries.map((e) => ({ ...e }));
    renderGpioEntryList();
    renderGpioAddPinOptions();
  }
  updateGpioSummary();
  gpioSettingsOverlay.classList.add("hidden");
}

document.getElementById("gpioSettingsSave").addEventListener("click", () => closeGpioSettings({ save: true }));
document.getElementById("gpioSettingsCancel").addEventListener("click", () => closeGpioSettings({ save: false }));
gpioSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === gpioSettingsOverlay) closeGpioSettings({ save: false });
});

// Same idea for Network (Ethernet + Wi-Fi): its own modal, its own
// Save/Cancel, independent of the main Settings dialog behind it.
document.getElementById("openNetworkSettings").addEventListener("click", () => {
  networkSettingsOverlay.classList.remove("hidden");
});

async function closeNetworkSettings({ save } = {}) {
  if (!save) {
    applyNetworkData(loadedNetworkData);
    updateNetworkSummary();
    networkSettingsOverlay.classList.add("hidden");
    return;
  }
  if (currentNetworkSnapshot() === loadedNetworkSnapshot) {
    networkSettingsOverlay.classList.add("hidden");
    return;
  }
  const body = {
    ethDhcp: ethDhcpInput.checked,
    ethIp: ethIpInput.value.trim(),
    ethMask: ethMaskInput.value.trim(),
    ethGw: ethGwInput.value.trim(),
    ethDns: ethDnsInput.value.trim(),
    wifiEnabled: wifiEnabledInput.checked,
    wifiSsid: wifiSsidInput.value.trim(),
    wifiPassword: wifiPasswordInput.value,
    wifiDhcp: wifiDhcpInput.checked,
    wifiIp: wifiIpInput.value.trim(),
    wifiMask: wifiMaskInput.value.trim(),
    wifiGw: wifiGwInput.value.trim(),
    wifiDns: wifiDnsInput.value.trim(),
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
    loadedNetworkData = { ...loadedNetworkData, ...body };
    loadedNetworkSnapshot = currentNetworkSnapshot();
    updateNetworkSummary();
    networkSettingsOverlay.classList.add("hidden");
    showToast(result.applied === false ? "Saved; Wi-Fi could not be applied" : "Saved — network changes applied");
  } catch (err) {
    appendLog(`Could not save network settings: ${err.message}`);
    showToast("Could not save network settings");
  }
}

document.getElementById("networkSettingsSave").addEventListener("click", () => closeNetworkSettings({ save: true }));
document.getElementById("networkSettingsCancel").addEventListener("click", () => closeNetworkSettings({ save: false }));
networkSettingsOverlay.addEventListener("click", (e) => {
  if (e.target === networkSettingsOverlay) closeNetworkSettings({ save: false });
});

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

document.getElementById("settingsSave").addEventListener("click", () => {
  const deviceParams = { name: deviceNameInput.value.trim() };
  if (ledStripInfo) {
    const ledPin = parseInt(ledStripPinSelect.value, 10);
    const ledCount = Math.max(0, Math.min(ledStripInfo.maxCount, parseInt(ledStripCountInput.value, 10) || 0));
    if (ledPin !== ledStripInfo.pin || ledCount !== ledStripInfo.count) {
      deviceParams.ledPin = Number.isFinite(ledPin) ? ledPin : -1;
      deviceParams.ledCount = ledCount;
      ledStripInfo = { ...ledStripInfo, pin: deviceParams.ledPin, count: ledCount };
      showToast("LED string settings take effect after a reboot");
    }
  }
  sendLiveParamUpdate("@deviceSettings", deviceParams);
  const loadSamples = loadSamplesSetting.checked;
  try {
    localStorage.setItem(LOAD_SAMPLES_KEY, loadSamples ? "1" : "0");
  } catch (err) {
    // Private mode / blocked storage: the choice just lasts for this page.
  }
  audioEngine.setSampleLoadingEnabled(loadSamples);
  // Switching it back on fetches whatever was skipped so far right away.
  if (loadSamples) audioEngine.loadDeferredSamples();
  closeSettings();
  showToast("Saved");
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

const presetDescriptionOverlay = document.getElementById("presetDescriptionOverlay");
const presetDescriptionTitleInput = document.getElementById("presetDescriptionTitleInput");
const presetDescriptionShortInput = document.getElementById("presetDescriptionShortInput");
const presetDescriptionLongInput = document.getElementById("presetDescriptionLongInput");
const presetDescriptionAuthorInput = document.getElementById("presetDescriptionAuthorInput");

function openPresetDescriptionModal() {
  presetDescriptionTitleInput.value = presetDescription.title || currentPresetName || "New";
  presetDescriptionShortInput.value = presetDescription.description;
  presetDescriptionLongInput.value = presetDescription.longDescription;
  presetDescriptionAuthorInput.value = presetDescription.author;
  presetDescriptionOverlay.classList.remove("hidden");
  presetDescriptionTitleInput.focus();
}

function closePresetDescriptionModal() {
  presetDescriptionOverlay.classList.add("hidden");
}

document.getElementById("presetDescriptionCancel").addEventListener("click", closePresetDescriptionModal);

document.getElementById("presetDescriptionSave").addEventListener("click", () => {
  presetDescription = {
    title: presetDescriptionTitleInput.value.trim(),
    description: presetDescriptionShortInput.value.trim(),
    longDescription: presetDescriptionLongInput.value,
    author: presetDescriptionAuthorInput.value.trim(),
  };
  closePresetDescriptionModal();
  savePresetNow();
});

document.getElementById("menuPresetDescription").addEventListener("click", (e) => {
  e.stopPropagation();
  menuItems.forEach((m) => m.classList.remove("open"));
  openPresetDescriptionModal();
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
  await applyPresetData(data, { fitToView: true });
  await savePresetNow();
  await refreshPresetMenu();
  showToast("Preset imported");
}

async function backupSystemToFile() {
  try {
    const res = await fetch("/api/backup");
    if (!res.ok) {
      showToast("Backup failed");
      return;
    }
    const blob = await res.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
    link.download = `espressynth-backup-${stamp}.json`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
    showToast("Backup downloaded");
  } catch (err) {
    appendLog(`Backup failed: ${err.message}`);
    showToast("Backup failed");
  }
}

async function restoreSystemFromFile(file) {
  let data;
  try {
    data = JSON.parse(await file.text());
  } catch (err) {
    appendLog(`Backup file is not valid JSON: ${err.message}`);
    showToast("Invalid backup file");
    return;
  }
  if (!data || typeof data !== "object" || typeof data.presets !== "object") {
    appendLog("Backup file has an unexpected format.");
    showToast("Invalid backup file");
    return;
  }
  if (!confirm("Restore this backup? This overwrites presets with the same name on the device.")) {
    return;
  }

  try {
    const res = await fetch("/api/backup/restore", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(data),
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({}));
      showToast(`Restore failed: ${err.error || res.status}`);
      return;
    }
    const result = await res.json();
    await loadPresetByName(null, { activate: false });
    showToast(`Restored ${result.count ?? 0} preset(s)`);
  } catch (err) {
    appendLog(`Restore failed: ${err.message}`);
    showToast("Restore failed");
  }
}



const LINKED_INPUT_TYPES = new Set(["linked-input", "linked-input-4", "linked-input-8"]);
const LINKED_OUTPUT_TYPES = new Set(["linked-output", "linked-output-4", "linked-output-8"]);
const isLinkedInputType = (type) => LINKED_INPUT_TYPES.has(type);
const isLinkedOutputType = (type) => LINKED_OUTPUT_TYPES.has(type);
const isLinkedIoType = (type) => isLinkedInputType(type) || isLinkedOutputType(type);
const linkedIoCountForType = (type) => type.endsWith("-8") ? 8 : (type.endsWith("-4") ? 4 : 1);
const linkedIoPortId = (direction, index, count) => count === 1 ? direction : `${direction}${index + 1}`;

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
      { id: "aftertouch", label: "Aftertouch" },
      { id: "pitchBend", label: "Pitch Bend" },
      { id: "note", label: "Note (0–127)" },
      { id: "velocity", label: "Velocity" },
      { id: "gate", label: "Gate" },
    ],
  },
  "midi-filter": {
    label: "MIDI-Filter",
    icon: "🎹",
    inputs: [{ id: "midi", label: "MIDI-In" }],
    outputs: [
      { id: "midi", label: "MIDI-Out" },
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
    inputs: [{ id: "layer", label: "Layer" }],
    outputs: [
      { id: "voltage", label: "" },
      ...Array.from({ length: 16 }, (_, knobIndex) =>
        Array.from({ length: 8 }, (_, layerIndex) => ({
          id: layerIndex === 0 ? `voltage${knobIndex + 1}` : `voltage${knobIndex + 1}Layer${layerIndex + 1}`,
          label: `${knobIndex + 1}.${layerIndex + 1}`,
        }))
      ).flat(),
    ],
  },
  button: {
    label: "Button",
    icon: "🔘",
    inputs: [{ id: "layer", label: "Layer" }],
    outputs: Array.from({ length: 16 }, (_, buttonIndex) =>
      Array.from({ length: 8 }, (_, layerIndex) => ({
        id: layerIndex === 0 ? `value${buttonIndex + 1}` : `value${buttonIndex + 1}Layer${layerIndex + 1}`,
        label: `${buttonIndex + 1}.${layerIndex + 1}`,
      }))
    ).flat(),
  },
  "up-down-button": {
    label: "Up/Down Button",
    icon: "🔼",
    inputs: [],
    outputs: [{ id: "value", label: "" }],
  },
  switch: {
    label: "Input-Switch",
    icon: "🔀",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  multiplexer: {
    label: "Input-Multiplexer",
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
  "output-switch": {
    label: "Output Switch",
    icon: "🔀",
    inputs: [{ id: "in", label: "In" }],
    outputs: [
      { id: "out1", label: "Out 1" },
      { id: "out2", label: "Out 2" },
    ],
  },
  "output-multiplexer": {
    label: "Output Multiplexer",
    icon: "🎯",
    // Static/default shape (2 outputs) - live output count (2-16) is
    // handled by renderOutputMultiplexerPorts() below, same pattern as
    // Multiplexer's own renderMultiplexerPorts().
    inputs: [
      { id: "in", label: "In" },
      { id: "select", label: "Select" },
    ],
    outputs: [
      { id: "out1", label: "Out 1" },
      { id: "out2", label: "Out 2" },
    ],
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
  "logic-and": {
    label: "AND",
    icon: "&",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  "logic-or": {
    label: "OR",
    icon: "≥1",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  "logic-xor": {
    label: "XOR",
    icon: "=1",
    inputs: [
      { id: "in1", label: "In 1" },
      { id: "in2", label: "In 2" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  "logic-not": {
    label: "NOT",
    icon: "¬",
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
    // Tempo comes from the single Master Clock (tab bar, see
    // openMasterClockSettings) - this widget only owns its own pulse width.
    inputs: [],
    // "frequency" mirrors the current Master Clock tempo (see
    // propagateClockFrequency below) - control-rate like "pulse" (see
    // AUDIO_PORTS' Clock omission comment), not a real Web Audio node.
    outputs: [
      { id: "pulse", label: "Pulse" },
      { id: "frequency", label: "Frequency" },
    ],
  },
  "cv-in": {
    label: "CV In",
    icon: "🔌",
    // Port/address/channel picked in a settings modal from the live boot-time I2C scan
    // (see openCvInSettings) - no live value simulation client-side, same reasoning as
    // Clock never calling audioEngine.createNode().
    inputs: [],
    outputs: [{ id: "value", label: "Value" }],
  },
  "cv-out": {
    label: "CV Out",
    icon: "🔌",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
  "digital-in": {
    label: "Digital In",
    icon: "🔲",
    // Pin picked in a settings modal from the user-configured GPIO table (Settings dialog,
    // see openDigitalInSettings) - no live value simulation client-side, same reasoning as
    // cv-in above.
    inputs: [],
    outputs: [{ id: "value", label: "Value" }],
  },
  "digital-out": {
    label: "Digital Out",
    icon: "🔲",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
  // One LED of the WS2812 string (Settings -> LED String). The color is computed by the
  // firmware and mirrored back via cvStatus - no client-side simulation, same as cv-out.
  "rgb-led": {
    label: "LED",
    icon: "💡",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
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
      { id: "feedback", label: "Feedback (Self FB)" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  "fm-oscillator": {
    label: "Oscillator",
    icon: "🔔",
    // Modular single-operator phase-modulation FM: no built-in modulator -
    // patch any node's audio output (typically a second WaveGen) into
    // "fmIn" to modulate this carrier. Gate retriggers carrier phase and
    // self-feedback state, matching WaveGen.
    inputs: [
      { id: "fmIn", label: "FM In" },
      { id: "fmIndex", label: "FM Ratio" },
      { id: "pitchCv", label: "Pitch" },
      { id: "tune", label: "Tune" },
      { id: "amplitude", label: "Amplitude" },
      { id: "waveform", label: "Waveform" },
      { id: "gate", label: "Gate" },
      { id: "gain", label: "Gain" },
      { id: "feedback", label: "Feedback (Self)" },
      // Square-only (see updateFmOscillatorDutyCycleVisual). 0-10V CV,
      // 5V = classic 50% square, matching the firmware default.
      { id: "dutyCycle", label: "Duty Cycle" },
      { id: "superSawDetune", label: "SuperSaw Detune" },
      { id: "superSawMix", label: "SuperSaw Mix" },
      // Wavetable-only (row hidden otherwise): 0-10V = first..last table.
      { id: "morph", label: "Morph" },
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
      { id: "gain", label: "Gain" },
    ],
    outputs: [
      { id: "left", label: "Out L" },
      { id: "right", label: "Out R" },
    ],
  },
  "soundfont-player": {
    label: "SoundFont Player",
    icon: "🎼",
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
  "mda-piano": {
    label: "MDA Piano",
    icon: "🎹",
    inputs: [
      { id: "midi", label: "MIDI" },
      { id: "enableModWheel", label: "Enable Mod Wheel" },
      { id: "enablePitchWheel", label: "Enable Pitch Wheel" },
      { id: "voiceType", label: "Voice Type" },
      { id: "drive", label: "Drive" },
      { id: "tremWahMode", label: "Trem/Wah Mode" },
      { id: "tremWahDepth", label: "Trem/Wah Depth" },
      { id: "tremWahRate", label: "Trem/Wah Rate" },
      { id: "choPhaMode", label: "Cho/Pha Mode" },
      { id: "choPhaDepth", label: "Cho/Pha Depth" },
      { id: "choPhaSpeed", label: "Cho/Pha Speed" },
      { id: "preGain", label: "Pre Gain" },
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
    outputs: [{ id: "out", label: "Out" }],
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
      { id: "amount", label: "Output Gain" },
      { id: "curve", label: "Curve" },
      { id: "global", label: "Global" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  filter: {
    label: "Filter",
    icon: "🎚️",
    inputs: [
      { id: "in", label: "In" },
      { id: "filterType", label: "Filter Type" },
      { id: "slope", label: "Slope" },
      { id: "cutoff", label: "Cutoff" },
      { id: "resonance", label: "Resonance" },
      { id: "global", label: "Global" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "comb-filter": {
    label: "Comb Filter",
    icon: "🎛️",
    inputs: [
      { id: "in", label: "In" },
      { id: "time", label: "Delay" },
      { id: "feedback", label: "Feedback" },
      { id: "mix", label: "Mix" },
      { id: "polarity", label: "Feedback ±" },
      { id: "damping", label: "Damping" },
      { id: "gain", label: "Output Gain" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "curtis-filter": {
    label: "CEM3320 Filter",
    icon: "🎚️",
    inputs: [
      { id: "in", label: "In" },
      { id: "filterType", label: "Filter Type" },
      { id: "slope", label: "Slope" },
      { id: "cutoff", label: "Cutoff" },
      { id: "resonance", label: "Resonance" },
      { id: "global", label: "Global" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "esp-eq": { label: "EQ", icon: "📶", inputs: [{ id: "inL", label: "In L" }, { id: "inR", label: "In R" }, { id: "bypass", label: "Bypass" }, { id: "frequency", label: "Frequency" }, { id: "q", label: "Q" }, { id: "gainDb", label: "Gain dB" }], outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }] },
  "graphic-eq": {
    label: "8-Band EQ",
    icon: "📶",
    inputs: [
      { id: "inL", label: "In L" },
      { id: "inR", label: "In R" },
      { id: "bypass", label: "Bypass" },
      { id: "band1", label: "63 Hz" },
      { id: "band2", label: "125 Hz" },
      { id: "band3", label: "250 Hz" },
      { id: "band4", label: "500 Hz" },
      { id: "band5", label: "1 kHz" },
      { id: "band6", label: "2 kHz" },
      { id: "band7", label: "5 kHz" },
      { id: "band8", label: "14 kHz" },
      { id: "gain", label: "Gain" },
    ],
    outputs: [{ id: "outL", label: "Out L" }, { id: "outR", label: "Out R" }],
  },
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
  "dx-envelope": {
    label: "DX Envelope",
    icon: "⌁",
    inputs: [
      { id: "gate", label: "Gate" },
      { id: "velocity", label: "Velocity" },
      { id: "level1", label: "Level 1" },
      { id: "time1", label: "Time 1" },
      { id: "level2", label: "Level 2" },
      { id: "time2", label: "Time 2" },
      { id: "level3", label: "Level 3" },
      { id: "time3", label: "Time 3" },
      { id: "level4", label: "Level 4" },
      { id: "time4", label: "Time 4" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "step-arpeggiator": {
    label: "Step Arpeggiator",
    icon: "▦",
    // Monophonic 8/16-step CV sequencer/arpeggiator - external clock/gate
    // drive playback, "direction" picks Forward/Backward/Ping-Pong/Random
    // (see renderStepArpPorts() below for the sixteen "stepN" rows,
    // drawn as a dense knob row rather than the usual vertical port list).
    inputs: [
      { id: "clock", label: "Clock" },
      { id: "gate", label: "Gate" },
      { id: "pitch", label: "Pitch" },
      { id: "direction", label: "Direction" },
      { id: "pulseTime", label: "Gate Out Time" },
      // Internal fallback clock's own rate - only used while "clock" above
      // is itself unwired (see stepArpRestartInternalClock).
      { id: "rate", label: "Rate" },
      ...Array.from({ length: 16 }, (_, i) => ({ id: `step${i + 1}`, label: `Step ${i + 1}` })),
    ],
    outputs: [
      { id: "cv", label: "CV" },
      { id: "gateOut", label: "Gate Out" },
    ],
  },
  "drum-sequencer": {
    label: "Drum Sequencer",
    icon: "▦",
    inputs: [
      { id: "clock", label: "Clock" },
      { id: "gate", label: "Gate" },
      { id: "pattern", label: "Pattern" },
      { id: "rate", label: "Rate" },
      { id: "switchAtEnd", label: "Switch @ End" },
    ],
    outputs: Array.from({ length: 8 }, (_, i) => [
      { id: `trigger${i + 1}`, label: `T${i + 1}` },
      { id: `velocity${i + 1}`, label: `V${i + 1}` },
    ]).flat(),
  },
  const: {
    label: "Const",
    icon: "🔢",
    inputs: [],
    outputs: [{ id: "value", label: "" }],
  },
  bitcrusher: {
    label: "Bitcrusher",
    icon: "▦",
    inputs: [
      { id: "in", label: "In" },
      { id: "bypass", label: "Bypass" },
      { id: "crush", label: "Crush" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "wave-folder": {
    label: "Wave Folder",
    icon: "〰",
    inputs: [
      { id: "in", label: "In" },
      { id: "drive", label: "Drive" },
      { id: "symmetry", label: "Symmetry" },
      { id: "gain", label: "Gain" },
      { id: "mix", label: "Mix" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "envelope-follower": {
    label: "Envelope Follower",
    icon: "◟◝",
    inputs: [
      { id: "in", label: "In" },
      { id: "attack", label: "Attack" },
      { id: "release", label: "Release" },
      { id: "preGain", label: "PreGain" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  "burst-generator": {
    label: "Burst Gen",
    pickerLabel: "Burst Generator / Trigger Delay",
    icon: "⁘",
    inputs: [
      { id: "in", label: "In" },
      { id: "mode", label: "Mode" },
      { id: "count", label: "Count" },
      { id: "time", label: "Time" },
      { id: "decay", label: "Decay" },
    ],
    outputs: [{ id: "out", label: "Out" }],
  },
  pan: {
    label: "Pan",
    icon: "⇄",
    inputs: [
      { id: "in", label: "In" },
      { id: "pan", label: "Pan" },
    ],
    outputs: [
      { id: "out1", label: "Out1" },
      { id: "out2", label: "Out2" },
    ],
  },
  "sample-hold": {
    label: "S&H",
    pickerLabel: "S&H - Sample and Hold",
    icon: "⌁",
    inputs: [
      { id: "in", label: "In" },
      { id: "trigger", label: "Trigger" },
      { id: "threshold", label: "Threshold" },
    ],
    outputs: [{ id: "out", label: "Out" }],
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
  "reverb-light": {
    label: "Reverb",
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
  "linked-input": {
    label: "Link In",
    icon: "📥",
    inputs: [{ id: "in", label: "In" }],
    outputs: [],
  },
  "linked-output": {
    label: "Link Out",
    icon: "📤",
    inputs: [],
    outputs: [{ id: "out", label: "Out" }],
  },
  "linked-input-4": {
    label: "Link In 4×",
    icon: "📥",
    inputs: Array.from({ length: 4 }, (_, index) => ({ id: `in${index + 1}`, label: String(index + 1) })),
    outputs: [],
  },
  "linked-output-4": {
    label: "Link Out 4×",
    icon: "📤",
    inputs: [],
    outputs: Array.from({ length: 4 }, (_, index) => ({ id: `out${index + 1}`, label: String(index + 1) })),
  },
  "linked-input-8": {
    label: "Link In 8×",
    icon: "📥",
    inputs: Array.from({ length: 8 }, (_, index) => ({ id: `in${index + 1}`, label: String(index + 1) })),
    outputs: [],
  },
  "linked-output-8": {
    label: "Link Out 8×",
    icon: "📤",
    inputs: [],
    outputs: Array.from({ length: 8 }, (_, index) => ({ id: `out${index + 1}`, label: String(index + 1) })),
  },
  lfo: {
    label: "LFO",
    icon: "🔄",
    inputs: [
      { id: "rate", label: "Rate" },
      { id: "phase", label: "Phase" },
      { id: "reset", label: "Reset" },
      { id: "oneShot", label: "One Shot" },
      { id: "global", label: "Global" },
      { id: "zeroPoint", label: "Offset" },
      { id: "gain", label: "Gain" },
    ],
    outputs: [{ id: "value", label: "Out" }],
  },
  // Own widget (not an LFO mode) so ordinary LFO presets never carry the
  // table payload. Shares the LFO's state/timer/reset/one-shot/global logic
  // (lfoStates, tickLfo, ...) - see isLfoLikeType().
  wavetable: {
    label: "Wavetable",
    icon: "∿",
    inputs: [
      { id: "rate", label: "Rate" },
      { id: "morph", label: "Morph" },
      { id: "reset", label: "Reset" },
      { id: "oneShot", label: "One Shot" },
      { id: "global", label: "Global" },
      { id: "gain", label: "Gain" },
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
  { label: "Midi", types: ["midi-in", "midi-split", "midi-filter"] },
  { label: "Generator", types: ["fm-oscillator", "hammond", "wave-player", "soundfont-player", "mda-piano"] },
  { label: "Modulator", types: ["lfo", "wavetable", "adsr", "dx-envelope", "detune", "clock", "step-arpeggiator", "drum-sequencer", "sample-hold", "burst-generator"] },
  { label: "Control", types: ["knob", "button", "up-down-button", "switch", "multiplexer", "output-switch", "output-multiplexer", "const"] },
  { label: "Math", types: ["adder", "divide", "frequency-divider", "multiplier", "abs", "value-mapper", "logic-and", "logic-or", "logic-xor", "logic-not"] },
  { label: "Filter", types: ["filter", "curtis-filter", "comb-filter", "esp-eq", "graphic-eq"] },
  { label: "Effects", types: ["bitcrusher", "wave-folder", "envelope-follower", "pan", "chorus", "delay", "reverb-light"] },
  { label: "Mixing/Output", types: ["gain", "mixer", "audio-out", "linked-input", "linked-output", "linked-input-4", "linked-output-4", "linked-input-8", "linked-output-8"] },
  { label: "External Audio", types: ["usb-out", "es8311-out", "wm8960-out"] },
  { label: "Misc", types: ["annotation", "debug", "voltmeter", "vu-meter", "vu-meter-horizontal", "oscilloscope", "spectrum"] },
  { label: "External Hardware", types: ["cv-in", "cv-out", "digital-in", "digital-out", "rgb-led"] },
];



const AUDIO_PORTS = {
  
  
  
  
  
  
  oscillator: { in: ["amplitude", "gain", "pitchCv", "tune", "feedback"], out: ["value"] },
  "fm-oscillator": { in: ["amplitude", "gain", "pitchCv", "tune", "fmIn", "fmIndex", "feedback", "dutyCycle", "superSawDetune", "superSawMix"], out: ["value"] },
  // All 13 are real audio-rate ports here (unlike Mixer's control-only
  // "gainN") - each of the 9 drawbar levels is independently CV-connectable.
  hammond: {
    in: ["amplitude", "pitchCv", "tune", "gain",
         "drawbar1", "drawbar2", "drawbar3", "drawbar4", "drawbar5", "drawbar6", "drawbar7", "drawbar8", "drawbar9"],
    out: ["value"],
  },
  switch: { in: ["in1", "in2"], out: ["value"] },
  "output-switch": { in: ["in"], out: ["out1", "out2"] },
  // "select" (CV) is deliberately absent here - control-rate, same reasoning
  // as C++ PortTables' isControlOnlyInput (see nodes/MultiplexerNode.h).
  multiplexer: { in: ["in1", "in2", "in3", "in4", "in5", "in6", "in7", "in8", "in9", "in10", "in11", "in12", "in13", "in14", "in15", "in16"], out: ["value"] },
  // "select" (CV) deliberately absent, same reasoning as multiplexer above.
  "output-multiplexer": { in: ["in"], out: ["out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8", "out9", "out10", "out11", "out12", "out13", "out14", "out15", "out16"] },
  
  
  
  "wave-player": { in: ["amplitude", "gain"], out: ["left", "right"] },
  "soundfont-player": { out: ["left", "right"] },
  "mda-piano": { out: ["left", "right"] },
  
  
  
  lfo: { out: ["value"] },
  wavetable: { out: ["value"] },
  // Const is a scalar control source. It is not an audio-rate output.
  const: { out: [] },
  
  
  
  
  "audio-out": { in: ["left", "right"] },
  
  
  
  
  
  
  gain: { in: ["in", "amount", "curve"], out: ["out"] },
  
  
  
  
  filter: { in: ["in", "cutoff", "resonance"], out: ["out"] },
  "curtis-filter": { in: ["in", "cutoff", "resonance"], out: ["out"] },
  "comb-filter": { in: ["in", ...COMB_FILTER_PORTS], out: ["out"] },
  "esp-eq": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "graphic-eq": { in: ["inL", "inR"], out: ["outL", "outR"] },









  mixer: { in: ["in1", "in2", "in3", "in4", "in5", "in6", "in7", "in8"], out: ["out"] },
  
  
  
  
  
  
  
  
  
  
  
  
  
  // "gate" is a real, wireable audio-rate port (e.g. from a CV Step-
  // Arpeggiator's "gateOut") - att/dec/sus/rel stay unlisted here (existing,
  // unrelated gap: the C++ engine already treats them as real ports too,
  // this table just never reflected it).
  adsr: { in: ["gate"], out: ["out"] },
  "dx-envelope": { in: ["gate"], out: ["out"] },
  "step-arpeggiator": {
    in: ["clock", "gate", "pitch", "direction", "pulseTime", "rate", ...Array.from({ length: 16 }, (_, i) => `step${i + 1}`)],
    // "cv"/"gateOut" are genuine audio-rate outputs in the C++ engine, but
    // deliberately left OUT of this list - same reasoning as Clock's own
    // "pulse" (also absent here): propagate()'s Voltmeter/Debug delivery
    // skips any "audio"-kind source, on the assumption that
    // tickDebugVoltmeterAudioSources() will sample it instead from a real
    // Web Audio node - but this widget (like Clock) never calls
    // audioEngine.createNode(), so no such node exists to sample. Leaving
    // these classified "control" here routes their propagate()d value
    // through the normal deliverToWidget path instead, which is the only
    // path that will ever actually reach a connected Voltmeter/Debug widget.
    out: [],
  },
  "drum-sequencer": { in: ["clock", "gate", "pattern", "rate", "switchAtEnd"], out: [] },
  chorus: { in: ["in"], out: ["left", "right"] },
  bitcrusher: { in: ["in"], out: ["out"] },
  "wave-folder": { in: ["in", "drive", "symmetry", "gain", "mix"], out: ["out"] },
  "envelope-follower": { in: ["in", "attack", "release", "preGain"], out: ["out"] },
  "burst-generator": { in: ["in", "mode", "count", "time", "decay"], out: ["out"] },
  pan: { in: ["in", "pan"], out: ["out1", "out2"] },
  "sample-hold": { in: ["in", "trigger", "threshold"], out: ["out"] },
  
  
  
  delay: { in: ["inL", "inR"], out: ["outL", "outR"] },

  "reverb-light": { in: ["inL", "inR"], out: ["outL", "outR"] },
  "audio-output-link": { in: Array.from({ length: LINK_BUS_COUNT }, (_, i) => `in${i + 1}`) },
  "audio-input-link": {
    in: Array.from({ length: LINK_BUS_COUNT }, (_, i) => `gain${i + 1}`),
    out: Array.from({ length: LINK_BUS_COUNT }, (_, i) => `out${i + 1}`),
  },
  "linked-input": { in: ["in"] },
  "linked-output": { out: ["out"] },
  "linked-input-4": { in: Array.from({ length: 4 }, (_, index) => `in${index + 1}`) },
  "linked-output-4": { out: Array.from({ length: 4 }, (_, index) => `out${index + 1}`) },
  "linked-input-8": { in: Array.from({ length: 8 }, (_, index) => `in${index + 1}`) },
  "linked-output-8": { out: Array.from({ length: 8 }, (_, index) => `out${index + 1}`) },
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
  "logic-and": { states: () => logicGateStates, audioPorts: "all" },
  "logic-or": { states: () => logicGateStates, audioPorts: "all" },
  "logic-xor": { states: () => logicGateStates, audioPorts: "all" },
  "logic-not": { states: () => logicGateStates, audioPorts: "all" },
  detune: { states: () => detuneStates, audioPorts: { in: ["cv"], out: ["out"] } },
};

function getPortKind(widgetId, portId, direction, resolvingLinks = new Set()) {
  const el = document.getElementById(widgetId);
  if (!el) return "control";
  const type = el.dataset.widgetType;

  // Linked I/O carries either a scalar control value or a real audio/CV
  // signal.  Derive that mode from the Link In's upstream source instead of
  // treating every link as audio.  The latter creates a silent GainNode for
  // MIDI Pitch/Gate and incorrectly takes ownership of audio-rate targets.
  if (isLinkedInputType(type) && direction === "in") {
    const key = `${portId}:${widgetId}`;
    if (resolvingLinks.has(key)) return "control";
    const next = new Set(resolvingLinks);
    next.add(key);
    return connections.some(
      (c) => c.to.widgetId === widgetId && c.to.portId === portId &&
        getPortKind(c.from.widgetId, c.from.portId, "out", next) === "audio"
    ) ? "audio" : "control";
  }

  if (isLinkedOutputType(type) && direction === "out") {
    const key = `${portId}:${widgetId}`;
    if (resolvingLinks.has(key)) return "control";
    const sourceLinkId = linkedOutputStates.get(widgetId)?.sourceLinkId;
    if (!sourceLinkId) return "control";
    const sourceWidgetId = [...linkedInputStates.entries()].find(
      ([, state]) => state.linkId === sourceLinkId
    )?.[0];
    if (!sourceWidgetId) return "control";
    const next = new Set(resolvingLinks);
    next.add(key);
    const count = linkedIoCountForType(type);
    const portIndex = count === 1 ? 0 : Math.max(0, Number(portId.slice(3)) - 1);
    return getPortKind(sourceWidgetId, linkedIoPortId("in", portIndex, count), "in", next);
  }

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

// Inline oscilloscope panel embedded under an oscillator/fm-oscillator
// widget's own controls - shown/hidden in bulk via body.show-oscilloscopes
// (see setOscilloscopesVisible), drawn by tickAudioMonitors using
// audioEngine.getOscilloscopeWaveform (own analyser tap on the node's
// "value" output, independent of the patchable "oscilloscope" widget).
function appendOscilloscopePanel(body, port = "value") {
  const panel = document.createElement("div");
  panel.className = "osc-scope-panel";
  const canvas = document.createElement("canvas");
  canvas.className = "osc-scope-canvas";
  canvas.dataset.role = "osc-scope-canvas";
  canvas.dataset.scopePort = port;
  canvas.width = 160;
  canvas.height = 48;
  panel.appendChild(canvas);
  body.appendChild(panel);
}

const OSCILLOSCOPE_WIDGET_TYPES = ["oscillator", "fm-oscillator", "hammond", "wave-player", "soundfont-player", "mda-piano"];

const GRAPHIC_EQ_DEFAULT_FREQS = [63, 125, 250, 500, 1000, 2000, 5000, 14000];

function formatHzLabel(hz) {
  return hz >= 1000 ? `${formatNumber(hz / 1000)} kHz` : `${formatNumber(hz)} Hz`;
}

function formatGraphicEqHzLabel(hz) {
  const rounded = Math.round(Number(hz));
  return rounded >= 1000 && rounded % 1000 === 0 ? `${rounded / 1000} kHz` : `${rounded} Hz`;
}

function formatGraphicEqAxisLabel(hz) {
  const rounded = Math.round(Number(hz));
  return rounded >= 1000 && rounded % 1000 === 0 ? `${rounded / 1000}k` : `${rounded}`;
}

function validGraphicEqFrequencies(frequencies) {
  return Array.isArray(frequencies) && frequencies.length === GRAPHIC_EQ_DEFAULT_FREQS.length &&
    frequencies.every((hz, i) => Number.isInteger(Number(hz)) && Number(hz) >= 20 && Number(hz) <= 20000 &&
      (i === 0 || Number(hz) > Number(frequencies[i - 1])));
}

const WAVE_TYPE_LABELS = {
  sine: "Sine",
  square: "Square",
  triangle: "Triangle",
  sawtooth: "Sawtooth",
  "sawtooth-down": "Sawtooth Down",
  "linear-up": "Linear Up",
  "linear-down": "Linear Down",
  noise: "Noise",
  "pink-noise": "Pink Noise",
  supersaw: "SuperSaw",
  wavetable: "Wavetable",
};

const WAVE_ICON_ATTRS = 'viewBox="0 0 100 100" fill="none" stroke="currentColor" stroke-linecap="round" stroke-linejoin="round"';

const WAVE_TYPE_BUTTONS = [
  {
    id: "sine",
    color: "#06b6d4",
    glow: "rgba(6, 182, 212, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="9"><path d="M 6,50 Q 28,8 50,50 T 94,50"/></svg>`,
  },
  {
    id: "linear-up",
    color: "#22c55e",
    glow: "rgba(34, 197, 94, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="9"><path d="M 10,84 L 90,16"/></svg>`,
  },
  {
    id: "linear-down",
    color: "#f59e0b",
    glow: "rgba(245, 158, 11, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="9"><path d="M 10,16 L 90,84"/></svg>`,
  },
  {
    id: "sawtooth",
    color: "#3b82f6",
    glow: "rgba(59, 130, 246, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="9"><path d="M 10,84 L 90,16 L 90,84"/></svg>`,
  },
  {
    id: "sawtooth-down",
    color: "#0ea5e9",
    glow: "rgba(14, 165, 233, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="9"><path d="M 10,16 L 90,84 L 90,16"/></svg>`,
  },
  {
    id: "triangle",
    color: "#06b6d4",
    glow: "rgba(6, 182, 212, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="9"><path d="M 8,82 L 50,18 L 92,82"/></svg>`,
  },
  {
    id: "square",
    color: "#8b5cf6",
    glow: "rgba(139, 92, 246, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="9"><path d="M 10,84 L 10,16 L 50,16 L 50,84 L 90,84 L 90,16"/></svg>`,
  },
  {
    id: "pink-noise",
    color: "#ec4899",
    glow: "rgba(236, 72, 153, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 6,50 L 18,18 L 30,82 L 42,22 L 54,78 L 66,15 L 78,85 L 94,50"/></svg>`,
  },
  {
    id: "noise",
    color: "#10b981",
    glow: "rgba(16, 185, 129, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="6"><path d="M 6,65 L 14,15 L 22,85 L 30,10 L 38,88 L 46,20 L 54,80 L 62,12 L 70,85 L 78,25 L 86,75 L 94,50"/></svg>`,
  },
];

// Button row AND "waveform" CV band order (FmOscillatorNode.h's waveTypeFromCv() mirrors it):
// the shapes up to Square, then SuperSaw, Wavetable, and the two noises last.
const FM_WAVE_TYPE_BUTTONS = [
  ...WAVE_TYPE_BUTTONS.filter((w) => w.id !== "pink-noise" && w.id !== "noise"),
  {
    id: "supersaw",
    color: "#ef4444",
    glow: "rgba(239, 68, 68, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="7"><path d="M 6,80 L 46,20 L 46,80 L 86,20" opacity=".4"/><path d="M 6,70 L 42,14 L 42,70 L 82,14" opacity=".7"/><path d="M 10,88 L 50,26 L 50,88 L 90,26"/></svg>`,
  },
  // Morphable, self-drawn wavetable bank (same editor/format as the Wavetable widget).
  {
    id: "wavetable",
    color: "#a855f7",
    glow: "rgba(168, 85, 247, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,90 Q 25,90 40,50 Q 55,10 70,50 Q 85,90 90,10"/></svg>`,
  },
  ...WAVE_TYPE_BUTTONS.filter((w) => w.id === "pink-noise" || w.id === "noise"),
];



const LFO_TYPE_BUTTONS = WAVE_TYPE_BUTTONS.filter((w) => w.id !== "noise" && w.id !== "pink-noise");

// Same icon-button treatment as the oscillator/filter type buttons (shared
// .osc-wave-btn styling) instead of plain text pills.
const LFO_ZERO_MODE_BUTTONS = [
  {
    id: "auto",
    title: "Auto: offset = half amplitude",
    color: "#06b6d4",
    glow: "rgba(6, 182, 212, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,50 Q 30,20 50,50 Q 70,80 90,50"/></svg>`,
  },
  {
    id: "fixed",
    title: "Fixed: offset set manually below",
    color: "#f59e0b",
    glow: "rgba(245, 158, 11, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,80 L 90,80" opacity=".55"/><path d="M 10,60 Q 30,30 50,60 Q 70,90 90,60"/></svg>`,
  },
];





const GENERATOR_PAGE_TYPES = [
  "midi-in", "midi-split", "knob", "button", "multiplier", "adder", "divide", "frequency-divider", "abs", "value-mapper", "logic-and", "logic-or", "logic-xor", "logic-not", "clock", "oscillator", "fm-oscillator", "hammond", "wave-player", "soundfont-player", "mda-piano", "detune", "adsr", "dx-envelope", "step-arpeggiator", "drum-sequencer", "const", "bitcrusher", "wave-folder", "envelope-follower", "pan",
];
const FX_PAGE_TYPES = [
  "midi-in", "adsr",
  "bitcrusher", "wave-folder", "envelope-follower", "pan", "chorus", "delay", "reverb-light", "audio-out",
  "usb-out", "es8311-out", "wm8960-out",
];







const DUAL_PAGE_TYPES = ["midi-filter", "annotation", "knob", "button", "up-down-button", "switch", "multiplexer", "output-switch", "output-multiplexer", "debug", "voltmeter", "lfo", "wavetable", "vu-meter", "vu-meter-horizontal", "oscilloscope", "spectrum", "gain", "filter", "curtis-filter", "comb-filter", "mixer", "esp-eq", "graphic-eq", ...LINKED_INPUT_TYPES, ...LINKED_OUTPUT_TYPES, "sample-hold", "burst-generator", "cv-in", "cv-out", "digital-in", "digital-out", "rgb-led"];



const LINK_WIDGET_TYPES = ["audio-output-link", "audio-input-link"];
const LINKED_IO_WIDGET_TYPES = [...LINKED_INPUT_TYPES, ...LINKED_OUTPUT_TYPES];
const OUTPUT_SINGLETON_TYPES = ["audio-out", "usb-out", "es8311-out", "wm8960-out"];

function makePageState(kind, title, canvasEl, canvasContentEl, connectionsLayerEl, lassoBoxEl) {
  return {
    kind,
    title,
    canvasEl,
    canvasContentEl,
    connectionsLayerEl,
    lassoBoxEl,
    zoom: 1,
    panX: 0,
    panY: 0,
    selectedWidgetIds: new Set(),
    clipboard: { widgets: [], connections: [] },
    allowedTypes: kind === "generator"
      ? [...GENERATOR_PAGE_TYPES, ...DUAL_PAGE_TYPES]
      : [...FX_PAGE_TYPES, ...DUAL_PAGE_TYPES],
    toMasterUi: { x: 40, y: 40 },
    canvasEventsBound: false,
  };
}

const pages = {
  generator: makePageState(
    "generator",
    "Sound Generation",
    document.getElementById("canvas-generator"),
    document.getElementById("canvasContent-generator"),
    document.getElementById("connectionsLayer-generator"),
    document.getElementById("lassoBox-generator")
  ),
  fx: makePageState(
    "fx",
    "FX & Masteroutput",
    document.getElementById("canvas-fx"),
    document.getElementById("canvasContent-fx"),
    document.getElementById("connectionsLayer-fx"),
    document.getElementById("lassoBox-fx")
  ),
};
let generatorPageOrder = ["generator"];
let pageCounter = 0;
let currentPageName = "generator";

let canvas = pages.generator.canvasEl;
let canvasContent = pages.generator.canvasContentEl;
let lassoBox = pages.generator.lassoBoxEl;
const zoomIndicator = document.getElementById("zoomIndicator");

const ZOOM_MIN = 0.05;
const ZOOM_MAX = 2.0;
const ZOOM_STEP = 0.05;

let zoom = pages.generator.zoom;
let panX = pages.generator.panX;
let panY = pages.generator.panY;
let widgetCounter = 0;
let selectedWidgetIds = pages.generator.selectedWidgetIds;
let clipboard = pages.generator.clipboard;

function generatorPageNames() {
  return generatorPageOrder.filter((name) => pages[name]?.kind === "generator");
}

function pageTitleForWidget(widgetId) {
  const widget = document.getElementById(widgetId);
  if (!widget) return "Unknown Page";
  const page = Object.values(pages).find((candidate) => candidate.canvasContentEl.contains(widget));
  return page?.title || "Unknown Page";
}

function sharedToMasterElement() {
  return document.querySelector('.widget-instance[data-widget-type="audio-output-link"]');
}

// Sound pages are views of one graph. Move the one real To Master widget
// between visible canvases instead of creating extra engine nodes.
function captureToMasterPosition(pageName) {
  const page = pages[pageName];
  const el = sharedToMasterElement();
  if (!page || page.kind !== "generator" || !el || !page.canvasContentEl.contains(el)) return;
  page.toMasterUi = {
    x: parseInt(el.style.left, 10) || 0,
    y: parseInt(el.style.top, 10) || 0,
  };
}

function moveToMasterToPage(pageName) {
  const page = pages[pageName];
  const el = sharedToMasterElement();
  if (!page || page.kind !== "generator" || !el) return;
  page.canvasContentEl.appendChild(el);
  el.style.left = `${page.toMasterUi.x}px`;
  el.style.top = `${page.toMasterUi.y}px`;
}

function switchPage(name) {
  if (name === currentPageName || !pages[name]) return;

  
  
  
  cancelPendingPaste();

  const prev = pages[currentPageName];
  captureToMasterPosition(currentPageName);
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
  if (next.kind === "generator") moveToMasterToPage(name);
  next.canvasEl.classList.remove("hidden");

  document.querySelectorAll(".page-tab").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.page === name);
  });

  applyTransform();
  updateSelectionClasses();
  drawConnections();
}

const pageTabList = document.getElementById("pageTabList");
const pageTabContextMenu = document.getElementById("pageTabContextMenu");
let contextPageName = null;

function pageTab(name) {
  return Array.from(pageTabList.querySelectorAll(".page-tab")).find((tab) => tab.dataset.page === name) || null;
}

function nextPageId(requestedId = "") {
  const safeRequested = String(requestedId).replace(/[^a-zA-Z0-9_-]/g, "");
  if (safeRequested && safeRequested !== "generator" && safeRequested !== "fx" && !pages[safeRequested]) {
    return safeRequested;
  }
  do {
    pageCounter += 1;
  } while (pages[`generator-${pageCounter}`]);
  return `generator-${pageCounter}`;
}

function defaultGeneratorPageTitle() {
  let number = 2;
  const used = new Set(generatorPageNames().map((name) => pages[name].title.toLocaleLowerCase()));
  while (used.has(`sound generation ${number}`.toLocaleLowerCase())) number += 1;
  return `Sound Generation ${number}`;
}

function createGeneratorPage({ id = "", title = "", toMasterUi = null, activate = false } = {}) {
  const pageName = nextPageId(id);
  const canvasEl = document.createElement("main");
  canvasEl.className = "canvas hidden";
  canvasEl.id = `canvas-${pageName}`;

  const contentEl = document.createElement("div");
  contentEl.className = "canvas-content";
  contentEl.id = `canvasContent-${pageName}`;
  const layerEl = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  layerEl.classList.add("connections-layer");
  layerEl.id = `connectionsLayer-${pageName}`;
  contentEl.appendChild(layerEl);
  canvasEl.appendChild(contentEl);

  const lassoEl = document.createElement("div");
  lassoEl.className = "lasso-box hidden";
  lassoEl.id = `lassoBox-${pageName}`;
  canvasEl.appendChild(lassoEl);
  pages.fx.canvasEl.before(canvasEl);

  const pageTitle = String(title).trim().slice(0, 64) || defaultGeneratorPageTitle();
  const page = makePageState("generator", pageTitle, canvasEl, contentEl, layerEl, lassoEl);
  if (toMasterUi && Number.isFinite(toMasterUi.x) && Number.isFinite(toMasterUi.y)) {
    page.toMasterUi = { x: toMasterUi.x, y: toMasterUi.y };
  }
  pages[pageName] = page;
  generatorPageOrder.push(pageName);

  const tab = document.createElement("button");
  tab.className = "page-tab";
  tab.dataset.page = pageName;
  tab.textContent = pageTitle;
  pageTab("fx").before(tab);
  bindCanvasEvents(page);
  canvasEl.classList.toggle("hide-background-dots", !backgroundDotsEnabled);

  if (activate) switchPage(pageName);
  return pageName;
}

function resetAdditionalGeneratorPages() {
  if (currentPageName !== "generator" && pages[currentPageName]?.kind === "generator") {
    switchPage("generator");
  }
  generatorPageNames().slice(1).forEach((name) => {
    const page = pages[name];
    Array.from(page.canvasContentEl.querySelectorAll(".widget-instance")).forEach((el) => removeWidget(el.id, true));
    page.canvasEl.remove();
    pageTab(name)?.remove();
    delete pages[name];
  });
  generatorPageOrder = ["generator"];
}

function addGeneratorPage() {
  pushHistorySnapshot();
  const name = createGeneratorPage({ activate: true });
  moveToMasterToPage(name);
  drawConnections();
  savePresetNow();
}

function deleteGeneratorPage(name) {
  const page = pages[name];
  if (!page || page.kind !== "generator" || name === "generator") return;
  if (!confirm(`Delete page "${page.title}" and all of its widgets?`)) return;

  pushHistorySnapshot();
  const index = generatorPageOrder.indexOf(name);
  if (currentPageName === name) switchPage(generatorPageOrder[Math.max(0, index - 1)] || "generator");
  if (page.canvasContentEl.contains(sharedToMasterElement())) {
    captureToMasterPosition(name);
    moveToMasterToPage("generator");
  }
  Array.from(page.canvasContentEl.querySelectorAll(".widget-instance")).forEach((el) => removeWidget(el.id, true));
  page.canvasEl.remove();
  pageTab(name)?.remove();
  delete pages[name];
  generatorPageOrder = generatorPageOrder.filter((pageName) => pageName !== name);
  drawConnections();
  savePresetNow();
}

function startPageRename(name) {
  const page = pages[name];
  const tab = pageTab(name);
  if (!page || page.kind !== "generator" || !tab || tab.querySelector("input")) return;
  const currentTitle = page.title;
  const input = document.createElement("input");
  input.type = "text";
  input.maxLength = 64;
  input.className = "page-tab-rename-input";
  input.value = currentTitle;
  tab.textContent = "";
  tab.appendChild(input);
  input.focus();
  input.select();

  let cancelled = false;
  function commit() {
    input.removeEventListener("blur", commit);
    const title = cancelled ? currentTitle : (input.value.trim() || currentTitle);
    if (title !== currentTitle) {
      pushHistorySnapshot();
      page.title = title;
      refreshLinkedIO();
      savePresetNow();
    }
    tab.textContent = page.title;
  }
  input.addEventListener("blur", commit);
  input.addEventListener("click", (event) => event.stopPropagation());
  input.addEventListener("keydown", (event) => {
    event.stopPropagation();
    if (event.key === "Enter") input.blur();
    if (event.key === "Escape") {
      cancelled = true;
      input.blur();
    }
  });
}

function closePageTabContextMenu() {
  pageTabContextMenu.classList.add("hidden");
  contextPageName = null;
}

pageTabList.addEventListener("click", (event) => {
  const tab = event.target.closest(".page-tab");
  if (!tab || event.target.closest("input")) return;
  switchPage(tab.dataset.page);
});

pageTabList.addEventListener("dblclick", (event) => {
  const tab = event.target.closest(".page-tab");
  if (!tab || tab.dataset.page === "fx") return;
  event.preventDefault();
  startPageRename(tab.dataset.page);
});

pageTabList.addEventListener("contextmenu", (event) => {
  const tab = event.target.closest(".page-tab");
  if (!tab) return;
  event.preventDefault();
  closePageTabContextMenu();
  if (tab.dataset.page === "fx") return;
  contextPageName = tab.dataset.page;
  const deleteButton = pageTabContextMenu.querySelector('[data-action="delete"]');
  deleteButton.hidden = contextPageName === "generator";
  pageTabContextMenu.classList.remove("hidden");
  const rect = pageTabContextMenu.getBoundingClientRect();
  pageTabContextMenu.style.left = `${Math.min(event.clientX, window.innerWidth - rect.width - 8)}px`;
  pageTabContextMenu.style.top = `${Math.min(event.clientY, window.innerHeight - rect.height - 8)}px`;
});

pageTabContextMenu.addEventListener("click", (event) => {
  const action = event.target.closest("button")?.dataset.action;
  const name = contextPageName;
  closePageTabContextMenu();
  if (!name) return;
  if (action === "rename") startPageRename(name);
  if (action === "delete") deleteGeneratorPage(name);
});

document.addEventListener("click", (event) => {
  if (!event.target.closest("#pageTabContextMenu")) closePageTabContextMenu();
});

document.getElementById("menuAddPage").addEventListener("click", (event) => {
  event.stopPropagation();
  menuItems.forEach((item) => item.classList.remove("open"));
  addGeneratorPage();
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
    connectionsLayer.querySelectorAll(".connection-path.selected").forEach((path) => {
      path.classList.remove("selected");
    });
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

function uniqueWidgetLabel(requestedLabel) {
  const base = String(requestedLabel || "Widget").trim() || "Widget";
  const used = new Set(
    [...document.querySelectorAll(".widget-instance-label")]
      .map((el) => el.textContent.trim().toLocaleLowerCase())
  );
  if (!used.has(base.toLocaleLowerCase())) return base;

  let suffix = 1;
  while (used.has(`${base}${suffix}`.toLocaleLowerCase())) suffix += 1;
  return `${base}${suffix}`;
}

// Widget types whose "gain" input port is the unambiguous main Volume/Gain control (as opposed
// to a per-channel/per-band gain-ish port like Mixer's "gainN" or ESP-EQ's "gainDb") - see
// createWidget's tagging of .gain-control-row/[data-role="main-gain-control"].
const MAIN_GAIN_WIDGET_TYPES = new Set(["oscillator", "fm-oscillator", "hammond", "soundfont-player", "mda-piano", "wave-folder", "wave-player", "lfo", "wavetable", "comb-filter"]);

// LFO and Wavetable share one implementation (state in lfoStates, tickLfo,
// reset/one-shot/global handling, rate settings dialog); state.wavetable
// tells them apart where they differ.
function isLfoLikeType(type) {
  return type === "lfo" || type === "wavetable";
}

function createWidget(type, label, x, y, forcedId) {
  // The reverb was renamed; presets saved with the old default name follow along.
  if (type === "reverb-light" && label === "Reverb Light") label = "Reverb";
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
  // Restoring a preset keeps its saved labels unchanged. Fresh placement and
  // paste, however, get a unique label immediately (ADSR, ADSR1, ADSR2 …).
  const widgetLabel = forcedId ? label : uniqueWidgetLabel(label || def.label);

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
  labelSpan.textContent = widgetLabel;
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
    if (type === "audio-output-link" || type === "audio-input-link") {
      openAudioLinkSettings(id);
      return;
    }
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

  if (type === "output-switch") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button") || e.target.closest("label")) return;
      e.stopPropagation();
      openOutputSwitchSettings(id);
    });
  }

  if (isLinkedInputType(type)) {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openLinkedInputSettings(id);
    });
  }

  if (isLinkedOutputType(type)) {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openLinkedOutputSettings(id);
    });
  }

  if (type === "audio-output-link" || type === "audio-input-link") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button, input")) return;
      e.stopPropagation();
      openAudioLinkSettings(id);
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

  if (type === "logic-and" || type === "logic-or") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openLogicGateSettings(id);
    });
  }

  if (type === "clock") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openClockSettings(id);
    });
  }

  if (type === "cv-in") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openCvInSettings(id);
    });
  }

  if (type === "cv-out") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openCvOutSettings(id);
    });
  }

  if (type === "digital-in") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openDigitalInSettings(id);
    });
  }

  if (type === "digital-out") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openDigitalOutSettings(id);
    });
  }

  if (type === "rgb-led") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openRgbLedSettings(id);
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

  let outputMultiplexerSwitchBody = null;

  if (type === "output-multiplexer") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button") || e.target.closest("input")) return;
      e.stopPropagation();
      openOutputMultiplexerSettings(id);
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

  if (type === "mda-piano") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button, input")) return;
      e.stopPropagation();
      openMdaPianoSettings(id);
    });
  }

  if (type === "adsr") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button, .adsr-graph")) return;
      e.stopPropagation();
      openAdsrSettings(id);
    });
  }

  if (type === "dx-envelope") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button, input, .dx-envelope-graph")) return;
      e.stopPropagation();
      openDxEnvelopeSettings(id);
    });
  }

  if (type === "filter" || type === "curtis-filter") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openFilterSettings(id);
    });
  }

  if (type === "envelope-follower") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button, input")) return;
      e.stopPropagation();
      openEnvelopeFollowerSettings(id);
    });
  }

  if (type === "detune") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openDetuneSettings(id);
    });
  }

  if (type === "value-mapper") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openValueMapperSettings(id);
    });
  }

  if (type === "delay" || type === "reverb-light") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openEffectCvSettings(id);
    });
  }

  if (type === "graphic-eq") {
    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openGraphicEqSettings(id);
    });
  }

  el.appendChild(header);

  if (type === "annotation") {
    createAnnotationBody(id, el);
  }

  if (type === "midi-in") {
    midiInStates.set(id, { channel: null, voices: 1, overflowMode: "steal-oldest", glide: false, glideTime: 0, pitchBendRangeCents: 200, pitchBendQuantizeCents: false });
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

  if (type === "midi-filter") {
    midiFilterStates.set(id, createMidiFilterState());
    midiVoiceSlotStates.set(id, {
      slots: [{ note: null, heldSince: 0, held: false }], holdCounter: 0,
      pedalDown: false, pendingRelease: false, monoHeldNotes: new Map(), monoCurrentNote: null,
    });
    const body = document.createElement("div");
    body.className = "widget-instance-body midi-filter-body";
    for (const [parameter, title, maximum] of [["channel", "Kanal", 15], ["note", "Note", 127]]) {
      const label = document.createElement("label");
      label.textContent = title;
      const select = document.createElement("select");
      select.dataset.role = `midi-filter-${parameter}`;
      select.add(new Option("Alle", "-1"));
      for (let value = 0; value <= maximum; value += 1) {
        const name = parameter === "channel" ? String(value + 1) :
          `${value} · ${["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"][value % 12]}${Math.floor(value / 12) - 1}`;
        select.add(new Option(name, String(value)));
      }
      select.value = "-1";
      select.addEventListener("change", () => {
        pushHistorySnapshot();
        const state = midiFilterStates.get(id);
        state[parameter] = midiFilterCriterion(select.value, maximum);
        sendLiveParamUpdate(id, { [parameter]: state[parameter] });
      });
      label.appendChild(select);
      body.appendChild(label);
    }
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

  if (type === "logic-and" || type === "logic-or") {
    // Unconnected-input default: AND's neutral element is "true" (an open
    // AND input must not force the whole gate false, mirrors Multiplier's
    // unconnected factor defaulting to 1); OR's neutral element is "false"
    // (an open OR input must not force it true) - see updateLogicGateMode,
    // which re-applies this per-index on every recompute anyway.
    const unconnectedDefault = type === "logic-and" ? CV_MAX_VOLTS : 0;
    logicGateStates.set(id, {
      gateType: type,
      inputCount: 2,
      values: new Array(8).fill(unconnectedDefault),
      audioMode: false,
    });
  }

  if (type === "logic-xor") {
    logicGateStates.set(id, { gateType: type, values: [0, 0], audioMode: false });
  }

  if (type === "logic-not") {
    logicGateStates.set(id, { gateType: type, values: [0], audioMode: false });
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
      pulseMs: 100,
      tickTimer: null,
      offTimer: null,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body clock-body";

    const bpmLabel = document.createElement("div");
    bpmLabel.className = "clock-bpm-label";
    bpmLabel.dataset.role = "clock-bpm-label";
    bpmLabel.textContent = "0 BPM";

    body.appendChild(bpmLabel);
    el.appendChild(body);
  }

  if (type === "cv-in" || type === "cv-out") {
    const stateMap = type === "cv-in" ? cvInStates : cvOutStates;
    stateMap.set(id, { port: -1, address: 0, channel: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body cv-io-body";

    const label = document.createElement("div");
    label.className = "cv-io-label";
    label.dataset.role = "cv-io-label";
    label.textContent = "Not configured";

    body.appendChild(label);
    el.appendChild(body);
  }

  if (type === "rgb-led") {
    rgbLedStates.set(id, { ledIndex: 0, stops: defaultRgbLedStops(), lastRgb: 0 });

    // Lamp + label are moved into the "In" port row once the ports are built (see the
    // port-row loop below), so the input sits level with the LED - same trick as voltmeter.
    const body = document.createElement("div");
    body.className = "widget-instance-body rgb-led-body";

    const group = document.createElement("div");
    group.className = "rgb-led-group";
    group.dataset.role = "rgb-led-group";

    const lamp = document.createElement("div");
    lamp.className = "rgb-led-lamp";
    lamp.dataset.role = "rgb-led-lamp";

    const label = document.createElement("div");
    label.className = "cv-io-label rgb-led-label";
    label.dataset.role = "rgb-led-label";
    label.textContent = "LED 1";

    group.appendChild(lamp);
    group.appendChild(label);
    body.appendChild(group);
    el.appendChild(body);
  }

  if (type === "digital-in" || type === "digital-out") {
    const stateMap = type === "digital-in" ? digitalInStates : digitalOutStates;
    stateMap.set(id, { pin: -1, found: true });

    const body = document.createElement("div");
    body.className = "widget-instance-body cv-io-body";

    const label = document.createElement("div");
    label.className = "cv-io-label";
    label.dataset.role = "gpio-io-label";
    label.textContent = "Not configured";

    body.appendChild(label);
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
      feedback: 0,
    };
    oscillatorStates.set(id, oscState);
    audioEngine.createNode(id, "oscillator", {
      waveType: oscState.waveType,
      frequency: oscState.freq,
      amplitude: 0,
      gain: oscState.gain,
      tuneVolts: oscState.tuneVolts,
      feedback: oscState.feedback,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body oscillator-body";

    const waveButtons = document.createElement("div");
    waveButtons.className = "osc-wave-buttons";
    WAVE_TYPE_BUTTONS.forEach((w) => {
      const btn = document.createElement("button");
      btn.className = "osc-wave-btn";
      btn.innerHTML = w.icon;
      btn.dataset.wave = w.id;
      btn.title = WAVE_TYPE_LABELS[w.id];
      if (w.color) btn.style.setProperty("--wave-color", w.color);
      if (w.glow) btn.style.setProperty("--wave-glow", w.glow);
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
    appendOscilloscopePanel(body);
    el.appendChild(body);
  }

  if (type === "fm-oscillator") {
    // Structurally a clone of the "oscillator" block above (same carrier
    // widget UI - wave buttons + freq label) plus fmIndex, which gets its
    // own slider on the "fmIndex" port row below (see createPortRow). No
    // built-in modulator: patch any node's output into "fmIn" to modulate.
    const center = CV_MAX_VOLTS / 2;
    const fmOscState = {
      pitchCv: center,
      tuneVolts: center,
      freq: hzFromPitchCv(center),
      waveType: "sine",
      amplitude: 1,
      gain: CV_MAX_VOLTS,
      fmIndex: 0,
      feedback: 0,
      dutyCycle: center,
      superSawDetune: center,
      superSawMix: center,
      morph: 0,
      wavetables: null, // created on first use (see fmOscillatorWavetableBank)
    };
    fmOscillatorStates.set(id, fmOscState);
    audioEngine.createNode(id, "fm-oscillator", {
      waveType: fmOscState.waveType,
      frequency: fmOscState.freq,
      amplitude: 0,
      gain: fmOscState.gain,
      tuneVolts: fmOscState.tuneVolts,
      fmIndex: fmOscState.fmIndex,
      feedback: fmOscState.feedback,
      dutyCycle: fmOscState.dutyCycle,
      superSawDetune: fmOscState.superSawDetune,
      superSawMix: fmOscState.superSawMix,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body oscillator-body";

    const waveButtons = document.createElement("div");
    waveButtons.className = "osc-wave-buttons";
    FM_WAVE_TYPE_BUTTONS.forEach((w) => {
      const btn = document.createElement("button");
      btn.className = "osc-wave-btn";
      btn.innerHTML = w.icon;
      btn.dataset.wave = w.id;
      btn.title = WAVE_TYPE_LABELS[w.id];
      if (w.color) btn.style.setProperty("--wave-color", w.color);
      if (w.glow) btn.style.setProperty("--wave-glow", w.glow);
      if (w.disabled) {
        btn.disabled = true;
      } else {
        if (w.id === fmOscState.waveType) btn.classList.add("active");
        btn.addEventListener("click", (e) => {
          e.stopPropagation();
          setFmOscillatorWaveType(id, w.id);
        });
      }
      waveButtons.appendChild(btn);
    });

    const freqLabel = document.createElement("div");
    freqLabel.className = "osc-freq-label";
    freqLabel.dataset.role = "osc-freq-label";
    freqLabel.textContent = `${formatNumber(fmOscState.freq)} Hz`;

    body.appendChild(waveButtons);
    body.appendChild(freqLabel);
    // Oscilloscope and - in Wavetable mode - the static morphed-wave preview side by side.
    const panels = document.createElement("div");
    panels.className = "osc-panels-row";
    appendOscilloscopePanel(panels);
    const wtPreview = document.createElement("canvas");
    wtPreview.className = "osc-wavetable-preview";
    wtPreview.width = 160;
    wtPreview.height = 48;
    wtPreview.dataset.role = "fm-wavetable-preview";
    wtPreview.title = "Wavetable (current morph position) - click to edit";
    wtPreview.hidden = true;
    wtPreview.addEventListener("mousedown", (e) => e.stopPropagation());
    wtPreview.addEventListener("dblclick", (e) => e.stopPropagation());
    wtPreview.addEventListener("click", (e) => {
      e.stopPropagation();
      openFmWavetableEditor(id);
    });
    panels.appendChild(wtPreview);
    body.appendChild(panels);
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
    appendOscilloscopePanel(body);
    el.appendChild(body);

    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openHammondSettings(id);
    });
  }

  if (type === "step-arpeggiator") {
    const stepArpState = {
      steps: new Array(STEP_ARP_MAX_STEPS).fill(STEP_ARP_DEFAULT_VOLTS),
      stepCount: STEP_ARP_MAX_STEPS,
      // "pulseTime" local knob - pulseMs is the resolved value (already in
      // ms, matches nodes/StepArpeggiatorNode.h's own local fallback field
      // 1:1); pulseTimeMinSec/MaxSec are the settings-dialog range that
      // port's 0-10V maps to whenever it IS wired. The slider's own 0-10V
      // position is never stored separately - derived from pulseMs via
      // rangeToCv() (inverse of cvToRange()) wherever it's drawn, same
      // pattern as this file's existing effectCvStates.
      pulseMs: 10,
      pulseTimeMinSec: 0.001,
      pulseTimeMaxSec: 2,
      // Settings-dialog cap (semitones) on a step knob's pitch-CV offset
      // while "pitch" is wired - see nodes/StepArpeggiatorNode.h's
      // StepArpState::maxSemitoneOffset comment. Default 60 = previous,
      // uncapped 5-octave behavior.
      maxSemitoneOffset: 60,
      // Settings-dialog toggle - see nodes/StepArpeggiatorNode.h's
      // StepArpState::retriggerGateOut comment. Mirrored here (see the
      // "clock" branch of deliverToWidget below) purely so the editor's own
      // gateOut LED/propagate() simulation shows the same brief reset gap.
      retriggerGateOut: false,
      // Client-side-simulated sequencing (mirrors nodes/StepArpeggiatorNode.h's
      // play-then-advance model exactly - see stepArpNextIndex below), driven
      // by incoming clock/gate cable values via propagate()/deliverToWidget,
      // purely so the editor can show a clock LED and the currently held
      // step, same idea as Clock's own local pulse timer.
      currentStep: 0,
      pingPongForward: true,
      clockPrev: false,
      gatePrev: false,
      gateVolts: 0,
      pitchVolts: 0,
      directionVolts: 0,
      heldCv: STEP_ARP_DEFAULT_VOLTS,
      gateOutTimer: null,
      // Internal fallback clock (Port-Index 21, "rate") - used only while
      // "clock" itself is unwired, see stepArpRestartInternalClock. Same
      // cvVolts/maxBpm/roundBpm shape as clockStates, minBpm is always 0.
      clockCvVolts: 0,
      clockMaxBpm: 120,
      clockRoundBpm: false,
      clockTickTimer: null,
    };
    stepArpStates.set(id, stepArpState);
    // No audioEngine.createNode() call - like Clock, this widget is a pure
    // CV/gate generator with no browser Web Audio preview counterpart (see
    // audio-engine.js: neither "clock" nor "step-arpeggiator" is handled
    // there).
    // No header/info body - the whole widget-instance still opens settings
    // on dblclick (below), it just doesn't need a dedicated label to do it.

    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button")) return;
      e.stopPropagation();
      openStepArpSettings(id);
    });
  }

  if (type === "drum-sequencer") {
    const cellCount = DRUM_DEFAULT_STEPS;
    drumSequencerStates.set(id, {
      stepCount: DRUM_DEFAULT_STEPS,
      trackCount: 1,
      patterns: [{
        active: new Array(cellCount).fill(false),
        velocities: new Array(cellCount).fill(CV_MAX_VOLTS),
      }],
      activePatternIndex: 0,
      pendingPatternIndex: -1,
      switchAtEnd: false,
      trackPulseMs: new Array(DRUM_MAX_TRACKS).fill(50),
      clockCvVolts: 0,
      clockMaxBpm: 120,
      clockRoundBpm: false,
      currentStep: 0,
      lastStep: -1,
      clockPrev: false,
      gateVolts: 0,
      gatePrev: false,
      heldVelocity: new Array(DRUM_MAX_TRACKS).fill(0),
      triggerTimers: new Array(DRUM_MAX_TRACKS).fill(null),
      clockTickTimer: null,
    });

    const patternMenuBtn = document.createElement("button");
    patternMenuBtn.type = "button";
    patternMenuBtn.className = "drum-sequencer-pattern-menu-btn";
    patternMenuBtn.textContent = "⋮";
    patternMenuBtn.title = "Pattern actions";
    patternMenuBtn.addEventListener("mousedown", (e) => e.stopPropagation());
    patternMenuBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      openDrumPatternMenu(id, patternMenuBtn);
    });
    const closeBtn = header.querySelector(".widget-instance-close");
    header.insertBefore(patternMenuBtn, closeBtn);

    el.addEventListener("dblclick", (e) => {
      if (e.target.closest("button, input, .drum-sequencer-grid, .port-dot")) return;
      e.stopPropagation();
      openDrumSequencerSettings(id);
    });
  }

  if (type === "wave-player") {
    const wpState = { sampleFile: "", rootNoteMidi: 60, gain: CV_MAX_VOLTS };
    wavePlayerStates.set(id, wpState);
    audioEngine.createNode(id, "wave-player", { sampleFile: wpState.sampleFile, rootNoteMidi: wpState.rootNoteMidi, gain: wpState.gain });

    const body = document.createElement("div");
    body.className = "widget-instance-body wave-player-body";

    const sampleLabel = document.createElement("div");
    sampleLabel.className = "wave-player-sample-label";
    sampleLabel.dataset.role = "wave-player-sample-label";
    sampleLabel.textContent = "No sample";

    body.appendChild(sampleLabel);
    appendOscilloscopePanel(body, "left");
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
    appendOscilloscopePanel(body, "left");
    el.appendChild(body);
  }

  if (type === "mda-piano") {
    const state = {
      maxVoices: 32,
      enableModWheel: false,
      enablePitchWheel: false,
      voiceType: 0,
      drive: 0,
      tremWahMode: 0,
      tremWahDepth: 0,
      tremWahRate: 5,
      choPhaMode: 0,
      choPhaDepth: 0,
      choPhaSpeed: 5,
      preGain: CV_MAX_VOLTS,
      gain: CV_MAX_VOLTS,
    };
    mdaPianoStates.set(id, state);
    audioEngine.createNode(id, "mda-piano", state);

    const body = document.createElement("div");
    body.className = "widget-instance-body oscilloscope-only-body";
    appendOscilloscopePanel(body, "left");
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
    volumeRow.className = "audio-out-volume-row gain-control-row";
    volumeRow.dataset.role = "main-gain-control";
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
    gainStates.set(id, { amount: CV_MAX_VOLTS, curve: CV_MAX_VOLTS / 2, global: false });
    audioEngine.createNode(id, "gain", { amount: CV_MAX_VOLTS, curve: CV_MAX_VOLTS / 2 });
  }

  if (type === "comb-filter") {
    combFilterStates.set(id, { ...COMB_FILTER_DEFAULTS });
    audioEngine.createNode(id, type, { ...COMB_FILTER_DEFAULTS });
  }

  if (type === "filter" || type === "curtis-filter") {
    const filterDefaults = {
      filterType: "lowpass",
      cutoff: 5,
      cutoffMinHz: 20,
      cutoffMaxHz: 20000,
      cutoffCurve: "log",
      resonance: 5,
      resonanceMinQ: 0.1,
      resonanceMaxQ: 20,
      resonanceCurve: "log",
      // The CEM3320's signature response is its four-pole ladder, so it starts
      // at 24 dB where the clean biquad filter starts at 12.
      slope: type === "curtis-filter" ? 24 : 12,






      coeffStride: 4,
      global: false,
    };
    filterStates.set(id, { ...filterDefaults });
    audioEngine.createNode(id, type, { ...filterDefaults });

    const body = document.createElement("div");
    body.className = "widget-instance-body filter-body";

    const typeButtons = document.createElement("div");
    typeButtons.className = "filter-type-buttons";
    FILTER_TYPE_BUTTONS.forEach((f) => {
      const btn = document.createElement("button");
      btn.className = "filter-type-btn osc-wave-btn";
      btn.innerHTML = f.icon;
      btn.style.setProperty("--wave-color", f.color);
      btn.style.setProperty("--wave-glow", f.glow);
      btn.dataset.filterType = f.id;
      btn.title = FILTER_TYPE_LABELS[f.id];
      if (f.id === filterDefaults.filterType) btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setFilterType(id, f.id);
      });
      typeButtons.appendChild(btn);
    });

    body.appendChild(typeButtons);

    const slopeButtons = document.createElement("div");
    slopeButtons.className = "filter-slope-buttons";
    FILTER_SLOPE_BUTTONS.forEach((s) => {
      const btn = document.createElement("button");
      btn.className = "filter-type-btn";
      btn.textContent = s.label;
      btn.dataset.filterSlope = String(s.id);
      btn.title = `${s.id} dB pro Oktave`;
      if (s.id === filterDefaults.slope) btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setFilterSlope(id, s.id);
      });
      slopeButtons.appendChild(btn);
    });
    body.appendChild(slopeButtons);

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

  if (type === "output-multiplexer") {
    outputMultiplexerStates.set(id, {
      outputCount: 2,
      manualSelect: 0,
      activeIndex: 0,
      cvVolts: null,
      lastValue: null,
    });
    audioEngine.createNode(id, "output-multiplexer", { select: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body switch-body multiplexer-switch-body";

    const selector = document.createElement("div");
    selector.className = "switch multiplexer-switch";
    selector.setAttribute("role", "img");
    selector.setAttribute("aria-label", "Aktiver Output-Multiplexer-Ausgang");
    const track = document.createElement("span");
    track.className = "switch-track";
    const thumb = document.createElement("span");
    thumb.className = "multiplexer-switch-thumb";
    thumb.dataset.role = "output-multiplexer-switch-thumb";
    selector.append(track, thumb);
    let dragging = false;
    selector.addEventListener("pointerdown", (e) => {
      if (e.button !== 0 || outputMultiplexerHasCvConnection(id)) return;
      e.preventDefault();
      e.stopPropagation();
      dragging = true;
      selector.setPointerCapture?.(e.pointerId);
      setOutputMultiplexerManualIndex(id, e.clientY);
    });
    selector.addEventListener("pointermove", (e) => {
      if (!dragging) return;
      e.preventDefault();
      setOutputMultiplexerManualIndex(id, e.clientY);
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
    outputMultiplexerSwitchBody = body;
  }

  if (isLfoLikeType(type)) {
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
      livePhase: "idle",
      liveLevel: 0,
      liveSegmentStartedAt: 0,
      liveSegmentStartLevel: 0,
      liveGate: false,
      liveVoiceIndex: null,
      graphCollapsed: false,
    };
    adsrStates.set(id, { ...adsrDefaults });
    audioEngine.createNode(id, "adsr", { ...adsrDefaults });

    const collapseBtn = document.createElement("button");
    collapseBtn.type = "button";
    collapseBtn.className = "adsr-collapse-toggle";
    collapseBtn.dataset.role = "adsr-collapse-toggle";
    collapseBtn.title = "Toggle envelope graph";
    collapseBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      const state = adsrStates.get(id);
      if (!state) return;
      state.graphCollapsed = !state.graphCollapsed;
      applyAdsrCollapsed(id);
      savePresetNow();
    });
    const closeBtn = header.querySelector(".widget-instance-close");
    if (closeBtn) header.insertBefore(collapseBtn, closeBtn);
    else header.appendChild(collapseBtn);

    const body = document.createElement("div");
    body.className = "widget-instance-body adsr-body";
    body.appendChild(createAdsrGraph(id));
    el.appendChild(body);
    applyAdsrCollapsed(id);
  }

  if (type === "dx-envelope") {
    const state = createDxEnvelopeDefaults();
    dxEnvelopeStates.set(id, state);
    audioEngine.createNode(id, "dx-envelope", { ...state });

    const collapseBtn = document.createElement("button");
    collapseBtn.type = "button";
    collapseBtn.className = "dx-envelope-collapse-toggle";
    collapseBtn.dataset.role = "dx-envelope-collapse-toggle";
    collapseBtn.title = "Toggle envelope graph";
    collapseBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      state.graphCollapsed = !state.graphCollapsed;
      applyDxEnvelopeCollapsed(id);
      savePresetNow();
    });
    const closeBtn = header.querySelector(".widget-instance-close");
    if (closeBtn) header.insertBefore(collapseBtn, closeBtn);
    else header.appendChild(collapseBtn);

    const body = document.createElement("div");
    body.className = "widget-instance-body dx-envelope-body";
    body.appendChild(createDxEnvelopeGraph(id));
    el.appendChild(body);
    applyDxEnvelopeCollapsed(id);
  }

  if (type === "chorus") {
    const chorusDefaults = { rate: 1.5, depth: 0.003, offset: 0.02, bypass: false };
    chorusStates.set(id, { ...chorusDefaults });
    audioEngine.createNode(id, "chorus", { ...chorusDefaults });
  }

  if (type === "bitcrusher") {
    const state = { crush: 5, bypass: false };
    bitcrusherStates.set(id, state);
    audioEngine.createNode(id, type, { ...state });
  }

  if (type === "wave-folder") {
    const state = { drive: 0, symmetry: CV_MAX_VOLTS / 2, gain: CV_MAX_VOLTS, mix: CV_MAX_VOLTS };
    waveFolderStates.set(id, state);
    audioEngine.createNode(id, type, { ...state });
  }

  if (type === "envelope-follower") {
    const state = { attack: CV_MAX_VOLTS / 2, release: CV_MAX_VOLTS / 2, preGain: CV_MAX_VOLTS / 2, coeffStride: 4 };
    envelopeFollowerStates.set(id, state);
    audioEngine.createNode(id, type, { ...state });
  }

  if (type === "pan") {
    const state = { pan: CV_MAX_VOLTS / 2 };
    panStates.set(id, state);
    audioEngine.createNode(id, type, { ...state });
  }

  if (type === "sample-hold") {
    const state = { threshold: CV_MAX_VOLTS / 2 };
    sampleHoldStates.set(id, state);
    audioEngine.createNode(id, type, { ...state });
  }

  if (type === "burst-generator") {
    const state = { mode: 0, count: CV_MAX_VOLTS / 2, time: CV_MAX_VOLTS / 2, decay: CV_MAX_VOLTS / 2 };
    burstGeneratorStates.set(id, state);
    audioEngine.createNode(id, type, { ...state });
  }

  if (type === "delay" || type === "reverb-light") {
    const state = defaultEffectCvState(type);
    effectCvStates.set(id, state);
    audioEngine.createNode(id, type, effectCvParamsToValues(type, state));
  }

  if (type === "graphic-eq") {
    const cvState = defaultEffectCvState(type);
    effectCvStates.set(id, cvState);
    graphicEqStates.set(id, { frequencies: [...GRAPHIC_EQ_DEFAULT_FREQS], outputGain: CV_MAX_VOLTS });
    const params = { ...effectCvParamsToValues(type, cvState), gain: CV_MAX_VOLTS };
    GRAPHIC_EQ_DEFAULT_FREQS.forEach((hz, i) => { params[`freq${i + 1}`] = hz; });
    audioEngine.createNode(id, type, params);
  }

  if (isLinkedIoType(type)) {
    const body = document.createElement("div");
    body.className = "widget-instance-body linked-io-body";
    if (isLinkedOutputType(type)) {
      const status = document.createElement("div");
      status.className = "linked-io-group-name";
      status.dataset.role = "linked-io-group-name";
      status.textContent = "Not connected";
      body.appendChild(status);
    }
    el.appendChild(body);
  }

  if (type === "audio-output-link" || type === "audio-input-link") {
    const state = type === "audio-input-link"
      ? { gains: Array(LINK_BUS_COUNT).fill(CV_MAX_VOLTS), labels: Array(LINK_BUS_COUNT).fill("") }
      : { labels: Array(LINK_BUS_COUNT).fill("") };
    if (type === "audio-output-link") audioOutputLinkStates.set(id, state);
    else audioInputLinkStates.set(id, state);
    audioEngine.createNode(id, type, state);
  }

  if (isLinkedInputType(type)) {
    const linkCount = linkedIoCountForType(type);
    const state = { linkId: generateLinkId(), linkName: "", linkCount, labels: Array(linkCount).fill(""), portSide: "left" };
    linkedInputStates.set(id, state);
    audioEngine.createNode(id, type, state);
  }

  if (isLinkedOutputType(type)) {
    const linkCount = linkedIoCountForType(type);
    const state = { sourceLinkId: "", linkCount, portSide: "right" };
    linkedOutputStates.set(id, state);
    audioEngine.createNode(id, type, state);
  }

  if (type === "lfo") {
    lfoStates.set(id, {
      waveType: "sine",
      rate: 2,
      maxRate: LFO_MAX_RATE_HZ,
      gain: CV_MAX_VOLTS,
      phase: 0,
      zeroPointFixed: false,
      zeroPoint: 5,
      oneShot: false,
      oneShotDone: true,
      oneShotAccum: 0,
      global: false,
      wavetable: false,
      cyclePos: 0,
      linearPos: 0,
      lastTick: null,
      timer: null,
      resetPrevByVoice: new Map(),
    });
    audioEngine.createNode(id, "lfo", {
      waveType: "sine",
      rate: 2,
      maxRate: LFO_MAX_RATE_HZ,
      gain: CV_MAX_VOLTS,
      phase: 0,
      zeroPointFixed: false,
      zeroPoint: 5,
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
      if (w.color) btn.style.setProperty("--wave-color", w.color);
      if (w.glow) btn.style.setProperty("--wave-glow", w.glow);
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
      btn.className = "filter-type-btn osc-wave-btn";
      btn.innerHTML = m.icon;
      btn.style.setProperty("--wave-color", m.color);
      btn.style.setProperty("--wave-glow", m.glow);
      btn.dataset.zeroMode = m.id;
      btn.title = m.title;
      if (m.id === "auto") btn.classList.add("active");
      btn.addEventListener("click", (e) => {
        e.stopPropagation();
        setLfoZeroPointMode(id, m.id === "fixed");
      });
      zeroModeButtons.appendChild(btn);
    });
    body.appendChild(zeroModeButtons);

    el.appendChild(body);

    startLfoTimer(id);
  }

  if (type === "wavetable") {
    const wavetables = createDefaultBank();
    lfoStates.set(id, {
      waveType: "sine",
      rate: 2,
      maxRate: LFO_MAX_RATE_HZ,
      gain: CV_MAX_VOLTS,
      phase: 0,
      zeroPointFixed: false,
      zeroPoint: 5,
      oneShot: false,
      oneShotDone: true,
      oneShotAccum: 0,
      global: false,
      wavetable: true,
      morph: 0,
      wavetables,
      cyclePos: 0,
      linearPos: 0,
      lastTick: null,
      timer: null,
      resetPrevByVoice: new Map(),
    });
    audioEngine.createNode(id, "wavetable", {
      rate: 2,
      maxRate: LFO_MAX_RATE_HZ,
      gain: CV_MAX_VOLTS,
      oneShot: false,
      global: false,
      morph: 0,
      wavetables,
    });

    const body = document.createElement("div");
    body.className = "widget-instance-body lfo-body";
    const wtPanel = document.createElement("div");
    wtPanel.className = "lfo-wavetable-panel";
    const wtPreview = document.createElement("canvas");
    wtPreview.className = "lfo-wavetable-preview";
    wtPreview.width = 160;
    wtPreview.height = 44;
    wtPreview.dataset.role = "lfo-wavetable-preview";
    wtPreview.title = "Current morphed waveform - click to edit";
    const wtEdit = document.createElement("button");
    wtEdit.className = "btn-secondary lfo-wavetable-edit";
    wtEdit.textContent = "Edit";
    wtEdit.title = "Open the wavetable editor";
    [wtPreview, wtEdit].forEach((node) => {
      node.addEventListener("mousedown", (e) => e.stopPropagation());
      node.addEventListener("dblclick", (e) => e.stopPropagation());
      node.addEventListener("click", (e) => {
        e.stopPropagation();
        openLfoWavetableEditor(id);
      });
    });
    wtPanel.append(wtPreview, wtEdit);
    body.appendChild(wtPanel);
    el.appendChild(body);

    startLfoTimer(id);
    requestAnimationFrame(() => updateLfoWavetablePreview(id));
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
    const state = createOscilloscopeState();
    oscilloscopeStates.set(id, state);

    const body = document.createElement("div");
    body.className = "widget-instance-body oscilloscope-body";

    const display = document.createElement("div");
    display.className = "scope-display";

    const canvas = document.createElement("canvas");
    canvas.className = "scope-canvas";
    canvas.dataset.role = "scope-canvas";
    display.appendChild(canvas);

    const controls = document.createElement("div");
    controls.className = "scope-trigger-controls";
    const triggerLabel = document.createElement("span");
    triggerLabel.className = "scope-trigger-label";
    triggerLabel.textContent = "Trig";
    const source = document.createElement("select");
    source.dataset.role = "scope-trigger-channel";
    source.title = "Trigger channel";
    [["in1", "Ch 1"], ["in2", "Ch 2"]].forEach(([value, label]) => {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = label;
      source.appendChild(option);
    });
    source.addEventListener("mousedown", (event) => event.stopPropagation());
    source.addEventListener("click", (event) => event.stopPropagation());
    source.addEventListener("change", () => setOscilloscopeTriggerChannel(id, source.value));

    const bufferLabel = document.createElement("span");
    bufferLabel.className = "scope-trigger-label";
    bufferLabel.textContent = "Buf";
    const bufferToggle = document.createElement("button");
    bufferToggle.type = "button";
    bufferToggle.className = "scope-buffer-toggle";
    bufferToggle.dataset.role = "scope-buffer-toggle";
    bufferToggle.title = "Toggle visible sample window";
    bufferToggle.addEventListener("mousedown", (event) => event.stopPropagation());
    bufferToggle.addEventListener("click", (event) => {
      event.stopPropagation();
      toggleOscilloscopeTimebase(id);
    });

    const slider = document.createElement("input");
    slider.type = "range";
    slider.className = "scope-trigger-slider";
    slider.dataset.role = "scope-trigger-level";
    slider.min = "-12";
    slider.max = "12";
    slider.step = "0.1";
    slider.title = "Trigger level";
    slider.addEventListener("mousedown", (event) => event.stopPropagation());
    slider.addEventListener("click", (event) => event.stopPropagation());
    slider.addEventListener("input", () => setOscilloscopeTriggerLevel(id, Number(slider.value)));

    const level = document.createElement("span");
    level.className = "scope-trigger-value";
    level.dataset.role = "scope-trigger-value";
    controls.appendChild(triggerLabel);
    controls.appendChild(source);
    controls.appendChild(bufferLabel);
    controls.appendChild(bufferToggle);
    controls.appendChild(slider);
    controls.appendChild(level);
    display.appendChild(controls);

    body.appendChild(display);
    el.appendChild(body);

    const resizeHandle = document.createElement("div");
    resizeHandle.className = "scope-resize-handle";
    resizeHandle.title = "Resize oscilloscope";
    resizeHandle.setAttribute("aria-hidden", "true");
    resizeHandle.addEventListener("mousedown", (event) => startOscilloscopeResize(event, id));
    el.appendChild(resizeHandle);
    applyOscilloscopeState(id);
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
      count: 1,
      layerCount: 1,
      activeLayer: 0,
      layers: [createKnobLayer()],
      outputSide: "bottom",
    });
    audioEngine.createNode(id, "knob", { value: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body knob-body";
    body.dataset.role = "knob-body";
    const controls = document.createElement("div");
    controls.className = "knob-controls";
    controls.dataset.role = "knob-grid";
    body.appendChild(controls);
    const layerSelector = document.createElement("div");
    layerSelector.className = "knob-layer-selector";
    layerSelector.dataset.role = "knob-layer-selector";
    body.appendChild(layerSelector);
    el.appendChild(body);
    renderKnobControls(id, controls);
    renderKnobLayerSelector(id, layerSelector);
  }

  if (type === "button") {
    buttonStates.set(id, {
      count: 1,
      layerCount: 1,
      activeLayer: 0,
      layers: [createButtonLayer()],
      outputSide: "bottom",
    });
    audioEngine.createNode(id, "button", { value: 0 });

    const body = document.createElement("div");
    body.className = "widget-instance-body button-body";
    body.dataset.role = "button-body";
    const controls = document.createElement("div");
    controls.className = "button-controls";
    controls.dataset.role = "button-grid";
    body.appendChild(controls);
    const layerSelector = document.createElement("div");
    layerSelector.className = "knob-layer-selector";
    layerSelector.dataset.role = "button-layer-selector";
    body.appendChild(layerSelector);
    el.appendChild(body);
    renderButtonControls(id, controls);
    renderButtonLayerSelector(id, layerSelector);
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

  let outputSwitchBody = null;

  if (type === "output-switch") {
    outputSwitchStates.set(id, { cc: null, channel: null, selectOut2: false, lastValue: null });
    audioEngine.createNode(id, "output-switch", { selectOut2: false });

    const body = document.createElement("div");
    body.className = "widget-instance-body switch-body";

    const toggleLabel = document.createElement("label");
    toggleLabel.className = "switch";
    const toggleInput = document.createElement("input");
    toggleInput.type = "checkbox";
    toggleInput.dataset.role = "output-switch-toggle";
    toggleInput.addEventListener("mousedown", (e) => e.stopPropagation());
    toggleInput.addEventListener("click", (e) => e.stopPropagation());
    toggleInput.addEventListener("change", () => setOutputSwitchSelectOut2(id, toggleInput.checked));
    const toggleTrack = document.createElement("span");
    toggleTrack.className = "switch-track";
    toggleLabel.appendChild(toggleInput);
    toggleLabel.appendChild(toggleTrack);

    const toggleText = document.createElement("span");
    toggleText.dataset.role = "output-switch-select-label";
    toggleText.textContent = "Out 1";

    const ccLabel = document.createElement("div");
    ccLabel.className = "button-cc-label";
    ccLabel.dataset.role = "output-switch-cc-label";

    body.appendChild(toggleLabel);
    body.appendChild(toggleText);
    body.appendChild(ccLabel);
    outputSwitchBody = body;
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
    detuneStates.set(id, {
      amount: CV_MAX_VOLTS / 2,
      cv: CV_MAX_VOLTS / 2,
      audioMode: false,
      // Settings-dialog range (always stored in cents) + step grid unit -
      // see DetuneState's comment in NodeState.h. "semitone" = 100-cent
      // grid, "cent" = 1-cent grid.
      maxOffsetCents: 1200,
      unit: "semitone",
    });
  }

  const ports = document.createElement("div");
  ports.className = "widget-ports";

  if (type === "knob") {
    renderKnobPorts(id, ports);
  } else if (type === "button") {
    renderButtonPorts(id, ports);
  } else if (type === "multiplier") {
    renderMultiplierPorts(id, ports);
  } else if (type === "adder") {
    renderAdderPorts(id, ports);
  } else if (type === "logic-and" || type === "logic-or") {
    renderLogicGatePorts(id, ports);
  } else if (type === "divide") {
    renderDividePorts(id, ports);
  } else if (type === "mixer") {
    renderMixerPorts(id, ports);
  } else if (type === "multiplexer") {
    renderMultiplexerPorts(id, ports);
  } else if (type === "output-multiplexer") {
    renderOutputMultiplexerPorts(id, ports);
  } else if (type === "hammond") {
    renderHammondPorts(id, ports);
  } else if (type === "step-arpeggiator") {
    renderStepArpPorts(id, ports);
  } else if (type === "drum-sequencer") {
    renderDrumSequencerPorts(id, ports);
  } else if (type === "midi-split") {
    renderMidiSplitPorts(id, ports);
  } else {
    def.inputs.forEach((port) => {
      const row = createPortRow(id, port, "in", type);
      // Main Gain/Volume control - tagged so the end-of-createWidget relocation below moves it
      // to the very last position in the widget, and styled distinctly (see .gain-control-row).
      // Scoped to the unambiguous "volume knob" per widget type, not every gain-ish port
      // (Mixer channel gains, ESP-EQ/ALC dB gains, Audio-Input-Link channel gains stay as-is).
      if ((MAIN_GAIN_WIDGET_TYPES.has(type) && port.id === "gain") ||
          (type === "gain" && port.id === "amount")) {
        row.classList.add("gain-control-row");
        row.dataset.role = "main-gain-control";
      }
      if (isLinkedInputType(type)) {
        const label = row.querySelector(".port-label");
        if (label) {
          const state = linkedInputStates.get(id);
          const index = state.linkCount === 1 ? 0 : Number(port.id.slice(2)) - 1;
          label.classList.add("linked-io-port-label");
          label.textContent = state.labels[index] || String(index + 1);
        }
      }
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
      if (type === "rgb-led" && port.id === "in") {
        const group = el.querySelector('[data-role="rgb-led-group"]');
        if (group) {
          const emptyBody = group.parentElement;
          row.classList.add("rgb-led-port-row");
          row.appendChild(group);
          if (emptyBody?.classList.contains("rgb-led-body")) emptyBody.remove();
        }
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
      if (type === "dx-envelope" && /^(level|time)[1-4]$/.test(port.id)) {
        const state = dxEnvelopeStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(dxEnvelopeSliderVolts(state, port.id));
        slider.dataset.role = `dx-envelope-slider-${port.id}`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, port.id, Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `dx-envelope-value-${port.id}`;
        valueSpan.textContent = formatDxEnvelopeValue(state, port.id);
        row.appendChild(valueSpan);
      }
      if (isLfoLikeType(type) && port.id === "oneShot") {
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
      if ((isLfoLikeType(type) || type === "gain" || type === "filter" || type === "curtis-filter") && port.id === "global") {
        const stateMap = { lfo: lfoStates, wavetable: lfoStates, gain: gainStates, filter: filterStates, "curtis-filter": filterStates }[type];
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
          if (isLfoLikeType(type)) setLfoGlobal(id, toggle.checked);
          else if (type === "gain") setGainGlobal(id, toggle.checked);
          else setFilterGlobal(id, toggle.checked);
        });
        const switchTrack = document.createElement("span");
        switchTrack.className = "switch-track";
        switchLabel.appendChild(toggle);
        switchLabel.appendChild(switchTrack);
        row.insertBefore(switchLabel, row.querySelector(".port-label"));
      }
      if ((type === "filter" || type === "curtis-filter") && (port.id === "filterType" || port.id === "slope")) {
        const filterState = filterStates.get(id);
        const isSlope = port.id === "slope";
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(isSlope ? filterSlopeToCv(filterState.slope) : filterTypeToCv(filterState.filterType));
        slider.dataset.role = isSlope ? "filter-slope-slider" : "filter-type-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, port.id, Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = isSlope ? "filter-slope-value" : "filter-type-value";
        valueSpan.textContent = isSlope ? filterSlopeLabel(filterState.slope) : filterTypeLabel(filterState.filterType);
        row.appendChild(valueSpan);
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
      if (type === "wavetable" && port.id === "morph") {
        // Wavetable position: 0 V = first slot, 10 V = last slot.
        const state = lfoStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.morph);
        slider.dataset.role = "lfo-morph-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, "morph", parseFloat(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "lfo-morph-value";
        valueSpan.textContent = `${formatNumber(state.morph)} V`;
        row.appendChild(valueSpan);
      }
      if (type === "comb-filter" && COMB_FILTER_PORTS.includes(port.id)) {
        const state = combFilterStates.get(id);
        const value = document.createElement("span");
        value.className = "port-value";
        value.dataset.role = `comb-${port.id}-value`;
        value.textContent = formatCombFilterValue(state, port.id);
        if (port.id === "polarity") {
          const switchLabel = document.createElement("label");
          switchLabel.className = "switch";
          const toggle = document.createElement("input");
          toggle.type = "checkbox";
          toggle.checked = state.polarity >= 5;
          toggle.dataset.role = "comb-polarity-switch";
          toggle.setAttribute("aria-label", "Negative feedback polarity");
          toggle.addEventListener("mousedown", (e) => e.stopPropagation());
          toggle.addEventListener("click", (e) => e.stopPropagation());
          toggle.addEventListener("change", () => {
            deliverToWidget(id, "polarity", toggle.checked ? 10 : 0);
            markDirty();
          });
          const track = document.createElement("span");
          track.className = "switch-track";
          switchLabel.append(toggle, track);
          row.insertBefore(switchLabel, row.querySelector(".port-label"));
        } else {
          const slider = document.createElement("input");
          slider.type = "range";
          slider.className = "port-slider";
          slider.min = "0";
          slider.max = "10";
          slider.step = "0.01";
          slider.value = String(state[port.id]);
          slider.dataset.role = `comb-${port.id}-slider`;
          slider.setAttribute("aria-label", port.label);
          slider.addEventListener("mousedown", (e) => e.stopPropagation());
          slider.addEventListener("click", (e) => e.stopPropagation());
          slider.addEventListener("input", () => {
            deliverToWidget(id, port.id, Number(slider.value));
            markDirty();
          });
          row.appendChild(slider);
        }
        row.appendChild(value);
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
      if (type === "wave-player" && port.id === "gain") {
        const state = wavePlayerStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.gain ?? CV_MAX_VOLTS);
        slider.dataset.role = "wave-player-gain-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "gain", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "wave-player-gain-value";
        valueSpan.textContent = formatNumber(state.gain ?? CV_MAX_VOLTS);
        row.appendChild(valueSpan);
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
      if (type === "mda-piano" && ["enableModWheel", "enablePitchWheel"].includes(port.id)) {
        const state = mdaPianoStates.get(id);
        const switchLabel = document.createElement("label");
        switchLabel.className = "switch";
        const toggle = document.createElement("input");
        toggle.type = "checkbox";
        toggle.checked = state[port.id] === true;
        toggle.dataset.role = `mda-piano-${port.id}-switch`;
        toggle.addEventListener("mousedown", (e) => e.stopPropagation());
        toggle.addEventListener("click", (e) => e.stopPropagation());
        toggle.addEventListener("change", () => {
          state[port.id] = toggle.checked;
          deliverToWidget(id, port.id, toggle.checked ? 1 : 0);
        });
        const switchTrack = document.createElement("span");
        switchTrack.className = "switch-track";
        switchLabel.appendChild(toggle);
        switchLabel.appendChild(switchTrack);
        row.insertBefore(switchLabel, row.querySelector(".port-label"));
      }
      if (type === "mda-piano" && !["midi", "enableModWheel", "enablePitchWheel"].includes(port.id)) {
        const state = mdaPianoStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state[port.id]);
        slider.dataset.role = `mda-piano-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `mda-piano-${port.id}-value`;
        valueSpan.textContent = formatNumber(state[port.id]);
        slider.addEventListener("input", () => {
          const value = Number(slider.value);
          state[port.id] = value;
          valueSpan.textContent = formatNumber(value);
          deliverToWidget(id, port.id, value);
        });
        row.appendChild(slider);
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
      if (type === "bitcrusher" && port.id === "crush") {
        const state = bitcrusherStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = "10";
        slider.step = "0.1";
        slider.value = String(state.crush);
        slider.dataset.role = "bitcrusher-crush-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, "crush", Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "bitcrusher-crush-value";
        valueSpan.textContent = formatNumber(state.crush);
        row.appendChild(valueSpan);
      }
      if (type === "sample-hold" && port.id === "threshold") {
        const state = sampleHoldStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.threshold);
        slider.dataset.role = "sample-hold-threshold-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => setSampleHoldThreshold(id, Number(slider.value)));
        slider.addEventListener("change", () => savePresetNow());
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "sample-hold-threshold-value";
        valueSpan.textContent = `${formatNumber(state.threshold)} V`;
        row.appendChild(valueSpan);
      }
      if (type === "wave-folder" && WAVE_FOLDER_SLIDER_PORTS.includes(port.id)) {
        const state = waveFolderStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state[port.id]);
        slider.dataset.role = `wave-folder-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => setWaveFolderParam(id, port.id, Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `wave-folder-${port.id}-value`;
        row.appendChild(valueSpan);
      }
      if (type === "envelope-follower" && ENVELOPE_FOLLOWER_SLIDER_PORTS.includes(port.id)) {
        const state = envelopeFollowerStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state[port.id]);
        slider.dataset.role = `envelope-follower-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => setEnvelopeFollowerParam(id, port.id, Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `envelope-follower-${port.id}-value`;
        row.appendChild(valueSpan);
      }
      if (type === "burst-generator" && port.id === "mode") {
        const state = burstGeneratorStates.get(id);
        const select = document.createElement("select");
        select.className = "port-select";
        [["burst", "Burst"], ["delay", "Trigger Delay"]].forEach(([value, label]) => {
          const option = document.createElement("option");
          option.value = value;
          option.textContent = label;
          select.appendChild(option);
        });
        select.value = state.mode >= CV_MAX_VOLTS / 2 ? "delay" : "burst";
        select.dataset.role = "burst-generator-mode-select";
        select.addEventListener("mousedown", (e) => e.stopPropagation());
        select.addEventListener("click", (e) => e.stopPropagation());
        select.addEventListener("change", () => setBurstGeneratorMode(id, select.value));
        row.appendChild(select);
      }
      if (type === "burst-generator" && BURST_GENERATOR_SLIDER_PORTS.includes(port.id)) {
        const state = burstGeneratorStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state[port.id]);
        slider.dataset.role = `burst-generator-${port.id}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => setBurstGeneratorParam(id, port.id, Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `burst-generator-${port.id}-value`;
        valueSpan.textContent = formatBurstGeneratorValue(port.id, state[port.id], state.mode >= CV_MAX_VOLTS / 2);
        row.appendChild(valueSpan);
      }
      if (type === "pan" && port.id === "pan") {
        const state = panStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.pan);
        slider.dataset.role = "pan-pan-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => setPanParam(id, Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "pan-pan-value";
        valueSpan.textContent = formatPanValue(state.pan);
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
      if (type === "graphic-eq" && port.id === "gain") {
        const state = graphicEqStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(state.outputGain);
        slider.dataset.role = "graphic-eq-gain-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "gain", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "graphic-eq-gain-value";
        valueSpan.textContent = formatNumber(state.outputGain);
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
      if (type === "oscillator" && port.id === "feedback") {
        const oscState = oscillatorStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(oscState.feedback);
        slider.dataset.role = "oscillator-feedback-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "feedback", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "oscillator-feedback-value";
        valueSpan.textContent = formatNumber(oscState.feedback);
        row.appendChild(valueSpan);
      }
      if (type === "fm-oscillator" && port.id === "tune") {
        const fmOscState = fmOscillatorStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(fmOscState.tuneVolts);
        slider.dataset.role = "fm-oscillator-tune-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "tune", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "fm-oscillator-tune-value";
        valueSpan.textContent = formatNumber(fmOscState.tuneVolts);
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
      if (type === "fm-oscillator" && port.id === "gain") {
        const fmOscState = fmOscillatorStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(fmOscState.gain);
        slider.dataset.role = "fm-oscillator-gain-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "gain", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "fm-oscillator-gain-value";
        valueSpan.textContent = formatNumber(fmOscState.gain);
        row.appendChild(valueSpan);
      }
      if (type === "fm-oscillator" && port.id === "fmIndex") {
        const fmOscState = fmOscillatorStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(fmOscState.fmIndex);
        slider.dataset.role = "fm-oscillator-fmindex-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "fmIndex", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "fm-oscillator-fmindex-value";
        valueSpan.textContent = formatNumber(fmOscState.fmIndex);
        row.appendChild(valueSpan);
      }
      if (type === "fm-oscillator" && port.id === "feedback") {
        const fmOscState = fmOscillatorStates.get(id);
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(fmOscState.feedback);
        slider.dataset.role = "fm-oscillator-feedback-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "feedback", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "fm-oscillator-feedback-value";
        valueSpan.textContent = formatNumber(fmOscState.feedback);
        row.appendChild(valueSpan);
      }
      if (type === "fm-oscillator" && port.id === "dutyCycle") {
        const fmOscState = fmOscillatorStates.get(id);
        // Only audible for WaveType::Square (see DspPrimitives.h's
        // PhaseOsc::sampleAt) - row always stays visible (unlike the "row
        // hidden for other waveforms" first attempt), just disabled for
        // every other waveform, toggled live in setFmOscillatorWaveType via
        // updateFmOscillatorDutyCycleVisual(). The "disabled" class (see
        // style.css, same one the mono/stereo mixer row toggle already
        // uses) dims label+slider+value together and is the only thing
        // that visually greys the row out - slider.disabled alone blocks
        // dragging but a custom-styled range input ignores the browser's
        // own disabled look entirely (appearance: none).
        const dutyDisabled = fmOscState.waveType !== "square";
        row.dataset.role = "fm-oscillator-dutycycle-row";
        row.classList.toggle("disabled", dutyDisabled);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(fmOscState.dutyCycle);
        slider.disabled = dutyDisabled;
        slider.dataset.role = "fm-oscillator-dutycycle-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "dutyCycle", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "fm-oscillator-dutycycle-value";
        valueSpan.textContent = formatNumber(fmOscState.dutyCycle);
        row.appendChild(valueSpan);
      }
      if (type === "fm-oscillator" && (port.id === "superSawDetune" || port.id === "superSawMix")) {
        const fmOscState = fmOscillatorStates.get(id);
        const role = port.id === "superSawDetune" ? "supersaw-detune" : "supersaw-mix";
        row.dataset.role = `fm-oscillator-${role}-row`;

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(fmOscState[port.id]);
        slider.dataset.role = `fm-oscillator-${role}-slider`;
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, port.id, parseFloat(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = `fm-oscillator-${role}-value`;
        valueSpan.textContent = formatNumber(fmOscState[port.id]);
        row.appendChild(valueSpan);
      }
      if (type === "fm-oscillator" && port.id === "morph") {
        const fmOscState = fmOscillatorStates.get(id);
        row.dataset.role = "fm-oscillator-morph-row";

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(fmOscState.morph);
        slider.dataset.role = "fm-oscillator-morph-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, "morph", parseFloat(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "fm-oscillator-morph-value";
        valueSpan.textContent = formatNumber(fmOscState.morph);
        row.appendChild(valueSpan);
      }
      if (type === "detune" && port.id === "amount") {
        const detuneState = detuneStates.get(id);

        // Same "0-10V, 5V = 0 offset" knob convention as StepArpeggiator's
        // per-step pitch knobs - see nodes/DetuneNode.h's detuneOffsetVolts().
        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(detuneState.amount);
        slider.dataset.role = "detune-amount-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => {
          deliverToWidget(id, "amount", parseFloat(slider.value));
        });
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "detune-amount-value";
        valueSpan.textContent = detuneOffsetLabel(detuneState);
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
        valueSpan.textContent = `${formatNumber(gainState.amount)} V`;
        row.appendChild(valueSpan);
      }
      if (type === "gain" && port.id === "curve") {
        const gainState = gainStates.get(id);

        const slider = document.createElement("input");
        slider.type = "range";
        slider.className = "port-slider";
        slider.min = "0";
        slider.max = String(CV_MAX_VOLTS);
        slider.step = "0.01";
        slider.value = String(gainState.curve);
        slider.dataset.role = "gain-curve-slider";
        slider.addEventListener("mousedown", (e) => e.stopPropagation());
        slider.addEventListener("click", (e) => e.stopPropagation());
        slider.addEventListener("input", () => deliverToWidget(id, "curve", Number(slider.value)));
        row.appendChild(slider);

        const valueSpan = document.createElement("span");
        valueSpan.className = "port-value";
        valueSpan.dataset.role = "gain-curve-value";
        valueSpan.textContent = `${formatNumber(gainState.curve)} V`;
        row.appendChild(valueSpan);
      }
      if (isLfoLikeType(type) && (port.id === "rate" || port.id === "gain" || port.id === "phase")) {
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
      if (isLinkedOutputType(type)) {
        const label = row.querySelector(".port-label");
        if (label) {
          label.classList.add("linked-io-port-label");
        }
      }
      if (type === "abs" && port.id === "value") {
        row.classList.add("abs-output-row");
        const readout = document.createElement("span");
        readout.className = "port-value";
        readout.dataset.role = "abs-readout";
        readout.textContent = formatNumber(Math.abs(absStates.get(id)?.value ?? 0));
        row.appendChild(readout);
      }
      if ((type === "logic-xor" || type === "logic-not") && port.id === "value") {
        row.classList.add("logic-gate-output-row");
        const readout = document.createElement("span");
        readout.className = "port-value";
        readout.dataset.role = "logic-gate-readout";
        readout.textContent = formatNumber(logicGateValue(logicGateStates.get(id)));
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
  } else if (type === "output-switch") {
    const outputSwitchContent = document.createElement("div");
    outputSwitchContent.className = "switch-content";
    outputSwitchContent.appendChild(ports);
    outputSwitchContent.appendChild(outputSwitchBody);
    const outputSwitchCcLabel = outputSwitchBody.querySelector('[data-role="output-switch-cc-label"]');
    if (outputSwitchCcLabel) outputSwitchContent.appendChild(outputSwitchCcLabel);
    el.appendChild(outputSwitchContent);
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
  } else if (type === "output-multiplexer") {
    const outputMultiplexerContent = document.createElement("div");
    outputMultiplexerContent.className = "switch-content output-multiplexer-content";
    outputMultiplexerContent.appendChild(ports);
    outputMultiplexerContent.appendChild(outputMultiplexerSwitchBody);
    const routing = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    routing.classList.add("multiplexer-routing");
    routing.setAttribute("aria-hidden", "true");
    routing.setAttribute("fill", "none");
    const routingPath = document.createElementNS("http://www.w3.org/2000/svg", "path");
    routingPath.dataset.role = "output-multiplexer-routing-path";
    routingPath.setAttribute("fill", "none");
    routing.appendChild(routingPath);
    outputMultiplexerContent.appendChild(routing);
    el.appendChild(outputMultiplexerContent);
  } else if (type === "knob") {
    ports.classList.add("knob-output-ports");
    el.appendChild(ports);
  } else if (type === "button") {
    ports.classList.add("button-output-ports");
    el.appendChild(ports);
  } else if (isLinkedIoType(type)) {
    el.appendChild(ports);
    renderLinkedIoLayout(id);
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

  if (["filter", "curtis-filter", "comb-filter"].includes(type)) {
    const scopeBody = document.createElement("div");
    scopeBody.className = "widget-instance-body spectrum-body";

    const canvas = document.createElement("canvas");
    canvas.className = "spectrum-canvas";
    canvas.dataset.role = "filter-spectrum-canvas";
    canvas.width = 220;
    canvas.height = 80;

    scopeBody.appendChild(canvas);
    el.appendChild(scopeBody);
  }

  if (type === "graphic-eq") {
    const scopeBody = document.createElement("div");
    scopeBody.className = "widget-instance-body spectrum-body";

    const canvas = document.createElement("canvas");
    canvas.className = "spectrum-canvas";
    canvas.dataset.role = "graphic-eq-spectrum-canvas";
    canvas.width = 220;
    canvas.height = 80;

    scopeBody.appendChild(canvas);
    const bandLabels = document.createElement("div");
    bandLabels.className = "graphic-eq-band-labels";
    bandLabels.dataset.role = "graphic-eq-band-labels";
    GRAPHIC_EQ_DEFAULT_FREQS.forEach(() => {
      const label = document.createElement("span");
      label.className = "graphic-eq-band-label";
      bandLabels.appendChild(label);
    });
    scopeBody.appendChild(bandLabels);
    el.appendChild(scopeBody);
  }

  canvasContent.appendChild(el);
  if (isLinkedInputType(type)) updateLinkedInputVisual(id);
  if (isLinkedOutputType(type)) updateLinkedOutputVisual(id);
  if (type === "audio-output-link") updateAudioOutputLinkVisual(id);
  if (type === "audio-input-link") updateAudioInputLinkVisual(id);
  if (type === "switch") updateSwitchVisual(id);
  if (type === "output-switch") updateOutputSwitchVisual(id);
  if (type === "button") updateButtonVisual(id);
  if (type === "up-down-button") updateUpDownButtonVisual(id);
  if (audioMonitorStates.has(id)) updateAudioMonitorRangeVisual(id);



  if (type !== "annotation" && type !== "knob" && type !== "button" && !isLinkedIoType(type)) layoutPortsGrid(ports);
  if (type === "multiplexer") {
    updateMultiplexerVisual(id);
    requestAnimationFrame(() => updateMultiplexerRouting(id));
  }
  if (type === "output-multiplexer") {
    updateOutputMultiplexerVisual(id);
    requestAnimationFrame(() => updateOutputMultiplexerRouting(id));
  }
  if (type === "midi-in") updateMidiInControls(id);
  if (type === "adsr") updateAdsrSliders(id);
  if (type === "dx-envelope") updateDxEnvelopeVisuals(id);
  if (type === "step-arpeggiator") updateStepArpVisual(id);
  if (type === "drum-sequencer") updateDrumSequencerVisual(id);
  if (type === "sample-hold") updateSampleHoldVisual(id);
  if (type === "wave-folder") updateWaveFolderVisual(id);
  if (type === "envelope-follower") updateEnvelopeFollowerVisual(id);
  if (type === "burst-generator") updateBurstGeneratorVisual(id);
  if (type === "pan") updatePanVisual(id);
  if (type === "comb-filter") updateCombFilterVisual(id);
  // Main Gain/Volume control (see MAIN_GAIN_WIDGET_TYPES/.gain-control-row above) always sits
  // last in the widget, below every other control - regardless of which code path built it
  // (a plain port row from the def.inputs loop, or the bespoke audio-out-family volume row).
  // appendChild() on an already-attached node relocates it rather than cloning it, so this
  // works no matter how deeply nested it currently is inside `el`.
  const mainGainControl = el.querySelector('[data-role="main-gain-control"]');
  if (mainGainControl) el.appendChild(mainGainControl);
  refreshWiredSliderLocks();
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
  outputSwitchStates.delete(id);
  midiInStates.delete(id);
  midiVoiceSlotStates.delete(id);
  midiSplitStates.delete(id);
  midiFilterStates.delete(id);
  multiplierStates.delete(id);
  adderStates.delete(id);
  divideStates.delete(id);
  frequencyDividerStates.delete(id);
  absStates.delete(id);
  valueMapperStates.delete(id);
  logicGateStates.delete(id);
  stopClockTimer(id);
  clockStates.delete(id);
  stopLfoTimer(id);
  lfoStates.delete(id);
  oscillatorStates.delete(id);
  fmOscillatorStates.delete(id);
  hammondStates.delete(id);
  wavePlayerStates.delete(id);
  soundFontPlayerStates.delete(id);
  mdaPianoStates.delete(id);
  detuneStates.delete(id);
  audioOutStates.delete(id);
  audioOutputLinkStates.delete(id);
  audioInputLinkStates.delete(id);
  linkedInputStates.delete(id);
  linkedOutputStates.delete(id);
  gainStates.delete(id);
  filterStates.delete(id);
  combFilterStates.delete(id);
  espEffectStates.delete(id);
  mixerStates.delete(id);
  multiplexerStates.delete(id);
  outputMultiplexerStates.delete(id);
  adsrStates.delete(id);
  dxEnvelopeStates.delete(id);
  constStates.delete(id);
  voltmeterStates.delete(id);
  rgbLedStates.delete(id);
  audioMonitorStates.delete(id);
  oscilloscopeStates.delete(id);
  annotationStates.delete(id);
  annotationSelections.delete(id);
  clearTimeout(annotationSaveTimers.get(id));
  annotationSaveTimers.delete(id);
  chorusStates.delete(id);
  bitcrusherStates.delete(id);
  waveFolderStates.delete(id);
  envelopeFollowerStates.delete(id);
  panStates.delete(id);
  sampleHoldStates.delete(id);
  effectCvStates.delete(id);
  stepArpStopInternalClock(id);
  clearTimeout(stepArpStates.get(id)?.gateOutTimer);
  stepArpStates.delete(id);
  drumSequencerStopInternalClock(id);
  const drumState = drumSequencerStates.get(id);
  if (drumState) drumState.triggerTimers.forEach((timer) => clearTimeout(timer));
  drumSequencerStates.delete(id);
  const removedWasLinkedIO = isLinkedIoType(el?.dataset.widgetType);
  audioEngine.removeNode(id);
  refreshControlGraph();
  refreshVoiceGroups();
  if (removedWasLinkedIO || connections.some(connectionTouchesLinkedIO)) refreshLinkedIO();
  if (midiLearnTargetId?.widgetId === id) midiLearnTargetId = null;
  if (buttonMidiLearnTargetId?.widgetId === id) buttonMidiLearnTargetId = null;
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
  if (pendingLogicGateId === id) pendingLogicGateId = null;
  if (pendingClockId === id) pendingClockId = null;
  if (pendingDrumSequencerId === id) pendingDrumSequencerId = null;
  if (pendingMixerId === id) pendingMixerId = null;
  if (pendingDxEnvelopeId === id) pendingDxEnvelopeId = null;
  if (pendingWavePlayerId === id) pendingWavePlayerId = null;
  if (pendingLinkedInputId === id) pendingLinkedInputId = null;
  if (pendingLinkedOutputId === id) pendingLinkedOutputId = null;
  if (el) el.remove();
}

function clearCanvas() {
  
  
  
  
  const ids = Array.from(canvasContent.querySelectorAll(".widget-instance")).map((el) => el.id);
  ids.forEach((id) => removeWidget(id, true));
  drawConnections();
}

function loadInitTemplate() {
  
  
  
  
  
  resetAdditionalGeneratorPages();
  pages.generator.title = "Sound Generation";
  pageTab("generator").textContent = pages.generator.title;
  pages.generator.toMasterUi = { x: 40, y: 40 };
  switchPage("generator");
  clearCanvas();
  const oscEl = createWidget("fm-oscillator", "Oscillator", 100, 100);
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
  const isPan = portsEl.closest('[data-widget-type="pan"]') !== null;

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
  const outputRowCount = outputRowStart + outRows.length + (isPan ? 1 : 0);
  portsEl.style.minHeight = `${rowHeight * Math.max(inRows.length, outputRowCount)}px`;
  outRows.forEach((row, i) => {
    row.style.position = "absolute";
    // Pan has a slider on its second input row. Keep its second output
    // label below that row instead of painting it over the slider/value.
    const outputIndex = isPan && i === 1 ? 2 : i;
    row.style.top = `${outputTop + outputCenterOffset + outputIndex * rowHeight}px`;
    row.style.right = "0";
  });
}

function createPortRow(widgetId, port, direction, widgetType) {
  const row = document.createElement("div");
  row.className = `port-row port-row-${direction}`;
  row.dataset.portId = port.id;
  row.dataset.portDir = direction;

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
const KNOB_MAX_COUNT = 16;
const KNOB_MAX_LAYERS = 8;

function createKnobControl() {
  return { cc: null, channel: null, raw: 0, point1In: 0, point1Out: 0, point2In: 127, point2Out: CV_MAX_VOLTS, constraintMin: null, constraintMax: null, listText: "" };
}

function createKnobLayer(bindingSource = null) {
  const controls = Array.from({ length: KNOB_MAX_COUNT }, (_, index) => {
    const control = createKnobControl();
    if (bindingSource?.controls[index]) {
      control.cc = bindingSource.controls[index].cc;
      control.channel = bindingSource.controls[index].channel;
    }
    return control;
  });
  return { name: "", controls, labels: Array(KNOB_MAX_COUNT).fill("") };
}

function knobLayer(state, layerIndex = state?.activeLayer ?? 0) {
  return state?.layers?.[layerIndex] || null;
}

function knobControlIndex(knobIndex, layerIndex) {
  return layerIndex * KNOB_MAX_COUNT + knobIndex;
}

function knobPortId(knobIndex, layerIndex = 0) {
  return layerIndex === 0
    ? `voltage${knobIndex + 1}`
    : `voltage${knobIndex + 1}Layer${layerIndex + 1}`;
}

function parseKnobPortId(portId) {
  if (portId === "voltage") return { knobIndex: 0, layerIndex: 0 };
  const match = /^voltage(\d+)(?:Layer(\d+))?$/.exec(portId);
  if (!match) return null;
  const knobIndex = Number(match[1]) - 1;
  const layerIndex = match[2] ? Number(match[2]) - 1 : 0;
  return knobIndex >= 0 && knobIndex < KNOB_MAX_COUNT && layerIndex >= 0 && layerIndex < KNOB_MAX_LAYERS
    ? { knobIndex, layerIndex }
    : null;
}

// audioEngine.setParam paramName for one control's ConstantSourceNode (see
// audio-engine.js's "knob" build case) - "value" (index 0, unsuffixed) stays
// backward compatible with callers that predate multi-knob, "value2".."valueN"
// address the rest.
function knobValueParamName(knobIndex, layerIndex = 0) {
  const index = knobControlIndex(knobIndex, layerIndex);
  return index === 0 ? "value" : `value${index + 1}`;
}

function renderKnobPorts(widgetId, portsEl) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";
  portsEl.className = `widget-ports knob-output-ports knob-output-${state.outputSide}`;
  portsEl.style.setProperty("--knob-count", state.count);
  portsEl.style.setProperty("--knob-layer-count", state.layerCount);
  for (let knobIndex = 0; knobIndex < state.count; knobIndex += 1) {
    const group = document.createElement("div");
    group.className = "knob-output-group";
    for (let layerIndex = 0; layerIndex < state.layerCount; layerIndex += 1) {
      const row = createPortRow(widgetId, {
        id: knobPortId(knobIndex, layerIndex),
        label: `L${layerIndex + 1}`,
      }, "out", "knob");
      row.classList.toggle("knob-output-active", layerIndex === state.activeLayer);
      group.appendChild(row);
    }
    portsEl.appendChild(group);
  }
}

function renderKnobControls(widgetId, gridElement = null) {
  const state = knobStates.get(widgetId);
  const grid = gridElement || document.getElementById(widgetId)?.querySelector('[data-role="knob-grid"]');
  if (!state || !grid) return;
  grid.innerHTML = "";
  const body = grid.closest('[data-role="knob-body"]');
  const columns = state.count;
  const layer = knobLayer(state);
  if (!layer) return;
  grid.style.setProperty("--knob-count", state.count);
  grid.style.setProperty("--knob-columns", columns);
  if (body) {
    body.style.setProperty("--knob-count", state.count);
    body.style.setProperty("--knob-columns", columns);
    body.style.setProperty("--knob-layer-count", state.layerCount);
    body.style.setProperty("--knob-column-min", `${Math.max(76, state.layerCount * 18 + (state.layerCount - 1) * 2)}px`);
    body.classList.toggle("knob-single-layer", state.layerCount === 1);
  }
  for (let index = 0; index < state.count; index += 1) {
    const cell = document.createElement("div");
    cell.className = "knob-control";
    const dial = document.createElement("div");
    dial.className = "knob-dial";
    const indicator = document.createElement("div");
    indicator.className = "knob-indicator";
    indicator.dataset.role = "knob-indicator";
    dial.appendChild(indicator);
    const cap = document.createElement("div");
    cap.className = "knob-center-cap";
    dial.appendChild(cap);
    dial.addEventListener("mousedown", (event) => startKnobDrag(event, widgetId, index));
    // Double-click resets this knob to its center position (raw 63.5, the
    // middle of the 0-127 MIDI-CC range) - widget settings stay reachable
    // via the widget-level dblclick handler elsewhere on the widget body.
    dial.addEventListener("dblclick", (event) => {
      event.preventDefault();
      event.stopPropagation();
      setKnobRaw(widgetId, 63.5, index);
      markDirty();
    });
    const value = document.createElement("div");
    value.className = "knob-readout";
    value.dataset.role = "knob-readout";
    const label = document.createElement("div");
    label.className = "knob-cc-label";
    label.dataset.role = "knob-label";
    indicator.style.transform = `rotate(${knobAngle(layer.controls[index].raw)}deg)`;
    value.textContent = formatNumber(mapKnobValue(layer.controls[index]));
    label.textContent = layer.labels[index] || `Knob ${index + 1}`;
    cell.append(dial, value, label);
    grid.appendChild(cell);
    updateKnobVisual(widgetId, index);
  }
}

function knobLayerIndexFromCv(volts, layerCount) {
  if (layerCount <= 1) return 0;
  return clamp(Math.floor(clamp(Number(volts) || 0, 0, CV_MAX_VOLTS) / (CV_MAX_VOLTS / layerCount)), 0, layerCount - 1);
}

function renderKnobLayerSelector(widgetId, target = null) {
  const state = knobStates.get(widgetId);
  const container = target || document.getElementById(widgetId)?.querySelector('[data-role="knob-layer-selector"]');
  if (!state || !container) return;
  container.innerHTML = "";
  container.classList.toggle("hidden", state.layerCount === 1);
  if (state.layerCount === 1) return;
  const port = createPortRow(widgetId, { id: "layer", label: "Layer" }, "in", "knob");
  const slider = document.createElement("input");
  slider.type = "range";
  slider.className = "port-slider";
  slider.min = "0";
  slider.max = String(state.layerCount - 1);
  slider.step = "1";
  slider.value = String(state.activeLayer);
  slider.disabled = state.layerCount <= 1 || connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "layer");
  slider.setAttribute("aria-label", "Active layer");
  slider.addEventListener("mousedown", (event) => event.stopPropagation());
  slider.addEventListener("click", (event) => event.stopPropagation());
  slider.addEventListener("input", () => setKnobActiveLayer(widgetId, Number(slider.value), true));
  const value = document.createElement("span");
  value.className = "knob-layer-value";
  value.textContent = `${state.activeLayer + 1}/${state.layerCount}`;
  const name = document.createElement("span");
  name.className = "knob-layer-name";
  name.textContent = knobLayer(state)?.name || `Layer ${state.activeLayer + 1}`;
  container.append(name, port, slider, value);
}

function setKnobActiveLayer(widgetId, layerIndex, sendLive = false) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  const next = clamp(Math.round(Number(layerIndex) || 0), 0, state.layerCount - 1);
  if (next === state.activeLayer && !sendLive) return;
  state.activeLayer = next;
  renderKnobControls(widgetId);
  const el = document.getElementById(widgetId);
  const slider = el?.querySelector('[data-role="knob-layer-selector"] input[type="range"]');
  const value = el?.querySelector(".knob-layer-value");
  const name = el?.querySelector(".knob-layer-name");
  if (slider) slider.value = String(next);
  if (value) value.textContent = `${next + 1}/${state.layerCount}`;
  if (name) name.textContent = knobLayer(state)?.name || `Layer ${next + 1}`;
  el?.querySelectorAll(".knob-output-group").forEach((group) => {
    Array.from(group.children).forEach((row, index) => row.classList.toggle("knob-output-active", index === next));
  });
  if (sendLive) {
    sendLiveParamUpdateThrottled(widgetId, { activeLayer: next });
    markDirty();
  }
}

const constStates = new Map();

const voltmeterStates = new Map();
const audioMonitorStates = new Map();
const oscilloscopeStates = new Map();
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

const OSCILLOSCOPE_DEFAULT_WIDTH = 300;
const OSCILLOSCOPE_DEFAULT_HEIGHT = 140;
const OSCILLOSCOPE_MIN_WIDTH = 220;
const OSCILLOSCOPE_MIN_HEIGHT = 90;
const OSCILLOSCOPE_MAX_WIDTH = 1100;
const OSCILLOSCOPE_MAX_HEIGHT = 700;

function createOscilloscopeState() {
  return {
    width: OSCILLOSCOPE_DEFAULT_WIDTH,
    height: OSCILLOSCOPE_DEFAULT_HEIGHT,
    triggerChannel: "in1",
    triggerLevel: 0,
    timebase: "short",
  };
}

function applyOscilloscopeState(widgetId) {
  const state = oscilloscopeStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  state.width = clamp(Math.round(Number(state.width) || OSCILLOSCOPE_DEFAULT_WIDTH), OSCILLOSCOPE_MIN_WIDTH, OSCILLOSCOPE_MAX_WIDTH);
  state.height = clamp(Math.round(Number(state.height) || OSCILLOSCOPE_DEFAULT_HEIGHT), OSCILLOSCOPE_MIN_HEIGHT, OSCILLOSCOPE_MAX_HEIGHT);
  state.triggerChannel = state.triggerChannel === "in2" ? "in2" : "in1";
  state.triggerLevel = clamp(Number(state.triggerLevel) || 0, -12, 12);
  state.timebase = state.timebase === "long" ? "long" : "short";
  const canvas = el.querySelector('[data-role="scope-canvas"]');
  if (canvas) {
    canvas.width = state.width;
    canvas.height = state.height;
  }
  const source = el.querySelector('[data-role="scope-trigger-channel"]');
  if (source) source.value = state.triggerChannel;
  const bufferToggle = el.querySelector('[data-role="scope-buffer-toggle"]');
  if (bufferToggle) {
    bufferToggle.textContent = state.timebase === "long" ? "2k" : "1k";
    bufferToggle.setAttribute("aria-label", `Visible samples: ${bufferToggle.textContent}`);
  }
  const slider = el.querySelector('[data-role="scope-trigger-level"]');
  if (slider) slider.value = String(state.triggerLevel);
  const value = el.querySelector('[data-role="scope-trigger-value"]');
  if (value) value.textContent = `${formatNumber(state.triggerLevel)} V`;
}

function setOscilloscopeTriggerChannel(widgetId, channel) {
  const state = oscilloscopeStates.get(widgetId);
  if (!state) return;
  state.triggerChannel = channel === "in2" ? "in2" : "in1";
  applyOscilloscopeState(widgetId);
  markDirty();
}

function setOscilloscopeTriggerLevel(widgetId, value) {
  const state = oscilloscopeStates.get(widgetId);
  if (!state) return;
  state.triggerLevel = clamp(Number(value) || 0, -12, 12);
  applyOscilloscopeState(widgetId);
  markDirty();
}

function toggleOscilloscopeTimebase(widgetId) {
  const state = oscilloscopeStates.get(widgetId);
  if (!state) return;
  state.timebase = state.timebase === "long" ? "short" : "long";
  applyOscilloscopeState(widgetId);
  markDirty();
}

function startOscilloscopeResize(event, widgetId) {
  event.preventDefault();
  event.stopPropagation();
  const state = oscilloscopeStates.get(widgetId);
  if (!state) return;
  selectOnlyWidget(widgetId);
  const startX = event.clientX;
  const startY = event.clientY;
  const startWidth = state.width;
  const startHeight = state.height;
  const zoomFactor = Math.max(zoom, ZOOM_MIN);
  let moved = false;

  const onMove = (moveEvent) => {
    if (!moved) {
      pushHistorySnapshot();
      moved = true;
    }
    state.width = clamp(Math.round(startWidth + (moveEvent.clientX - startX) / zoomFactor), OSCILLOSCOPE_MIN_WIDTH, OSCILLOSCOPE_MAX_WIDTH);
    state.height = clamp(Math.round(startHeight + (moveEvent.clientY - startY) / zoomFactor), OSCILLOSCOPE_MIN_HEIGHT, OSCILLOSCOPE_MAX_HEIGHT);
    applyOscilloscopeState(widgetId);
    drawConnections();
  };
  const onUp = () => {
    document.removeEventListener("mousemove", onMove);
    document.removeEventListener("mouseup", onUp);
    if (!moved) return;
    markDirty();
    drawConnections();
  };
  document.addEventListener("mousemove", onMove);
  document.addEventListener("mouseup", onUp);
}

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

function mapKnobValue(control) {
  const list = parseValueList(control.listText);
  if (list && list.length >= 2) {
    const idx = clamp(Math.floor((control.raw * list.length) / 128), 0, list.length - 1);
    return list[idx];
  }

  // Linear from A Out (raw 0, knob fully left) to B Out (raw 127) - same as KnobNode.h.
  return control.point1Out + (control.raw / 127) * (control.point2Out - control.point1Out);
}

function updateKnobVisual(widgetId, index = 0) {
  const state = knobStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const layer = knobLayer(state);
  const control = layer?.controls[index];
  const cell = el.querySelectorAll(".knob-control")[index];
  if (!control || !cell) return;

  const indicator = cell.querySelector('[data-role="knob-indicator"]');
  if (indicator) indicator.style.transform = `rotate(${knobAngle(control.raw)}deg)`;

  const readout = cell.querySelector('[data-role="knob-readout"]');
  if (readout) readout.textContent = formatNumber(mapKnobValue(control));

  const label = cell.querySelector('[data-role="knob-label"]');
  if (label) label.textContent = layer.labels[index] || `Knob ${index + 1}`;
}

function setKnobRaw(widgetId, raw, index = 0) {
  const state = knobStates.get(widgetId);
  const layerIndex = state?.activeLayer ?? 0;
  const control = knobLayer(state, layerIndex)?.controls[index];
  if (!control) return;
  control.raw = clamp(Math.round(raw), 0, 127);
  const mapped = roundFloat(mapKnobValue(control));
  audioEngine.setParam(widgetId, knobValueParamName(index, layerIndex), mapped);
  updateKnobVisual(widgetId, index);
  propagate(widgetId, knobPortId(index, layerIndex), mapped);
  if (index === 0 && layerIndex === 0) propagate(widgetId, "voltage", mapped);
  sendLiveParamUpdateThrottled(widgetId, {
    knobIndex: knobControlIndex(index, layerIndex) + 1,
    raw: control.raw,
  });
}

function startKnobDrag(e, widgetId, index = 0) {
  e.preventDefault();
  e.stopPropagation();
  const state = knobStates.get(widgetId);
  const control = knobLayer(state)?.controls[index];
  if (!control) return;

  const startY = e.clientY;
  const startRaw = control.raw;
  let moved = false;

  function onMove(ev) {
    const dy = startY - ev.clientY;
    if (Math.abs(dy) > 2) moved = true;
    setKnobRaw(widgetId, startRaw + dy / 2, index);
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
    const target = midiLearnTargetId;
    const state = knobStates.get(target.widgetId);
    const control = knobLayer(state, 0)?.controls[target.index];
    if (control) {
      state.layers.forEach((layer) => {
        layer.controls[target.index].cc = data.control;
        layer.controls[target.index].channel = data.channel;
      });
      midiLearnTargetId = null;
      updateKnobSettingsCcDisplay(target.widgetId, target.index);
      updateKnobVisual(target.widgetId, target.index);
      sendLiveParamUpdate(target.widgetId, {
        knobIndex: target.index + 1,
        cc: data.control,
        channel: data.channel,
      });
      savePresetNow();
    }
    midiLearnTargetId = null;
    return;
  }

  // Following an incoming CC only updates the display - the device already applied it.
  withDeviceSyncMuted(() => knobStates.forEach((state, widgetId) => {
    const bindings = knobLayer(state, 0);
    bindings.controls.slice(0, state.count).forEach((binding, index) => {
      if (binding.cc === data.control && binding.channel === data.channel) {
        setKnobRaw(widgetId, data.value, index);
        markDirty();
      }
    });
  }));
}

const knobSettingsOverlay = document.getElementById("knobSettingsOverlay");
const knobSettingsAdd = document.getElementById("knobSettingsAdd");
const knobSettingsAddLayer = document.getElementById("knobSettingsAddLayer");
const knobSettingsLayerTabs = document.getElementById("knobSettingsLayerTabs");
const knobLayerNameInput = document.getElementById("knobLayerNameInput");
const knobCcContextMenu = document.getElementById("knobCcContextMenu");
const knobLayerContextMenu = document.getElementById("knobLayerContextMenu");
const knobOutputSideInput = document.getElementById("knobOutputSideInput");
const knobSettingsGrid = document.getElementById("knobSettingsGrid");
let pendingKnobId = null;
let pendingKnobIndex = 0;
let pendingKnobLayerIndex = 0;
let knobCcContextTarget = null;
let knobLayerContextTarget = null;

function knobCcDisplayText(widgetId, index) {
  if (midiLearnTargetId?.widgetId === widgetId && midiLearnTargetId.index === index) return "Waiting…";
  const binding = knobLayer(knobStates.get(widgetId), 0)?.controls[index];
  return binding?.cc === null || binding?.cc === undefined
    ? "Learn"
    : `${binding.cc}${binding.channel === null ? "" : ` (Ch ${binding.channel + 1})`}`;
}

function updateKnobSettingsCcDisplay(widgetId, index) {
  const display = knobSettingsGrid.querySelector(`[data-knob-index="${index}"] [data-knob-field="cc"]`);
  if (!display || pendingKnobId !== widgetId) return;
  const waiting = midiLearnTargetId?.widgetId === widgetId && midiLearnTargetId.index === index;
  display.textContent = knobCcDisplayText(widgetId, index);
  display.classList.toggle("waiting", waiting);
}

function closeKnobCcContextMenu() {
  knobCcContextMenu.classList.add("hidden");
  knobCcContextTarget = null;
}

function startKnobMidiLearn(widgetId, index) {
  const previous = midiLearnTargetId;
  midiLearnTargetId = { widgetId, index };
  closeKnobCcContextMenu();
  if (previous) updateKnobSettingsCcDisplay(previous.widgetId, previous.index);
  updateKnobSettingsCcDisplay(widgetId, index);
}

function cancelKnobMidiLearn() {
  if (!midiLearnTargetId) return false;
  const target = midiLearnTargetId;
  midiLearnTargetId = null;
  updateKnobSettingsCcDisplay(target.widgetId, target.index);
  return true;
}

function openKnobCcContextMenu(event, widgetId, index) {
  event.preventDefault();
  event.stopPropagation();
  knobCcContextTarget = { widgetId, index };
  knobCcContextMenu.classList.remove("hidden");
  const rect = knobCcContextMenu.getBoundingClientRect();
  knobCcContextMenu.style.left = `${Math.min(event.clientX, window.innerWidth - rect.width - 8)}px`;
  knobCcContextMenu.style.top = `${Math.min(event.clientY, window.innerHeight - rect.height - 8)}px`;
}

function clearKnobMidiLearn(widgetId, index) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  if (midiLearnTargetId?.widgetId === widgetId && midiLearnTargetId.index === index) midiLearnTargetId = null;
  state.layers.forEach((layer) => {
    layer.controls[index].cc = null;
    layer.controls[index].channel = null;
  });
  updateKnobSettingsCcDisplay(widgetId, index);
  sendLiveParamUpdate(widgetId, { knobIndex: index + 1, cc: null, channel: null });
  savePresetNow();
}

function closeKnobLayerContextMenu() {
  knobLayerContextMenu.classList.add("hidden");
  knobLayerContextTarget = null;
}

function openKnobLayerContextMenu(event, widgetId, layerIndex) {
  event.preventDefault();
  event.stopPropagation();
  closeKnobCcContextMenu();
  knobLayerContextTarget = { widgetId, layerIndex };
  knobLayerContextMenu.querySelector('[data-action="delete"]').disabled = knobStates.get(widgetId)?.layerCount <= 1;
  knobLayerContextMenu.classList.remove("hidden");
  const rect = knobLayerContextMenu.getBoundingClientRect();
  knobLayerContextMenu.style.left = `${Math.min(event.clientX, window.innerWidth - rect.width - 8)}px`;
  knobLayerContextMenu.style.top = `${Math.min(event.clientY, window.innerHeight - rect.height - 8)}px`;
}

function createKnobSettingsInput(field, value, { readOnly = false, placeholder = "" } = {}) {
  const input = document.createElement("input");
  input.type = "text";
  input.inputMode = "decimal";
  input.dataset.knobField = field;
  input.value = value;
  input.readOnly = readOnly;
  input.placeholder = placeholder;
  input.setAttribute("aria-label", field);
  return input;
}

function updateKnobCardDisabledState(card) {
  const listInput = card.querySelector('[data-knob-field="listText"]');
  const listValid = !!(listInput && parseValueList(listInput.value)?.length >= 2);
  card.querySelectorAll('[data-knob-linear]').forEach((input) => {
    input.disabled = listValid;
  });
}

function renderKnobSettingsLayerTabs(widgetId) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  knobSettingsLayerTabs.innerHTML = "";
  for (let layerIndex = 0; layerIndex < state.layerCount; layerIndex += 1) {
    const tab = document.createElement("button");
    tab.type = "button";
    tab.className = "btn-secondary knob-settings-layer-tab";
    tab.textContent = state.layers[layerIndex].name || `Layer ${layerIndex + 1}`;
    tab.dataset.layerIndex = String(layerIndex);
    tab.classList.toggle("active", layerIndex === pendingKnobLayerIndex);
    tab.setAttribute("role", "tab");
    tab.setAttribute("aria-selected", String(layerIndex === pendingKnobLayerIndex));
    tab.addEventListener("click", () => {
      syncKnobSettingsGrid(widgetId, pendingKnobLayerIndex);
      pendingKnobLayerIndex = layerIndex;
      renderKnobSettingsGrid(widgetId);
    });
    tab.addEventListener("contextmenu", (event) => openKnobLayerContextMenu(event, widgetId, layerIndex));
    knobSettingsLayerTabs.appendChild(tab);
  }
  knobSettingsAddLayer.disabled = state.layerCount >= KNOB_MAX_LAYERS;
  knobLayerNameInput.value = knobLayer(state, pendingKnobLayerIndex)?.name || "";
}

function renderKnobSettingsGrid(widgetId) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  pendingKnobLayerIndex = clamp(pendingKnobLayerIndex, 0, state.layerCount - 1);
  const layer = knobLayer(state, pendingKnobLayerIndex);
  renderKnobSettingsLayerTabs(widgetId);
  knobSettingsGrid.innerHTML = "";
  for (let index = 0; index < state.count; index += 1) {
    const control = layer.controls[index];
    const card = document.createElement("section");
    card.className = "knob-settings-card";
    card.dataset.knobIndex = String(index);

    const number = document.createElement("strong");
    number.className = "knob-settings-number";
    number.textContent = String(index + 1);
    card.appendChild(number);

    const cc = document.createElement("div");
    cc.className = "knob-settings-cc";
    const ccValue = document.createElement("button");
    ccValue.type = "button";
    ccValue.className = "knob-settings-cc-value";
    ccValue.dataset.knobField = "cc";
    ccValue.textContent = knobCcDisplayText(widgetId, index);
    ccValue.classList.toggle("waiting", midiLearnTargetId?.widgetId === widgetId && midiLearnTargetId.index === index);
    if (pendingKnobLayerIndex === 0) {
      ccValue.addEventListener("click", () => {
        if (midiLearnTargetId?.widgetId === widgetId && midiLearnTargetId.index === index) cancelKnobMidiLearn();
        else startKnobMidiLearn(widgetId, index);
      });
    } else {
      ccValue.classList.add("readonly");
      ccValue.title = "MIDI Learn is available on Layer 1";
    }
    ccValue.addEventListener("contextmenu", (event) => openKnobCcContextMenu(event, widgetId, index));
    cc.appendChild(ccValue);
    card.appendChild(cc);
    card.appendChild(createKnobSettingsInput("label", layer.labels[index] ?? ""));

    card.appendChild(createKnobSettingsInput("listText", control.listText ?? ""));
    [
      ["point1Out", control.point1Out],
      ["point2Out", control.point2Out],
    ].forEach(([field, value]) => {
      const input = createKnobSettingsInput(field, value);
      input.dataset.knobLinear = "true";
      card.appendChild(input);
    });
    const listInput = card.querySelector('[data-knob-field="listText"]');
    listInput.addEventListener("input", () => updateKnobCardDisabledState(card));
    updateKnobCardDisabledState(card);
    const actions = document.createElement("div");
    actions.className = "knob-settings-actions";
    [["up", "↑", "Move knob left"], ["down", "↓", "Move knob right"], ["delete", "✕", "Delete knob"]].forEach(([action, text, title]) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "btn-secondary";
      button.dataset.knobAction = action;
      button.textContent = text;
      button.title = title;
      button.setAttribute("aria-label", title);
      button.disabled = (action === "up" && index === 0) || (action === "down" && index === state.count - 1) || (action === "delete" && state.count === 1);
      button.addEventListener("click", () => editKnobSettingsStructure(widgetId, index, action));
      actions.appendChild(button);
    });
    card.appendChild(actions);
    knobSettingsGrid.appendChild(card);
  }
}

function syncKnobSettingsGrid(widgetId, layerIndex = pendingKnobLayerIndex) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  const layer = knobLayer(state, layerIndex);
  if (!layer) return;
  layer.name = knobLayerNameInput.value.trim().slice(0, 32);
  const controls = [];
  const labels = [];
  knobSettingsGrid.querySelectorAll("[data-knob-index]").forEach((card) => {
    const control = layer.controls[Number(card.dataset.knobIndex)];
    const field = (name) => card.querySelector(`[data-knob-field="${name}"]`).value;
    const numberField = (name, fallback) => {
      const value = parseFloatFlexible(field(name));
      return Number.isNaN(value) ? fallback : value;
    };
    control.listText = field("listText");
    control.point1In = 0;
    control.point1Out = numberField("point1Out", 0);
    control.point2In = 127;
    control.point2Out = numberField("point2Out", CV_MAX_VOLTS);
    control.constraintMin = null;
    control.constraintMax = null;
    controls.push(control);
    labels.push(field("label").trim());
  });
  state.count = controls.length;
  layer.controls = [...controls, ...Array.from({ length: KNOB_MAX_COUNT - controls.length }, createKnobControl)];
  layer.labels = [...labels, ...Array(KNOB_MAX_COUNT - labels.length).fill("")];
}

function editKnobSettingsStructure(widgetId, index, action) {
  const state = knobStates.get(widgetId);
  if (!state) return;
  syncKnobSettingsGrid(widgetId, pendingKnobLayerIndex);
  if (action === "add" && state.count < KNOB_MAX_COUNT) {
    state.layers.forEach((layer, layerIndex) => {
      layer.controls[state.count] = createKnobControl();
      if (layerIndex > 0) {
        layer.controls[state.count].cc = state.layers[0].controls[state.count].cc;
        layer.controls[state.count].channel = state.layers[0].controls[state.count].channel;
      }
      layer.labels[state.count] = "";
    });
    state.count += 1;
  } else if (action === "delete" && state.count > 1) {
    state.layers.forEach((layer) => {
      layer.controls.splice(index, 1);
      layer.controls.push(createKnobControl());
      layer.labels.splice(index, 1);
      layer.labels.push("");
    });
    state.count -= 1;
  } else if (action === "up" && index > 0) {
    state.layers.forEach((layer) => {
      [layer.controls[index - 1], layer.controls[index]] = [layer.controls[index], layer.controls[index - 1]];
      [layer.labels[index - 1], layer.labels[index]] = [layer.labels[index], layer.labels[index - 1]];
    });
  } else if (action === "down" && index < state.count - 1) {
    state.layers.forEach((layer) => {
      [layer.controls[index], layer.controls[index + 1]] = [layer.controls[index + 1], layer.controls[index]];
      [layer.labels[index], layer.labels[index + 1]] = [layer.labels[index + 1], layer.labels[index]];
    });
  }
  renderKnobSettingsGrid(widgetId);
}

function addKnobLayer(widgetId) {
  const state = knobStates.get(widgetId);
  if (!state || state.layerCount >= KNOB_MAX_LAYERS) return;
  syncKnobSettingsGrid(widgetId, pendingKnobLayerIndex);
  state.layers.push(createKnobLayer(state.layers[0]));
  state.layerCount += 1;
  pendingKnobLayerIndex = state.layerCount - 1;
  renderKnobSettingsGrid(widgetId);
}

function deleteKnobLayer(widgetId, removedLayer = pendingKnobLayerIndex) {
  const state = knobStates.get(widgetId);
  if (!state || state.layerCount <= 1) return;
  const currentLayer = pendingKnobLayerIndex;
  syncKnobSettingsGrid(widgetId, pendingKnobLayerIndex);
  removedLayer = clamp(removedLayer, 0, state.layerCount - 1);
  for (let knobIndex = 0; knobIndex < state.count; knobIndex += 1) {
    removeConnectionsForPort(widgetId, knobPortId(knobIndex, removedLayer), "out");
  }
  const shiftedConnections = connections.filter((connection) => {
    if (connection.from.widgetId !== widgetId) return false;
    const parsed = parseKnobPortId(connection.from.portId);
    return parsed && parsed.layerIndex > removedLayer;
  });
  shiftedConnections.forEach(disconnectIfAudio);
  shiftedConnections.forEach((connection) => {
    const parsed = parseKnobPortId(connection.from.portId);
    connection.from.portId = knobPortId(parsed.knobIndex, parsed.layerIndex - 1);
    connectIfAudio(connection);
  });
  state.layers.splice(removedLayer, 1);
  state.layerCount -= 1;
  if (state.layerCount === 1) removeConnectionsForPort(widgetId, "layer", "in");
  if (state.activeLayer === removedLayer) state.activeLayer = Math.min(removedLayer, state.layerCount - 1);
  else if (state.activeLayer > removedLayer) state.activeLayer -= 1;
  pendingKnobLayerIndex = currentLayer === removedLayer
    ? Math.min(removedLayer, state.layerCount - 1)
    : currentLayer - (currentLayer > removedLayer ? 1 : 0);
  renderKnobSettingsGrid(widgetId);
}

function openKnobSettings(widgetId, index = 0) {
  const state = knobStates.get(widgetId);
  if (!knobLayer(state)?.controls[index]) return;
  pendingKnobId = widgetId;
  pendingKnobIndex = index;
  pendingKnobLayerIndex = state.activeLayer;
  knobOutputSideInput.value = state.outputSide;
  renderKnobSettingsGrid(widgetId);
  midiLearnTargetId = null;
  knobSettingsOverlay.classList.remove("hidden");
}

function closeKnobSettings() {
  knobSettingsOverlay.classList.add("hidden");
  closeKnobCcContextMenu();
  closeKnobLayerContextMenu();
  pendingKnobId = null;
  pendingKnobIndex = 0;
  pendingKnobLayerIndex = 0;
  midiLearnTargetId = null;
}


document.getElementById("knobSettingsCancel").addEventListener("click", closeKnobSettings);

knobSettingsAdd.addEventListener("click", () => {
  if (pendingKnobId) editKnobSettingsStructure(pendingKnobId, 0, "add");
});

knobSettingsAddLayer.addEventListener("click", () => {
  if (pendingKnobId) addKnobLayer(pendingKnobId);
});

knobLayerNameInput.addEventListener("input", () => {
  const state = knobStates.get(pendingKnobId);
  const layer = knobLayer(state, pendingKnobLayerIndex);
  if (!layer) return;
  layer.name = knobLayerNameInput.value.trimStart().slice(0, 32);
  const tab = knobSettingsLayerTabs.querySelector(`[data-layer-index="${pendingKnobLayerIndex}"]`);
  if (tab) tab.textContent = layer.name || `Layer ${pendingKnobLayerIndex + 1}`;
});

knobCcContextMenu.addEventListener("click", (event) => {
  const action = event.target.closest("button")?.dataset.action;
  const target = knobCcContextTarget;
  closeKnobCcContextMenu();
  if (action === "unlearn" && target) clearKnobMidiLearn(target.widgetId, target.index);
});

knobLayerContextMenu.addEventListener("click", (event) => {
  const action = event.target.closest("button")?.dataset.action;
  const target = knobLayerContextTarget;
  closeKnobLayerContextMenu();
  if (action === "delete" && target) deleteKnobLayer(target.widgetId, target.layerIndex);
});

document.addEventListener("mousedown", (event) => {
  if (!event.target.closest("#knobCcContextMenu")) closeKnobCcContextMenu();
  if (!event.target.closest("#knobLayerContextMenu")) closeKnobLayerContextMenu();
});

document.getElementById("knobSettingsSave").addEventListener("click", () => {
  if (!pendingKnobId) return;
  const state = knobStates.get(pendingKnobId);
  if (state) {
    syncKnobSettingsGrid(pendingKnobId, pendingKnobLayerIndex);
    state.outputSide = ["top", "bottom"].includes(knobOutputSideInput.value) ? knobOutputSideInput.value : "bottom";
    state.layers.forEach((layer, layerIndex) => {
      layer.controls.slice(0, state.count).forEach((control, knobIndex) => {
        const binding = state.layers[0].controls[knobIndex];
        control.cc = binding.cc;
        control.channel = binding.channel;
        const mapped = roundFloat(mapKnobValue(control));
        audioEngine.setParam(pendingKnobId, knobValueParamName(knobIndex, layerIndex), mapped);
        if (layerIndex === state.activeLayer) {
          propagate(pendingKnobId, knobPortId(knobIndex, layerIndex), mapped);
          if (knobIndex === 0 && layerIndex === 0) propagate(pendingKnobId, "voltage", mapped);
        }
        sendLiveParamUpdate(pendingKnobId, {
          knobIndex: knobControlIndex(knobIndex, layerIndex) + 1,
          knobCount: state.count,
          layerCount: state.layerCount,
          activeLayer: state.activeLayer,
          cc: control.cc,
          channel: control.channel,
          raw: control.raw,
          point1In: control.point1In,
          point1Out: control.point1Out,
          point2In: control.point2In,
          point2Out: control.point2Out,
          constraintMin: control.constraintMin,
          constraintMax: control.constraintMax,
          valueList: parseValueList(control.listText) || [],
        });
      });
    });
    renderKnobControls(pendingKnobId);
    renderKnobLayerSelector(pendingKnobId);
    const portsEl = document.getElementById(pendingKnobId)?.querySelector(".widget-ports");
    if (portsEl) renderKnobPorts(pendingKnobId, portsEl);
    drawConnections();
    savePresetNow();
  }
  closeKnobSettings();
});



const buttonStates = new Map();
let buttonMidiLearnTargetId = null;
const BUTTON_MAX_COUNT = 16;
const BUTTON_MAX_LAYERS = 8;

function createButtonControl() {
  return { cc: null, channel: null, value0: 0, value127: CV_MAX_VOLTS, listText: "", listIndex: 0, wasAbove: false };
}

function createButtonLayer(bindingSource = null) {
  const controls = Array.from({ length: BUTTON_MAX_COUNT }, (_, index) => {
    const control = createButtonControl();
    if (bindingSource?.controls[index]) {
      control.cc = bindingSource.controls[index].cc;
      control.channel = bindingSource.controls[index].channel;
    }
    return control;
  });
  return { name: "", controls, labels: Array(BUTTON_MAX_COUNT).fill("") };
}

function buttonLayer(state, layerIndex = state?.activeLayer ?? 0) {
  return state?.layers?.[layerIndex] || null;
}

function buttonControlIndex(buttonIndex, layerIndex) {
  return layerIndex * BUTTON_MAX_COUNT + buttonIndex;
}

function buttonPortId(buttonIndex, layerIndex = 0) {
  return layerIndex === 0 ? `value${buttonIndex + 1}` : `value${buttonIndex + 1}Layer${layerIndex + 1}`;
}

function parseButtonPortId(portId) {
  const match = /^value(\d+)(?:Layer(\d+))?$/.exec(portId);
  if (!match) return null;
  const buttonIndex = Number(match[1]) - 1;
  const layerIndex = match[2] ? Number(match[2]) - 1 : 0;
  return buttonIndex >= 0 && buttonIndex < BUTTON_MAX_COUNT && layerIndex >= 0 && layerIndex < BUTTON_MAX_LAYERS
    ? { buttonIndex, layerIndex }
    : null;
}

function buttonValueParamName(buttonIndex, layerIndex) {
  const index = buttonControlIndex(buttonIndex, layerIndex);
  return index === 0 ? "value" : `value${index + 1}`;
}

function buttonCurrentOutput(control) {
  const list = parseValueList(control.listText);
  if (list && list.length >= 2) {
    const idx = clamp(control.listIndex, 0, list.length - 1);
    return list[idx];
  }
  return control.wasAbove ? control.value127 : control.value0;
}

function renderButtonPorts(widgetId, portsEl) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";
  portsEl.className = `widget-ports knob-output-ports button-output-ports knob-output-${state.outputSide}`;
  portsEl.style.setProperty("--knob-count", state.count);
  portsEl.style.setProperty("--knob-layer-count", state.layerCount);
  for (let buttonIndex = 0; buttonIndex < state.count; buttonIndex += 1) {
    const group = document.createElement("div");
    group.className = "knob-output-group";
    for (let layerIndex = 0; layerIndex < state.layerCount; layerIndex += 1) {
      const row = createPortRow(widgetId, { id: buttonPortId(buttonIndex, layerIndex), label: `L${layerIndex + 1}` }, "out", "button");
      row.classList.toggle("knob-output-active", layerIndex === state.activeLayer);
      group.appendChild(row);
    }
    portsEl.appendChild(group);
  }
}

function renderButtonControls(widgetId, target = null) {
  const state = buttonStates.get(widgetId);
  const grid = target || document.getElementById(widgetId)?.querySelector('[data-role="button-grid"]');
  if (!state || !grid) return;
  const layer = buttonLayer(state);
  grid.innerHTML = "";
  grid.style.setProperty("--button-columns", state.count);
  const body = grid.closest('[data-role="button-body"]');
  if (body) {
    body.style.setProperty("--button-layer-count", state.layerCount);
    body.style.setProperty("--button-column-min", `${Math.max(72, state.layerCount * 18 + (state.layerCount - 1) * 2)}px`);
    body.classList.toggle("button-single-layer", state.layerCount === 1);
  }
  for (let index = 0; index < state.count; index += 1) {
    const cell = document.createElement("div");
    cell.className = "button-control";
    cell.dataset.buttonIndex = String(index);
    const trigger = document.createElement("button");
    trigger.className = "button-trigger";
    trigger.type = "button";
    trigger.dataset.role = "button-readout";
    trigger.title = "Advance value";
    trigger.addEventListener("click", (event) => {
      event.stopPropagation();
      advanceButtonValue(widgetId, index);
    });
    const label = document.createElement("div");
    label.className = "button-control-label";
    label.textContent = layer.labels[index] || `Button ${index + 1}`;
    cell.append(trigger, label);
    grid.appendChild(cell);
  }
  updateButtonVisual(widgetId);
}

function renderButtonLayerSelector(widgetId, target = null) {
  const state = buttonStates.get(widgetId);
  const container = target || document.getElementById(widgetId)?.querySelector('[data-role="button-layer-selector"]');
  if (!state || !container) return;
  container.innerHTML = "";
  container.classList.toggle("hidden", state.layerCount === 1);
  if (state.layerCount === 1) return;
  const port = createPortRow(widgetId, { id: "layer", label: "Layer" }, "in", "button");
  const slider = document.createElement("input");
  slider.type = "range";
  slider.className = "port-slider";
  slider.min = "0";
  slider.max = String(state.layerCount - 1);
  slider.step = "1";
  slider.value = String(state.activeLayer);
  slider.disabled = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "layer");
  slider.setAttribute("aria-label", "Active layer");
  slider.addEventListener("mousedown", (event) => event.stopPropagation());
  slider.addEventListener("click", (event) => event.stopPropagation());
  slider.addEventListener("input", () => setButtonActiveLayer(widgetId, Number(slider.value), true));
  const value = document.createElement("span");
  value.className = "knob-layer-value";
  value.textContent = `${state.activeLayer + 1}/${state.layerCount}`;
  const name = document.createElement("span");
  name.className = "knob-layer-name";
  name.textContent = buttonLayer(state)?.name || `Layer ${state.activeLayer + 1}`;
  container.append(name, port, slider, value);
}

function setButtonActiveLayer(widgetId, layerIndex, sendLive = false) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  const next = clamp(Math.round(Number(layerIndex) || 0), 0, state.layerCount - 1);
  if (next === state.activeLayer && !sendLive) return;
  state.activeLayer = next;
  renderButtonControls(widgetId);
  const el = document.getElementById(widgetId);
  const slider = el?.querySelector('[data-role="button-layer-selector"] input[type="range"]');
  const value = el?.querySelector(".knob-layer-value");
  const name = el?.querySelector(".knob-layer-name");
  if (slider) slider.value = String(next);
  if (value) value.textContent = `${next + 1}/${state.layerCount}`;
  if (name) name.textContent = buttonLayer(state)?.name || `Layer ${next + 1}`;
  el?.querySelectorAll(".knob-output-group").forEach((group) => {
    Array.from(group.children).forEach((row, index) => row.classList.toggle("knob-output-active", index === next));
  });
  if (sendLive) {
    sendLiveParamUpdateThrottled(widgetId, { activeLayer: next });
    markDirty();
  }
}

function publishButtonValue(widgetId, buttonIndex) {
  const state = buttonStates.get(widgetId);
  const control = buttonLayer(state)?.controls[buttonIndex];
  if (!state || !control) return;
  const output = buttonCurrentOutput(control);
  audioEngine.setParam(widgetId, buttonValueParamName(buttonIndex, state.activeLayer), output);
  updateButtonVisual(widgetId, buttonIndex);
  propagate(widgetId, buttonPortId(buttonIndex, state.activeLayer), output);
  sendLiveParamUpdate(widgetId, {
    buttonIndex: buttonControlIndex(buttonIndex, state.activeLayer) + 1,
    listIndex: control.listIndex,
    wasAbove: control.wasAbove ? 1 : 0,
  });
}

function advanceButtonValue(widgetId, buttonIndex) {
  const state = buttonStates.get(widgetId);
  const control = buttonLayer(state)?.controls[buttonIndex];
  if (!state || !control) return;
  const list = parseValueList(control.listText);
  if (list && list.length >= 2) {
    control.listIndex = (control.listIndex + 1) % list.length;
  } else {
    control.wasAbove = !control.wasAbove;
  }
  publishButtonValue(widgetId, buttonIndex);
}

function updateButtonVisual(widgetId, buttonIndex = null) {
  const state = buttonStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const layer = buttonLayer(state);
  const indexes = buttonIndex === null ? Array.from({ length: state.count }, (_, index) => index) : [buttonIndex];
  indexes.forEach((index) => {
    const control = layer.controls[index];
    const readout = el.querySelector(`[data-button-index="${index}"] [data-role="button-readout"]`);
    const list = parseValueList(control.listText);
    const active = list && list.length >= 2 ? control.listIndex > 0 : control.wasAbove;
    if (readout) {
      readout.textContent = formatNumber(buttonCurrentOutput(control));
      readout.classList.toggle("is-active", active);
      readout.setAttribute("aria-pressed", String(active));
    }
  });
}

function routeMidiToButtons(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;

  if (buttonMidiLearnTargetId) {
    const target = buttonMidiLearnTargetId;
    const state = buttonStates.get(target.widgetId);
    if (state) {
      state.layers.forEach((layer) => {
        layer.controls[target.index].cc = data.control;
        layer.controls[target.index].channel = data.channel;
      });
      buttonMidiLearnTargetId = null;
      updateButtonSettingsCcDisplay(target.widgetId, target.index);
      sendLiveParamUpdate(target.widgetId, { buttonIndex: target.index + 1, cc: data.control, channel: data.channel });
      savePresetNow();
    }
    buttonMidiLearnTargetId = null;
    return;
  }

  // Following an incoming CC only updates the display - the device already applied it.
  withDeviceSyncMuted(() => buttonStates.forEach((state, widgetId) => {
    const bindings = buttonLayer(state, 0);
    bindings.controls.slice(0, state.count).forEach((binding, buttonIndex) => {
      if (binding.cc !== data.control || binding.channel !== data.channel) return;
      const control = buttonLayer(state).controls[buttonIndex];
      const above = data.value >= 64;
      const list = parseValueList(control.listText);
      if (list && list.length >= 2 && above && !control.wasAbove) {
        control.listIndex = (control.listIndex + 1) % list.length;
      }
      control.wasAbove = above;
      publishButtonValue(widgetId, buttonIndex);
      markDirty();
    });
  }));
}

const buttonSettingsOverlay = document.getElementById("buttonSettingsOverlay");
const buttonSettingsAdd = document.getElementById("buttonSettingsAdd");
const buttonSettingsAddLayer = document.getElementById("buttonSettingsAddLayer");
const buttonSettingsLayerTabs = document.getElementById("buttonSettingsLayerTabs");
const buttonLayerNameInput = document.getElementById("buttonLayerNameInput");
const buttonSettingsGrid = document.getElementById("buttonSettingsGrid");
const buttonOutputSideInput = document.getElementById("buttonOutputSideInput");
const buttonCcContextMenu = document.getElementById("buttonCcContextMenu");
const buttonLayerContextMenu = document.getElementById("buttonLayerContextMenu");
let pendingButtonId = null;
let pendingButtonLayerIndex = 0;
let buttonCcContextTarget = null;
let buttonLayerContextTarget = null;

function updateButtonCardDisabledState(card) {
  const list = parseValueList(card.querySelector('[data-button-field="listText"]').value);
  const listValid = !!(list && list.length >= 2);
  card.querySelectorAll("[data-button-fixed]").forEach((input) => { input.disabled = listValid; });
}

function buttonCcDisplayText(widgetId, index) {
  if (buttonMidiLearnTargetId?.widgetId === widgetId && buttonMidiLearnTargetId.index === index) return "Waiting…";
  const binding = buttonLayer(buttonStates.get(widgetId), 0)?.controls[index];
  return binding?.cc === null || binding?.cc === undefined ? "Learn" : `${binding.cc}${binding.channel === null ? "" : ` (Ch ${binding.channel + 1})`}`;
}

function updateButtonSettingsCcDisplay(widgetId, index) {
  const display = buttonSettingsGrid.querySelector(`[data-button-index="${index}"] [data-button-field="cc"]`);
  if (!display || pendingButtonId !== widgetId) return;
  const waiting = buttonMidiLearnTargetId?.widgetId === widgetId && buttonMidiLearnTargetId.index === index;
  display.textContent = buttonCcDisplayText(widgetId, index);
  display.classList.toggle("waiting", waiting);
}

function startButtonMidiLearn(widgetId, index) {
  const previous = buttonMidiLearnTargetId;
  buttonMidiLearnTargetId = { widgetId, index };
  if (previous) updateButtonSettingsCcDisplay(previous.widgetId, previous.index);
  updateButtonSettingsCcDisplay(widgetId, index);
}

function cancelButtonMidiLearn() {
  if (!buttonMidiLearnTargetId) return false;
  const target = buttonMidiLearnTargetId;
  buttonMidiLearnTargetId = null;
  updateButtonSettingsCcDisplay(target.widgetId, target.index);
  return true;
}

function clearButtonMidiLearn(widgetId, index) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  if (buttonMidiLearnTargetId?.widgetId === widgetId && buttonMidiLearnTargetId.index === index) buttonMidiLearnTargetId = null;
  state.layers.forEach((layer) => {
    layer.controls[index].cc = null;
    layer.controls[index].channel = null;
  });
  updateButtonSettingsCcDisplay(widgetId, index);
  sendLiveParamUpdate(widgetId, { buttonIndex: index + 1, cc: null, channel: null });
  savePresetNow();
}

function closeButtonCcContextMenu() {
  buttonCcContextMenu.classList.add("hidden");
  buttonCcContextTarget = null;
}

function openButtonCcContextMenu(event, widgetId, index) {
  event.preventDefault();
  event.stopPropagation();
  buttonCcContextTarget = { widgetId, index };
  buttonCcContextMenu.classList.remove("hidden");
  const rect = buttonCcContextMenu.getBoundingClientRect();
  buttonCcContextMenu.style.left = `${Math.min(event.clientX, window.innerWidth - rect.width - 8)}px`;
  buttonCcContextMenu.style.top = `${Math.min(event.clientY, window.innerHeight - rect.height - 8)}px`;
}

function closeButtonLayerContextMenu() {
  buttonLayerContextMenu.classList.add("hidden");
  buttonLayerContextTarget = null;
}

function openButtonLayerContextMenu(event, widgetId, layerIndex) {
  event.preventDefault();
  event.stopPropagation();
  buttonLayerContextTarget = { widgetId, layerIndex };
  buttonLayerContextMenu.querySelector('[data-action="delete"]').disabled = buttonStates.get(widgetId)?.layerCount <= 1;
  buttonLayerContextMenu.classList.remove("hidden");
  const rect = buttonLayerContextMenu.getBoundingClientRect();
  buttonLayerContextMenu.style.left = `${Math.min(event.clientX, window.innerWidth - rect.width - 8)}px`;
  buttonLayerContextMenu.style.top = `${Math.min(event.clientY, window.innerHeight - rect.height - 8)}px`;
}

function renderButtonSettingsLayerTabs(widgetId) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  buttonSettingsLayerTabs.innerHTML = "";
  for (let layerIndex = 0; layerIndex < state.layerCount; layerIndex += 1) {
    const tab = document.createElement("button");
    tab.type = "button";
    tab.className = "btn-secondary knob-settings-layer-tab";
    tab.textContent = state.layers[layerIndex].name || `Layer ${layerIndex + 1}`;
    tab.dataset.layerIndex = String(layerIndex);
    tab.classList.toggle("active", layerIndex === pendingButtonLayerIndex);
    tab.setAttribute("role", "tab");
    tab.setAttribute("aria-selected", String(layerIndex === pendingButtonLayerIndex));
    tab.addEventListener("click", () => {
      syncButtonSettingsGrid(widgetId, pendingButtonLayerIndex);
      pendingButtonLayerIndex = layerIndex;
      renderButtonSettingsGrid(widgetId);
    });
    tab.addEventListener("contextmenu", (event) => openButtonLayerContextMenu(event, widgetId, layerIndex));
    buttonSettingsLayerTabs.appendChild(tab);
  }
  buttonSettingsAddLayer.disabled = state.layerCount >= BUTTON_MAX_LAYERS;
  buttonLayerNameInput.value = buttonLayer(state, pendingButtonLayerIndex)?.name || "";
}

function renderButtonSettingsGrid(widgetId) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  pendingButtonLayerIndex = clamp(pendingButtonLayerIndex, 0, state.layerCount - 1);
  const layer = buttonLayer(state, pendingButtonLayerIndex);
  renderButtonSettingsLayerTabs(widgetId);
  buttonSettingsGrid.innerHTML = "";
  for (let index = 0; index < state.count; index += 1) {
    const control = layer.controls[index];
    const card = document.createElement("section");
    card.className = "button-settings-card";
    card.dataset.buttonIndex = String(index);
    const number = document.createElement("strong");
    number.className = "knob-settings-number";
    number.textContent = String(index + 1);
    card.appendChild(number);
    const cc = document.createElement("button");
    cc.type = "button";
    cc.className = "knob-settings-cc-value";
    cc.dataset.buttonField = "cc";
    cc.textContent = buttonCcDisplayText(widgetId, index);
    cc.classList.toggle("waiting", buttonMidiLearnTargetId?.widgetId === widgetId && buttonMidiLearnTargetId.index === index);
    if (pendingButtonLayerIndex === 0) {
      cc.addEventListener("click", () => {
        if (buttonMidiLearnTargetId?.widgetId === widgetId && buttonMidiLearnTargetId.index === index) cancelButtonMidiLearn();
        else startButtonMidiLearn(widgetId, index);
      });
    } else {
      cc.classList.add("readonly");
      cc.title = "MIDI Learn is available on Layer 1";
    }
    cc.addEventListener("contextmenu", (event) => openButtonCcContextMenu(event, widgetId, index));
    card.appendChild(cc);
    const addInput = (field, value, fixed = false) => {
      const input = createKnobSettingsInput(field, value);
      delete input.dataset.knobField;
      input.dataset.buttonField = field;
      if (fixed) input.dataset.buttonFixed = "true";
      card.appendChild(input);
      return input;
    };
    addInput("label", layer.labels[index] ?? "");
    const listInput = addInput("listText", control.listText ?? "");
    addInput("value0", control.value0, true);
    addInput("value127", control.value127, true);
    listInput.addEventListener("input", () => updateButtonCardDisabledState(card));
    updateButtonCardDisabledState(card);
    const actions = document.createElement("div");
    actions.className = "knob-settings-actions";
    [["up", "↑", "Move button left"], ["down", "↓", "Move button right"], ["delete", "✕", "Delete button"]].forEach(([action, text, title]) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "btn-secondary";
      button.textContent = text;
      button.title = title;
      button.setAttribute("aria-label", title);
      button.disabled = (action === "up" && index === 0) || (action === "down" && index === state.count - 1) || (action === "delete" && state.count === 1);
      button.addEventListener("click", () => editButtonSettingsStructure(widgetId, index, action));
      actions.appendChild(button);
    });
    card.appendChild(actions);
    buttonSettingsGrid.appendChild(card);
  }
}

function syncButtonSettingsGrid(widgetId, layerIndex = pendingButtonLayerIndex) {
  const state = buttonStates.get(widgetId);
  const layer = buttonLayer(state, layerIndex);
  if (!state || !layer) return;
  layer.name = buttonLayerNameInput.value.trim().slice(0, 32);
  buttonSettingsGrid.querySelectorAll("[data-button-index]").forEach((card) => {
    const index = Number(card.dataset.buttonIndex);
    const field = (name) => card.querySelector(`[data-button-field="${name}"]`).value;
    const control = layer.controls[index];
    control.listText = field("listText");
    const value0 = parseFloatFlexible(field("value0"));
    const value127 = parseFloatFlexible(field("value127"));
    control.value0 = Number.isNaN(value0) ? 0 : value0;
    control.value127 = Number.isNaN(value127) ? CV_MAX_VOLTS : value127;
    const list = parseValueList(control.listText);
    if (list) control.listIndex = clamp(control.listIndex, 0, list.length - 1);
    layer.labels[index] = field("label").trim();
  });
}

function editButtonSettingsStructure(widgetId, index, action) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  syncButtonSettingsGrid(widgetId);
  if (action === "add" && state.count < BUTTON_MAX_COUNT) {
    state.layers.forEach((layer, layerIndex) => {
      layer.controls[state.count] = createButtonControl();
      if (layerIndex > 0) {
        layer.controls[state.count].cc = state.layers[0].controls[state.count].cc;
        layer.controls[state.count].channel = state.layers[0].controls[state.count].channel;
      }
      layer.labels[state.count] = "";
    });
    state.count += 1;
  } else if (action === "delete" && state.count > 1) {
    for (let layerIndex = 0; layerIndex < state.layerCount; layerIndex += 1) removeConnectionsForPort(widgetId, buttonPortId(index, layerIndex), "out");
    const shifted = connections.filter((connection) => {
      if (connection.from.widgetId !== widgetId) return false;
      const parsed = parseButtonPortId(connection.from.portId);
      return parsed && parsed.buttonIndex > index;
    });
    shifted.forEach(disconnectIfAudio);
    shifted.forEach((connection) => {
      const parsed = parseButtonPortId(connection.from.portId);
      connection.from.portId = buttonPortId(parsed.buttonIndex - 1, parsed.layerIndex);
      connectIfAudio(connection);
    });
    state.layers.forEach((layer) => {
      layer.controls.splice(index, 1);
      layer.controls.push(createButtonControl());
      layer.labels.splice(index, 1);
      layer.labels.push("");
    });
    state.count -= 1;
  } else if (action === "up" && index > 0) {
    state.layers.forEach((layer) => {
      [layer.controls[index - 1], layer.controls[index]] = [layer.controls[index], layer.controls[index - 1]];
      [layer.labels[index - 1], layer.labels[index]] = [layer.labels[index], layer.labels[index - 1]];
    });
  } else if (action === "down" && index < state.count - 1) {
    state.layers.forEach((layer) => {
      [layer.controls[index], layer.controls[index + 1]] = [layer.controls[index + 1], layer.controls[index]];
      [layer.labels[index], layer.labels[index + 1]] = [layer.labels[index + 1], layer.labels[index]];
    });
  }
  renderButtonSettingsGrid(widgetId);
}

function addButtonLayer(widgetId) {
  const state = buttonStates.get(widgetId);
  if (!state || state.layerCount >= BUTTON_MAX_LAYERS) return;
  syncButtonSettingsGrid(widgetId);
  state.layers.push(createButtonLayer(state.layers[0]));
  state.layerCount += 1;
  pendingButtonLayerIndex = state.layerCount - 1;
  renderButtonSettingsGrid(widgetId);
}

function deleteButtonLayer(widgetId, removedLayer = pendingButtonLayerIndex) {
  const state = buttonStates.get(widgetId);
  if (!state || state.layerCount <= 1) return;
  const currentLayer = pendingButtonLayerIndex;
  syncButtonSettingsGrid(widgetId);
  removedLayer = clamp(removedLayer, 0, state.layerCount - 1);
  for (let buttonIndex = 0; buttonIndex < state.count; buttonIndex += 1) removeConnectionsForPort(widgetId, buttonPortId(buttonIndex, removedLayer), "out");
  const shifted = connections.filter((connection) => {
    if (connection.from.widgetId !== widgetId) return false;
    const parsed = parseButtonPortId(connection.from.portId);
    return parsed && parsed.layerIndex > removedLayer;
  });
  shifted.forEach(disconnectIfAudio);
  shifted.forEach((connection) => {
    const parsed = parseButtonPortId(connection.from.portId);
    connection.from.portId = buttonPortId(parsed.buttonIndex, parsed.layerIndex - 1);
    connectIfAudio(connection);
  });
  state.layers.splice(removedLayer, 1);
  state.layerCount -= 1;
  if (state.layerCount === 1) removeConnectionsForPort(widgetId, "layer", "in");
  if (state.activeLayer === removedLayer) state.activeLayer = Math.min(removedLayer, state.layerCount - 1);
  else if (state.activeLayer > removedLayer) state.activeLayer -= 1;
  pendingButtonLayerIndex = currentLayer === removedLayer ? Math.min(removedLayer, state.layerCount - 1) : currentLayer - (currentLayer > removedLayer ? 1 : 0);
  renderButtonSettingsGrid(widgetId);
}

function openButtonSettings(widgetId) {
  const state = buttonStates.get(widgetId);
  if (!state) return;
  pendingButtonId = widgetId;
  pendingButtonLayerIndex = state.activeLayer;
  buttonOutputSideInput.value = state.outputSide;
  buttonMidiLearnTargetId = null;
  renderButtonSettingsGrid(widgetId);
  buttonSettingsOverlay.classList.remove("hidden");
}

function closeButtonSettings() {
  buttonSettingsOverlay.classList.add("hidden");
  closeButtonCcContextMenu();
  closeButtonLayerContextMenu();
  pendingButtonId = null;
  pendingButtonLayerIndex = 0;
  buttonMidiLearnTargetId = null;
}


document.getElementById("buttonSettingsCancel").addEventListener("click", closeButtonSettings);

buttonSettingsAdd.addEventListener("click", () => {
  if (pendingButtonId) editButtonSettingsStructure(pendingButtonId, 0, "add");
});

buttonSettingsAddLayer.addEventListener("click", () => {
  if (pendingButtonId) addButtonLayer(pendingButtonId);
});

buttonLayerNameInput.addEventListener("input", () => {
  const state = buttonStates.get(pendingButtonId);
  const layer = buttonLayer(state, pendingButtonLayerIndex);
  if (!layer) return;
  layer.name = buttonLayerNameInput.value.trimStart().slice(0, 32);
  const tab = buttonSettingsLayerTabs.querySelector(`[data-layer-index="${pendingButtonLayerIndex}"]`);
  if (tab) tab.textContent = layer.name || `Layer ${pendingButtonLayerIndex + 1}`;
});

buttonCcContextMenu.addEventListener("click", (event) => {
  const action = event.target.closest("button")?.dataset.action;
  const target = buttonCcContextTarget;
  closeButtonCcContextMenu();
  if (action === "unlearn" && target) clearButtonMidiLearn(target.widgetId, target.index);
});

buttonLayerContextMenu.addEventListener("click", (event) => {
  const action = event.target.closest("button")?.dataset.action;
  const target = buttonLayerContextTarget;
  closeButtonLayerContextMenu();
  if (action === "delete" && target) deleteButtonLayer(target.widgetId, target.layerIndex);
});

document.addEventListener("mousedown", (event) => {
  if (!event.target.closest("#buttonCcContextMenu")) closeButtonCcContextMenu();
  if (!event.target.closest("#buttonLayerContextMenu")) closeButtonLayerContextMenu();
});

document.getElementById("buttonSettingsSave").addEventListener("click", () => {
  if (!pendingButtonId) return;
  const state = buttonStates.get(pendingButtonId);
  if (state) {
    syncButtonSettingsGrid(pendingButtonId);
    state.outputSide = ["top", "bottom"].includes(buttonOutputSideInput.value) ? buttonOutputSideInput.value : "bottom";
    state.layers.forEach((layer, layerIndex) => {
      layer.controls.slice(0, state.count).forEach((control, buttonIndex) => {
        const binding = state.layers[0].controls[buttonIndex];
        control.cc = binding.cc;
        control.channel = binding.channel;
        const output = buttonCurrentOutput(control);
        audioEngine.setParam(pendingButtonId, buttonValueParamName(buttonIndex, layerIndex), output);
        if (layerIndex === state.activeLayer) propagate(pendingButtonId, buttonPortId(buttonIndex, layerIndex), output);
        sendLiveParamUpdate(pendingButtonId, {
          buttonIndex: buttonControlIndex(buttonIndex, layerIndex) + 1,
          buttonCount: state.count,
          layerCount: state.layerCount,
          activeLayer: state.activeLayer,
          cc: control.cc,
          channel: control.channel,
          value0: control.value0,
          value127: control.value127,
          listIndex: control.listIndex,
          wasAbove: control.wasAbove ? 1 : 0,
          valueList: parseValueList(control.listText) || [],
        });
      });
    });
    renderButtonControls(pendingButtonId);
    renderButtonLayerSelector(pendingButtonId);
    const portsEl = document.getElementById(pendingButtonId)?.querySelector(".widget-ports");
    if (portsEl) renderButtonPorts(pendingButtonId, portsEl);
    drawConnections();
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

  
  
  
  
  // Following an incoming CC only updates the display - the device already applied it.
  withDeviceSyncMuted(() => upDownButtonStates.forEach((state, widgetId) => {
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
  }));
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

  
  
  
  // Following an incoming CC only updates the display - the device already applied it.
  withDeviceSyncMuted(() => switchStates.forEach((state, widgetId) => {
    if (state.cc === null || state.cc !== data.control || state.channel !== data.channel) return;
    setSwitchSelectIn2(widgetId, data.value >= 64);
    markDirty();
  }));
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



const outputSwitchStates = new Map();
let outputSwitchMidiLearnTargetId = null;

function updateOutputSwitchVisual(widgetId) {
  const state = outputSwitchStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const toggle = el.querySelector('[data-role="output-switch-toggle"]');
  if (toggle) toggle.checked = state.selectOut2;
  const label = el.querySelector('[data-role="output-switch-select-label"]');
  if (label) label.textContent = state.selectOut2 ? "Out 2" : "Out 1";
  ["out1", "out2"].forEach((portId) => {
    const row = el.querySelector(`.port-row-out:has(.port-dot[data-port-id="${portId}"])`);
    if (!row) return;
    row.classList.toggle("switch-selected", (portId === "out2") === state.selectOut2);
  });
  const ccLabel = el.querySelector('[data-role="output-switch-cc-label"]');
  if (ccLabel) {
    ccLabel.textContent =
      state.cc === null ? "CC: –" : `CC ${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  }
}

function setOutputSwitchSelectOut2(widgetId, selectOut2) {
  const state = outputSwitchStates.get(widgetId);
  if (!state) return;
  state.selectOut2 = !!selectOut2;
  audioEngine.setParam(widgetId, "selectOut2", state.selectOut2);
  updateOutputSwitchVisual(widgetId);
  sendLiveParamUpdate(widgetId, { selectOut2: state.selectOut2 });
  // Deliberately no propagate() here: flipping the switch must not push the
  // CURRENT "in" value onto the newly active output - the just-activated
  // output should keep showing whatever it last actually received (matches
  // the firmware's held1/held2 freeze), not jump to a value it never saw.
  // Only an actual change on "in" (below) is ever allowed to propagate.
}

function routeMidiToOutputSwitches(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;

  if (outputSwitchMidiLearnTargetId) {
    const state = outputSwitchStates.get(outputSwitchMidiLearnTargetId);
    if (state) {
      state.cc = data.control;
      state.channel = data.channel;
      outputSwitchCcDisplay.value = `${data.control} (Ch ${data.channel + 1})`;
      outputSwitchLearnBtn.textContent = "Learn";
      updateOutputSwitchVisual(outputSwitchMidiLearnTargetId);
      sendLiveParamUpdate(outputSwitchMidiLearnTargetId, { cc: data.control, channel: data.channel });
      savePresetNow();
    }
    outputSwitchMidiLearnTargetId = null;
    return;
  }

  // Following an incoming CC only updates the display - the device already applied it.
  withDeviceSyncMuted(() => outputSwitchStates.forEach((state, widgetId) => {
    if (state.cc === null || state.cc !== data.control || state.channel !== data.channel) return;
    setOutputSwitchSelectOut2(widgetId, data.value >= 64);
    markDirty();
  }));
}

const outputSwitchSettingsOverlay = document.getElementById("outputSwitchSettingsOverlay");
const outputSwitchCcDisplay = document.getElementById("outputSwitchCcDisplay");
const outputSwitchLearnBtn = document.getElementById("outputSwitchLearnBtn");
let pendingOutputSwitchId = null;

function openOutputSwitchSettings(widgetId) {
  const state = outputSwitchStates.get(widgetId);
  if (!state) return;
  pendingOutputSwitchId = widgetId;
  outputSwitchCcDisplay.value = state.cc === null ? "" : `${state.cc}${state.channel === null ? "" : ` (Ch ${state.channel + 1})`}`;
  outputSwitchLearnBtn.textContent = "Learn";
  outputSwitchMidiLearnTargetId = null;
  outputSwitchSettingsOverlay.classList.remove("hidden");
}

function closeOutputSwitchSettings() {
  outputSwitchSettingsOverlay.classList.add("hidden");
  pendingOutputSwitchId = null;
  outputSwitchMidiLearnTargetId = null;
  outputSwitchLearnBtn.textContent = "Learn";
}


document.getElementById("outputSwitchSettingsCancel").addEventListener("click", closeOutputSwitchSettings);

outputSwitchLearnBtn.addEventListener("click", () => {
  if (!pendingOutputSwitchId) return;
  outputSwitchMidiLearnTargetId = pendingOutputSwitchId;
  outputSwitchLearnBtn.textContent = "Waiting for CC…";
});

document.getElementById("outputSwitchSettingsSave").addEventListener("click", () => {
  closeOutputSwitchSettings();
});



const linkedInputSettingsOverlay = document.getElementById("linkedInputSettingsOverlay");
const linkedInputNameInput = document.getElementById("linkedInputNameInput");
const linkedInputNameError = document.getElementById("linkedInputNameError");
const linkedInputLabels = document.getElementById("linkedInputLabels");
const linkedInputPortSide = document.getElementById("linkedInputPortSide");

function openLinkedInputSettings(widgetId) {
  const state = linkedInputStates.get(widgetId);
  if (!state) return;
  pendingLinkedInputId = widgetId;
  linkedInputNameInput.value = state.linkName || "";
  linkedInputPortSide.value = state.portSide || "left";
  linkedInputLabels.innerHTML = "";
  for (let index = 0; index < state.linkCount; index += 1) {
    const label = document.createElement("label");
    label.className = "modal-field";
    const title = document.createElement("span");
    title.textContent = `Port ${index + 1} label`;
    const input = document.createElement("input");
    input.type = "text";
    input.maxLength = 32;
    input.dataset.linkedPortLabel = String(index);
    input.placeholder = String(index + 1);
    input.value = state.labels?.[index] || "";
    label.append(title, input);
    linkedInputLabels.appendChild(label);
  }
  linkedInputNameError.style.display = "none";
  linkedInputNameError.textContent = "";
  linkedInputSettingsOverlay.classList.remove("hidden");
}

function closeLinkedInputSettings() {
  linkedInputSettingsOverlay.classList.add("hidden");
  pendingLinkedInputId = null;
}


document.getElementById("linkedInputSettingsCancel").addEventListener("click", closeLinkedInputSettings);

document.getElementById("linkedInputSettingsSave").addEventListener("click", () => {
  if (!pendingLinkedInputId) return;
  const state = linkedInputStates.get(pendingLinkedInputId);
  if (!state) {
    closeLinkedInputSettings();
    return;
  }
  const trimmed = linkedInputNameInput.value.trim();
  if (!trimmed) {
    linkedInputNameError.textContent = " Name must not be empty";
    linkedInputNameError.style.display = "block";
    return;
  }
  const duplicate = [...linkedInputStates.entries()].some(
    ([otherId, other]) => otherId !== pendingLinkedInputId && other.linkName.trim().toLowerCase() === trimmed.toLowerCase()
  );
  if (duplicate) {
    linkedInputNameError.textContent = "Name already taken";
    linkedInputNameError.style.display = "block";
    return;
  }
  state.linkName = trimmed;
  state.labels = Array.from(linkedInputLabels.querySelectorAll("[data-linked-port-label]"), (input) => input.value.trim());
  state.portSide = ["top", "bottom", "left", "right"].includes(linkedInputPortSide.value) ? linkedInputPortSide.value : "left";
  refreshLinkedIO();
  savePresetNow();
  closeLinkedInputSettings();
});

const audioLinkSettingsOverlay = document.getElementById("audioLinkSettingsOverlay");
const audioLinkLabels = document.getElementById("audioLinkLabels");

function openAudioLinkSettings(widgetId) {
  if (!audioLinkSettingsOverlay || !audioLinkLabels) return;
  pendingAudioLinkId = widgetId;
  const labels = getAudioLinkLabels(widgetId);
  audioLinkLabels.innerHTML = "";
  for (let index = 0; index < LINK_BUS_COUNT; index += 1) {
    const field = document.createElement("label");
    field.className = "modal-field";
    const title = document.createElement("span");
    title.textContent = `Bus ${index + 1} label`;
    const input = document.createElement("input");
    input.type = "text";
    input.maxLength = 32;
    input.placeholder = String(index + 1);
    input.value = labels[index] || "";
    input.dataset.audioLinkLabel = String(index);
    field.append(title, input);
    audioLinkLabels.appendChild(field);
  }
  audioLinkSettingsOverlay.classList.remove("hidden");
}

function closeAudioLinkSettings() {
  audioLinkSettingsOverlay?.classList.add("hidden");
  pendingAudioLinkId = null;
}

document.getElementById("audioLinkSettingsCancel").addEventListener("click", closeAudioLinkSettings);
document.getElementById("audioLinkSettingsSave").addEventListener("click", () => {
  if (!pendingAudioLinkId) return;
  const labels = Array.from(
    audioLinkLabels.querySelectorAll("[data-audio-link-label]"),
    (input) => input.value.trim(),
  );
  setAudioLinkLabels(labels);
  savePresetNow();
  closeAudioLinkSettings();
});

const linkedOutputSettingsOverlay = document.getElementById("linkedOutputSettingsOverlay");
const linkedOutputSourceSelect = document.getElementById("linkedOutputSourceSelect");
const linkedOutputPortSide = document.getElementById("linkedOutputPortSide");

function openLinkedOutputSettings(widgetId) {
  const state = linkedOutputStates.get(widgetId);
  if (!state) return;
  pendingLinkedOutputId = widgetId;
  linkedOutputPortSide.value = state.portSide || "right";

  linkedOutputSourceSelect.innerHTML = "";
  const noneOpt = document.createElement("option");
  noneOpt.value = "";
  noneOpt.textContent = "— Not connected —";
  linkedOutputSourceSelect.appendChild(noneOpt);

  const outputs = [...linkedInputStates.entries()]
    .filter(([, source]) => source.linkCount === state.linkCount)
    .map(([widgetId, source]) => ({ widgetId, ...source }));
  outputs.forEach((out) => {
    const opt = document.createElement("option");
    opt.value = out.linkId;
    opt.textContent = `${pageTitleForWidget(out.widgetId)} — ${out.linkName || out.linkId}`;
    linkedOutputSourceSelect.appendChild(opt);
  });

  if (state.sourceLinkId && !outputs.some((out) => out.linkId === state.sourceLinkId)) {
    const opt = document.createElement("option");
    opt.value = state.sourceLinkId;
    opt.textContent = `Missing link: ${state.sourceLinkId}`;
    linkedOutputSourceSelect.appendChild(opt);
  }

  linkedOutputSourceSelect.value = state.sourceLinkId || "";
  linkedOutputSettingsOverlay.classList.remove("hidden");
}

function closeLinkedOutputSettings() {
  linkedOutputSettingsOverlay.classList.add("hidden");
  pendingLinkedOutputId = null;
}


document.getElementById("linkedOutputSettingsCancel").addEventListener("click", closeLinkedOutputSettings);

document.getElementById("linkedOutputSettingsSave").addEventListener("click", () => {
  if (!pendingLinkedOutputId) return;
  const state = linkedOutputStates.get(pendingLinkedOutputId);
  if (state) {
    state.sourceLinkId = linkedOutputSourceSelect.value || "";
    state.portSide = ["top", "bottom", "left", "right"].includes(linkedOutputPortSide.value) ? linkedOutputPortSide.value : "right";
    audioEngine.setParam(pendingLinkedOutputId, "sourceLinkId", state.sourceLinkId);
    refreshLinkedIO();
    refreshControlGraph();
    refreshVoiceGroups();
    savePresetNow();
  }
  closeLinkedOutputSettings();
});



const midiInStates = new Map();
const midiFilterStates = new Map();



const midiVoiceSlotStates = new Map();
const MIDI_IN_MIN_VOICES = 1;
const MIDI_IN_MAX_VOICES = 16;
const midiInSettingsOverlay = document.getElementById("midiInSettingsOverlay");
const midiInChannelSelect = document.getElementById("midiInChannelSelect");
const midiInOverflowModeSelect = document.getElementById("midiInOverflowModeSelect");
const midiInPitchBendRangeInput = document.getElementById("midiInPitchBendRangeInput");
const midiInPitchBendQuantizeInput = document.getElementById("midiInPitchBendQuantizeInput");
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

function updateMidiInVoiceSlotsLabel(widgetId) {
  const el = document.getElementById(widgetId);
  const label = el ? el.querySelector('[data-role="midi-in-voice-slots"]') : null;
  if (!label) return;
  const state = midiInStates.get(widgetId);
  const slotState = midiVoiceSlotStates.get(widgetId);
  if (!state || !slotState) return;

  if (state.voices <= 1) {
    label.classList.add("hidden");
    return;
  }

  label.classList.remove("hidden");
  const active = slotState.slots.reduce((n, s) => n + (s.note !== null ? 1 : 0), 0);
  label.textContent = `${active}/${state.voices}`;
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
  updateMidiInVoiceSlotsLabel(widgetId);
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
  midiInPitchBendRangeInput.value = state.pitchBendRangeCents ?? 200;
  midiInPitchBendQuantizeInput.checked = state.pitchBendQuantizeCents === true;
  midiInSettingsOverlay.classList.remove("hidden");
}

function closeMidiInSettings() {
  midiInSettingsOverlay.classList.add("hidden");
  pendingMidiInId = null;
}


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
    const cents = parseFloatFlexible(midiInPitchBendRangeInput.value);
    state.pitchBendRangeCents = Number.isNaN(cents) ? 200 : clamp(cents, 1, 6000);
    state.pitchBendQuantizeCents = midiInPitchBendQuantizeInput.checked === true;
    sendLiveParamUpdate(pendingMidiInId, {
      pitchBendRangeCents: state.pitchBendRangeCents,
      pitchBendQuantizeCents: state.pitchBendQuantizeCents,
    });
    savePresetNow();
  }
  closeMidiInSettings();
});












function handleVoicesCapped() {}



function createPianoKeyboard({ minNote = 21, maxNote = 108, zones = [], pressedNotes = new Set(), onRangeSelect }) {
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
    key.classList.toggle("piano-key-midi-active", pressedNotes.has(note));
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
const midiSplitPressedNotes = new Set();
const midiSplitReleaseTimers = new Map();
const MIDI_SPLIT_KEY_PREVIEW_MS = 160;

function previewMidiSplitNote(note, noteOn) {
  if (!Number.isInteger(note) || !pendingMidiSplitId) return;
  const timer = midiSplitReleaseTimers.get(note);
  if (timer) clearTimeout(timer);
  if (noteOn) {
    midiSplitReleaseTimers.delete(note);
    midiSplitPressedNotes.add(note);
  } else {
    midiSplitReleaseTimers.set(note, setTimeout(() => {
      midiSplitReleaseTimers.delete(note);
      midiSplitPressedNotes.delete(note);
      midiSplitKeyboardContainer.querySelector(`[data-note="${note}"]`)?.classList.remove("piano-key-midi-active");
    }, MIDI_SPLIT_KEY_PREVIEW_MS));
  }
  midiSplitKeyboardContainer.querySelector(`[data-note="${note}"]`)?.classList.toggle(
    "piano-key-midi-active",
    midiSplitPressedNotes.has(note)
  );
}

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
        midiSplitEditState.zones.push({ from: 127, to: 0, octave: 0 });
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
    pressedNotes: midiSplitPressedNotes,
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
  midiSplitPressedNotes.clear();
  midiSplitActiveZone = 0;
  renderMidiSplitEditor();
  midiSplitSettingsOverlay.classList.remove("hidden");
}

function closeMidiSplitSettings() {
  midiSplitSettingsOverlay.classList.add("hidden");
  pendingMidiSplitId = null;
  midiSplitEditState = null;
  midiSplitPressedNotes.clear();
  midiSplitReleaseTimers.forEach((timer) => clearTimeout(timer));
  midiSplitReleaseTimers.clear();
}


document.getElementById("midiSplitSettingsCancel").addEventListener("click", closeMidiSplitSettings);

midiSplitAddZoneButton.addEventListener("click", () => {
  if (!midiSplitEditState || midiSplitEditState.zoneCount >= MIDI_SPLIT_MAX_ZONES) return;
  midiSplitEditState.zones[midiSplitEditState.zoneCount] = { from: 127, to: 0, octave: 0 };
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
  fileExplorerList.innerHTML = '<div class="file-explorer-loading">⏳ Loading…</div>';

  const [listData, info] = await Promise.all([fetchFileList(fileExplorerDir), fetchFsInfo()]);
  if (myToken !== fileExplorerRenderToken) return;

  if (info?.available) {
    fileExplorerFreeBytes = info.freeBytes;
    fileExplorerSpace.textContent = `${humanFileSize(info.usedBytes)} / ${humanFileSize(info.totalBytes)} used`;
  } else {
    fileExplorerFreeBytes = 0;
    fileExplorerSpace.textContent = fileExplorerVolume === "sd" ? "not SD Card available" : "not available";
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
const mdaPianoStates = new Map();
const mdaPianoSettingsOverlay = document.getElementById("mdaPianoSettingsOverlay");
const mdaPianoVoicesInput = document.getElementById("mdaPianoVoicesInput");
let pendingMdaPianoId = null;

const MDA_PIANO_CV_PARAMS = [
  "voiceType", "drive", "tremWahMode", "tremWahDepth", "tremWahRate",
  "choPhaMode", "choPhaDepth", "choPhaSpeed", "preGain", "gain",
];

function updateMdaPianoControls(widgetId) {
  const state = mdaPianoStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  ["enableModWheel", "enablePitchWheel"].forEach((portId) => {
    const input = el.querySelector(`[data-role="mda-piano-${portId}-switch"]`);
    if (input) input.checked = state[portId] === true;
  });
  MDA_PIANO_CV_PARAMS.forEach((portId) => {
    const slider = el.querySelector(`[data-role="mda-piano-${portId}-slider"]`);
    const value = el.querySelector(`[data-role="mda-piano-${portId}-value"]`);
    if (slider) slider.value = String(state[portId]);
    if (value) value.textContent = formatNumber(state[portId]);
  });
}

function openMdaPianoSettings(widgetId) {
  const state = mdaPianoStates.get(widgetId);
  if (!state) return;
  pendingMdaPianoId = widgetId;
  mdaPianoVoicesInput.value = String(state.maxVoices);
  mdaPianoSettingsOverlay.classList.remove("hidden");
}

function closeMdaPianoSettings() {
  mdaPianoSettingsOverlay.classList.add("hidden");
  pendingMdaPianoId = null;
}

document.getElementById("mdaPianoSettingsCancel").addEventListener("click", closeMdaPianoSettings);
document.getElementById("mdaPianoSettingsSave").addEventListener("click", () => {
  const state = mdaPianoStates.get(pendingMdaPianoId);
  if (!state) return;
  state.maxVoices = clamp(parseInt(mdaPianoVoicesInput.value, 10) || 32, 4, 128);
  savePresetNow();
  closeMdaPianoSettings();
});

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
          connectIfAudio(c);
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

  updateDualModeAudioNode(widgetId, "multiplier", state, inputPortIds, { inputCount: state.inputCount, values: state.values });

  recomputeMultiplier(widgetId);
}

function refreshAllDualModeWidgets() {
  oscillatorStates.forEach((_, id) => {
    
    
    
    
    if (!connections.some((c) => c.to.widgetId === id && c.to.portId === "amplitude")) {
      audioEngine.setParam(id, "amplitude", 0, 0);
    }
    updateOscillatorGainVisual(id);
    updateOscillatorTuneVisual(id);
    updateOscillatorFeedbackVisual(id);
  });
  fmOscillatorStates.forEach((_, id) => {
    if (!connections.some((c) => c.to.widgetId === id && c.to.portId === "amplitude")) {
      audioEngine.setParam(id, "amplitude", 0, 0);
    }
    updateFmOscillatorGainVisual(id);
    updateFmOscillatorTuneVisual(id);
    updateFmOscillatorIndexVisual(id);
    updateFmOscillatorFeedbackVisual(id);
    updateFmOscillatorDutyCycleVisual(id);
    updateFmOscillatorSuperSawVisual(id);
  });
  hammondStates.forEach((_, id) => {
    if (!connections.some((c) => c.to.widgetId === id && c.to.portId === "amplitude")) {
      audioEngine.setParam(id, "amplitude", 0, 0);
    }
    updateHammondVisual(id);
  });

  
  
  gainStates.forEach((_, id) => updateGainVisual(id));
  filterStates.forEach((_, id) => updateFilterVisual(id));
  combFilterStates.forEach((_, id) => updateCombFilterVisual(id));
  multiplexerStates.forEach((_, id) => updateMultiplexerVisual(id));
  switchStates.forEach((_, id) => updateSwitchVisual(id));
  
  
  
  
  
  
}





function refreshControlGraph() {
  refreshAllDualModeWidgets();

  const sources = [];
  const push = (widgetId, portId, value) => sources.push({ widgetId, portId, value });
  constStates.forEach((state, id) => push(id, "value", state.value));
  knobStates.forEach((state, id) => {
    const layerIndex = state.activeLayer;
    knobLayer(state, layerIndex).controls.slice(0, state.count).forEach((control, index) => {
      const value = roundFloat(mapKnobValue(control));
      push(id, knobPortId(index, layerIndex), value);
      if (index === 0 && layerIndex === 0) push(id, "voltage", value);
    });
  });
  buttonStates.forEach((state, id) => {
    const layerIndex = state.activeLayer;
    buttonLayer(state, layerIndex).controls.slice(0, state.count).forEach((control, index) => {
      push(id, buttonPortId(index, layerIndex), buttonCurrentOutput(control));
    });
  });
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
    if (sourceType === "knob" && connection.from.portId.startsWith("voltage")) {
      const sourceState = knobStates.get(connection.from.widgetId);
      const parsed = parseKnobPortId(connection.from.portId);
      const control = parsed ? knobLayer(sourceState, parsed.layerIndex)?.controls[parsed.knobIndex] : null;
      if (control) value = mapKnobValue(control);
    } else if (sourceType === "const" && connection.from.portId === "value") {
      value = constStates.get(connection.from.widgetId)?.value ?? null;
    } else if (sourceType === "button") {
      const sourceState = buttonStates.get(connection.from.widgetId);
      const parsed = parseButtonPortId(connection.from.portId);
      const control = parsed ? buttonLayer(sourceState, parsed.layerIndex)?.controls[parsed.buttonIndex] : null;
      if (control) value = buttonCurrentOutput(control);
    } else if (sourceType === "up-down-button" && connection.from.portId === "value") {
      const sourceState = upDownButtonStates.get(connection.from.widgetId);
      if (sourceState) value = upDownButtonCurrentOutput(sourceState);
    }
    if (Number.isFinite(value)) {
      state.cvVolts = clamp(value, 0, CV_MAX_VOLTS);
      multiplexerSetActiveIndex(muxId, multiplexerSelectFromCv(state.cvVolts, state.inputCount));
    }
  });

  const derivedTypes = new Set(["multiplier", "adder", "divide", "frequency-divider", "abs", "value-mapper", "detune", "logic-and", "logic-or", "logic-xor", "logic-not"]);
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
      else if (type === "logic-and" || type === "logic-or" || type === "logic-xor" || type === "logic-not") updateLogicGateMode(id);
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

    if (isLinkedInputType(type)) {
      visited.add(widgetId);
      const linkId = linkedInputStates.get(widgetId)?.linkId;
      if (linkId) {
        linkedOutputStates.forEach((state, linkedOutputId) => {
          if (state.sourceLinkId === linkId && !visited.has(linkedOutputId)) {
            queue.push(linkedOutputId);
          }
        });
      }
      continue;
    }

    if (isLinkedOutputType(type)) {
      visited.add(widgetId);
      connections
        .filter((c) => c.from.widgetId === widgetId)
        .forEach((c) => {
          if (!visited.has(c.to.widgetId)) queue.push(c.to.widgetId);
        });
      continue;
    }

    const outputs = (WIDGET_TYPES[type] && WIDGET_TYPES[type].outputs) || [];
    const isBoundary = LINK_WIDGET_TYPES.includes(type) || (outputs.length === 0 && type !== "midi-split") || type === "soundfont-player";
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

  midiFilterStates.forEach((state, id) => {
    state.voices = desiredVoiceCounts.get(id) || 1;
    resizeVoiceSlots(id, state.voices);
  });

  document.querySelectorAll(".widget-instance").forEach((el) => {
    const widgetId = el.id;
    const type = el.dataset.widgetType;
    if (LINK_WIDGET_TYPES.includes(type) || LINKED_IO_WIDGET_TYPES.includes(type)) return;

    const currentCount = audioEngine.getVoiceCount(widgetId);
    if (currentCount === 0) return; 

    const desired = desiredVoiceCounts.get(widgetId) || 1;
    if (currentCount === desired) return;

    
    
    
    
    
    
    
    
    const engine = WIDGET_ENGINES[type];
    const params = engine ? engine.getParams(widgetId) : {};
    connections
      .filter((c) => c.from.widgetId === widgetId || c.to.widgetId === widgetId)
      .forEach((c) => linkedAudioRouteIds.delete(c.id));
    audioEngine.removeNode(widgetId);
    audioEngine.createNode(widgetId, type, params, desired);
    if (engine) engine.applyParams(widgetId, params);

    connections
      .filter((c) => c.from.widgetId === widgetId || c.to.widgetId === widgetId)
      .forEach(connectIfAudio);
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

  updateDualModeAudioNode(widgetId, "adder", state, inputPortIds, { inputCount: state.inputCount, values: state.values });

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



// Shared by all four logic gates (AND/OR/XOR/NOT) - keyed by gateType rather
// than four near-identical state maps, since they differ only in input
// count (AND/OR: configurable 2-8, see logicGateSettingsOverlay below;
// XOR: fixed 2; NOT: fixed 1) and in how logicGateValue() combines the
// inputs. Mirrors Adder/Multiplier's dual-mode (control-value vs real
// audio-node) pattern above.
const logicGateStates = new Map();
const LOGIC_GATE_MIN_INPUTS = 2;
const LOGIC_GATE_MAX_INPUTS = 8;

function logicGateInputCount(state) {
  if (state.gateType === "logic-not") return 1;
  if (state.gateType === "logic-xor") return 2;
  return state.inputCount;
}

// >0V reads as logical 1 (per spec) - mirrors the firmware's
// nodes::arithmetic::renderLogic*() functions (ArithmeticNodes.h) exactly,
// including AND's neutral-true/OR's neutral-false unconnected defaults
// (see updateLogicGateMode).
function logicGateValue(state) {
  if (!state) return 0;
  const values = state.values || [];
  switch (state.gateType) {
    case "logic-and": {
      for (let i = 0; i < state.inputCount; i += 1) {
        if (!((values[i] ?? 0) > 0)) return 0;
      }
      return CV_MAX_VOLTS;
    }
    case "logic-or": {
      for (let i = 0; i < state.inputCount; i += 1) {
        if ((values[i] ?? 0) > 0) return CV_MAX_VOLTS;
      }
      return 0;
    }
    case "logic-xor":
      return ((values[0] ?? 0) > 0) !== ((values[1] ?? 0) > 0) ? CV_MAX_VOLTS : 0;
    case "logic-not":
      return (values[0] ?? 0) > 0 ? 0 : CV_MAX_VOLTS;
    default:
      return 0;
  }
}

// Only AND/OR use this (configurable input count, dispatched in
// createWidget's port-render chain) - XOR/NOT have a fixed port shape and
// fall through to the generic `else` branch there, using their static
// WIDGET_TYPES definition directly.
function renderLogicGatePorts(widgetId, portsEl) {
  const state = logicGateStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  for (let i = 1; i <= state.inputCount; i += 1) {
    portsEl.appendChild(createPortRow(widgetId, { id: `in${i}`, label: `In ${i}` }, "in", state.gateType));
  }

  const outRow = createPortRow(widgetId, { id: "value", label: "" }, "out", state.gateType);
  outRow.classList.add("logic-gate-output-row");
  const readout = document.createElement("span");
  readout.className = "port-value";
  readout.dataset.role = "logic-gate-readout";
  readout.textContent = formatNumber(logicGateValue(state));
  outRow.appendChild(readout);
  portsEl.appendChild(outRow);
  layoutPortsGrid(portsEl);
}

function recomputeLogicGate(widgetId) {
  const state = logicGateStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const readout = el.querySelector('[data-role="logic-gate-readout"]');

  const value = logicGateValue(state);
  if (readout) readout.textContent = formatNumber(value);
  if (!state.audioMode) propagate(widgetId, "value", value);
}

function updateLogicGateMode(widgetId) {
  const state = logicGateStates.get(widgetId);
  if (!state) return;
  const count = logicGateInputCount(state);
  const unconnectedDefault = state.gateType === "logic-and" ? CV_MAX_VOLTS : 0;
  const inputPortIds = [];
  for (let i = 0; i < count; i += 1) {
    inputPortIds.push(`in${i + 1}`);
    if (!connections.some((c) => c.to.widgetId === widgetId && c.to.portId === `in${i + 1}`)) {
      state.values[i] = unconnectedDefault;
    }
  }

  const params = state.gateType === "logic-and" || state.gateType === "logic-or"
    ? { inputCount: state.inputCount }
    : undefined;
  updateDualModeAudioNode(widgetId, state.gateType, state, inputPortIds, params);

  recomputeLogicGate(widgetId);
}

const logicGateSettingsOverlay = document.getElementById("logicGateSettingsOverlay");
const logicGateCountInput = document.getElementById("logicGateCountInput");
let pendingLogicGateId = null;

function openLogicGateSettings(widgetId) {
  const state = logicGateStates.get(widgetId);
  if (!state) return;
  pendingLogicGateId = widgetId;
  logicGateCountInput.value = state.inputCount;
  logicGateSettingsOverlay.classList.remove("hidden");
}

function closeLogicGateSettings() {
  logicGateSettingsOverlay.classList.add("hidden");
  pendingLogicGateId = null;
}

document.getElementById("logicGateSettingsCancel").addEventListener("click", closeLogicGateSettings);

document.getElementById("logicGateSettingsSave").addEventListener("click", () => {
  if (!pendingLogicGateId) return;
  const state = logicGateStates.get(pendingLogicGateId);
  if (state) {
    let count = parseInt(logicGateCountInput.value, 10);
    if (Number.isNaN(count)) count = LOGIC_GATE_MIN_INPUTS;
    count = clamp(count, LOGIC_GATE_MIN_INPUTS, LOGIC_GATE_MAX_INPUTS);

    if (count < state.inputCount) {
      for (let i = count + 1; i <= state.inputCount; i += 1) {
        removeConnectionsForPort(pendingLogicGateId, `in${i}`, "in");
      }
    }
    state.inputCount = count;

    const el = document.getElementById(pendingLogicGateId);
    const portsEl = el ? el.querySelector(".widget-ports") : null;
    if (portsEl) renderLogicGatePorts(pendingLogicGateId, portsEl);

    recomputeLogicGate(pendingLogicGateId);
    drawConnections();
    savePresetNow();
  }
  closeLogicGateSettings();
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

  updateDualModeAudioNode(widgetId, "divide", state, ["in1", "in2"], { values: state.values });

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

// External Hardware (CV In/Out) - port/address/channel per instance, set via the settings
// modal from the live boot-time I2C scan (see openCvInSettings/openCvOutSettings below).
// port: -1 = never configured.
const cvInStates = new Map();
const cvOutStates = new Map();

// External Hardware (Digital In/Out) - which configured GPIO (Settings dialog -> GPIO) this
// widget talks to, set via the settings modal (see openDigitalInSettings/
// openDigitalOutSettings below). pin: -1 = never configured. lastLevel: last HIGH/LOW seen
// via the cvStatus broadcast, used to show a live level on the widget itself (updateGpioIoLabel).
// found: whether `pin` is still a configured GPIO in Settings -> GPIO as of the last cvStatus
// tick (~every 50ms, see main.cpp) - false once the user removes that pin from Settings while
// this widget still references it, matching the firmware's live re-check in
// DigitalInNode.h/DigitalOutNode.h's render() (no graph rebuild needed to notice the removal).
const digitalInStates = new Map();
const digitalOutStates = new Map();

function updateGpioIoLabel(widgetId) {
  const state = digitalInStates.get(widgetId) || digitalOutStates.get(widgetId);
  const el = document.getElementById(widgetId);
  const label = el?.querySelector('[data-role="gpio-io-label"]');
  if (!label || !state) return;
  if (state.pin < 0) {
    label.textContent = "Not configured";
    return;
  }
  if (state.found === false) {
    label.textContent = `GPIO${state.pin} - no longer configured`;
    return;
  }
  label.textContent = state.lastLevel === undefined || state.lastLevel === null
    ? `GPIO${state.pin}`
    : `GPIO${state.pin} - ${state.lastLevel ? "HIGH" : "LOW"}`;
}

const digitalInSettingsOverlay = document.getElementById("digitalInSettingsOverlay");
const digitalInPinSelect = document.getElementById("digitalInPinSelect");
let pendingDigitalInId = null;

async function openDigitalInSettings(widgetId) {
  const state = digitalInStates.get(widgetId);
  if (!state) return;
  pendingDigitalInId = widgetId;

  const info = await fetchGpioInfo();
  const pins = (Array.isArray(info.configured) ? info.configured : []).filter((e) => e.direction === "in");
  digitalInPinSelect.innerHTML = "";
  pins.forEach((e) => {
    const opt = document.createElement("option");
    opt.value = String(e.pin);
    opt.textContent = `GPIO${e.pin}${e.pull && e.pull !== "none" ? ` (${e.pull === "up" ? "pull-up" : "pull-down"})` : ""}`;
    digitalInPinSelect.appendChild(opt);
  });
  if (pins.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "No Digital Input pins configured";
    digitalInPinSelect.appendChild(opt);
  }
  if (state.pin >= 0 && [...digitalInPinSelect.options].some((o) => o.value === String(state.pin))) {
    digitalInPinSelect.value = String(state.pin);
  }
  digitalInSettingsOverlay.classList.remove("hidden");
}

function closeDigitalInSettings() {
  digitalInSettingsOverlay.classList.add("hidden");
  pendingDigitalInId = null;
}

document.getElementById("digitalInSettingsCancel").addEventListener("click", closeDigitalInSettings);

document.getElementById("digitalInSettingsSave").addEventListener("click", () => {
  if (!pendingDigitalInId) return;
  const state = digitalInStates.get(pendingDigitalInId);
  const pin = parseInt(digitalInPinSelect.value, 10);
  if (!state || !Number.isFinite(pin)) {
    showToast("Select a GPIO pin first");
    return;
  }
  state.pin = pin;
  updateGpioIoLabel(pendingDigitalInId);
  savePresetNow();
  closeDigitalInSettings();
});

const digitalOutSettingsOverlay = document.getElementById("digitalOutSettingsOverlay");
const digitalOutPinSelect = document.getElementById("digitalOutPinSelect");
let pendingDigitalOutId = null;

async function openDigitalOutSettings(widgetId) {
  const state = digitalOutStates.get(widgetId);
  if (!state) return;
  pendingDigitalOutId = widgetId;

  const info = await fetchGpioInfo();
  const pins = (Array.isArray(info.configured) ? info.configured : []).filter((e) => e.direction === "out");
  digitalOutPinSelect.innerHTML = "";
  pins.forEach((e) => {
    const opt = document.createElement("option");
    opt.value = String(e.pin);
    opt.textContent = `GPIO${e.pin}`;
    digitalOutPinSelect.appendChild(opt);
  });
  if (pins.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "No Digital Output pins configured";
    digitalOutPinSelect.appendChild(opt);
  }
  if (state.pin >= 0 && [...digitalOutPinSelect.options].some((o) => o.value === String(state.pin))) {
    digitalOutPinSelect.value = String(state.pin);
  }
  digitalOutSettingsOverlay.classList.remove("hidden");
}

function closeDigitalOutSettings() {
  digitalOutSettingsOverlay.classList.add("hidden");
  pendingDigitalOutId = null;
}

document.getElementById("digitalOutSettingsCancel").addEventListener("click", closeDigitalOutSettings);

document.getElementById("digitalOutSettingsSave").addEventListener("click", () => {
  if (!pendingDigitalOutId) return;
  const state = digitalOutStates.get(pendingDigitalOutId);
  const pin = parseInt(digitalOutPinSelect.value, 10);
  if (!state || !Number.isFinite(pin)) {
    showToast("Select a GPIO pin first");
    return;
  }

  // Uniqueness rule (Digital Out only, same reasoning as CV Out): two Digital Out widgets
  // driving the same physical pin is a real conflict.
  const conflict = [...digitalOutStates.entries()].some(
    ([wid, other]) => wid !== pendingDigitalOutId && other.pin === pin
  );
  if (conflict) {
    showToast(`GPIO${pin} is already used by another Digital Out`);
    return;
  }

  state.pin = pin;
  updateGpioIoLabel(pendingDigitalOutId);
  savePresetNow();
  closeDigitalOutSettings();
});

// RGB LED (External Hardware) - one LED of the WS2812 string. The input voltage maps through a
// stop list (sorted by volts) to a color: below the first / above the last stop that stop's
// color holds, between stop i and i+1 the color fades if stop i has `fade`, else stays hard.
// The firmware drives the real LED (nodes/RgbLedNode.h); the on-widget LED is computed here from
// the browser preview engine exactly like the voltmeter (deliverToWidget /
// tickDebugVoltmeterAudioSources) - rgbLedColorAt() mirrors the firmware's mapColor().
const rgbLedStates = new Map();
const RGB_LED_MAX_STOPS = 8;

function defaultRgbLedStops() {
  return [
    { v: 0, c: "#ff0000", fade: true },
    { v: 5, c: "#00ff00", fade: true },
    { v: 10, c: "#0000ff", fade: false },
  ];
}

function normalizeRgbLedStops(stops) {
  return stops
    .slice(0, RGB_LED_MAX_STOPS)
    .map((stop) => ({
      v: Number.isFinite(stop.v) ? stop.v : 0,
      c: /^#[0-9a-f]{6}$/i.test(stop.c) ? stop.c.toLowerCase() : "#000000",
      fade: stop.fade === true,
    }))
    .sort((a, b) => a.v - b.v);
}

function rgbLedColorAt(stops, volts) {
  if (stops.length === 0) return 0;
  const hex = (stop) => parseInt(stop.c.slice(1), 16);
  if (volts <= stops[0].v) return hex(stops[0]);
  const last = stops.length - 1;
  if (volts >= stops[last].v) return hex(stops[last]);
  let i = 0;
  while (i + 1 < last && volts >= stops[i + 1].v) ++i;
  const a = hex(stops[i]);
  if (!stops[i].fade) return a;
  const b = hex(stops[i + 1]);
  const span = stops[i + 1].v - stops[i].v;
  const t = span > 0 ? (volts - stops[i].v) / span : 0;
  let out = 0;
  for (const shift of [16, 8, 0]) {
    const ca = (a >> shift) & 0xff;
    const cb = (b >> shift) & 0xff;
    out |= Math.round(ca + (cb - ca) * t) << shift;
  }
  return out;
}

// One-time fetch of the LED string length, so each widget can flag an out-of-range LED
// position itself (red "!") without any recurring network traffic.
fetchLedStripInfo().then((info) => {
  if (info && !ledStripInfo) ledStripInfo = info;
  rgbLedStates.forEach((_, id) => updateRgbLedDisplay(id));
});

function rgbToCss(rgb) {
  return `#${(rgb >>> 0).toString(16).padStart(6, "0")}`;
}

function updateRgbLedDisplay(widgetId) {
  const state = rgbLedStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const lamp = el.querySelector('[data-role="rgb-led-lamp"]');
  const label = el.querySelector('[data-role="rgb-led-label"]');
  if (lamp) {
    const color = rgbToCss(state.lastRgb || 0);
    lamp.style.backgroundColor = color;
    lamp.style.boxShadow = state.lastRgb ? `0 0 12px 3px ${color}` : "none";
  }
  // "Not in string" is decided locally from the LED string settings (fetched once at page load,
  // see ledStripInfoReady) - no per-widget status comes over the network.
  const found = !ledStripInfo || state.ledIndex < ledStripInfo.activeCount;
  if (label) {
    label.textContent = `LED ${state.ledIndex + 1}`;
    if (!found) {
      const warn = document.createElement("span");
      warn.className = "rgb-led-warn";
      warn.textContent = " !";
      label.appendChild(warn);
    }
    label.title = !found ? "This LED position is not in the configured LED string" : "";
  }
}

const rgbLedSettingsOverlay = document.getElementById("rgbLedSettingsOverlay");
const rgbLedIndexInput = document.getElementById("rgbLedIndexInput");
const rgbLedIndexHint = document.getElementById("rgbLedIndexHint");
const rgbLedModeSelect = document.getElementById("rgbLedModeSelect");
const rgbLedStopList = document.getElementById("rgbLedStopList");
const rgbLedAddStop = document.getElementById("rgbLedAddStop");
const rgbLedPreview = document.getElementById("rgbLedPreview");
let pendingRgbLedId = null;
let pendingRgbLedStops = [];
let pendingRgbLedCount = null; // configured string length, null = unknown (settings unreachable)

function rgbLedModeOf(stops) {
  // The last stop's fade flag has no segment to act on - ignore it.
  const flags = stops.slice(0, -1).map((stop) => stop.fade);
  if (flags.length === 0 || flags.every(Boolean)) return "gradient";
  if (flags.every((f) => !f)) return "steps";
  return "free";
}

function renderRgbLedPreview() {
  const stops = normalizeRgbLedStops(pendingRgbLedStops);
  const parts = [];
  for (let step = 0; step <= 100; ++step) {
    parts.push(`${rgbToCss(rgbLedColorAt(stops, step / 10))} ${step}%`);
  }
  rgbLedPreview.style.background = `linear-gradient(to right, ${parts.join(", ")})`;
}

function renderRgbLedStops() {
  const free = rgbLedModeSelect.value === "free";
  rgbLedStopList.replaceChildren();
  pendingRgbLedStops.forEach((stop, idx) => {
    const row = document.createElement("div");
    row.className = "rgb-led-stop-row";

    const volts = document.createElement("input");
    volts.type = "number";
    volts.step = "0.01";
    volts.min = "-10";
    volts.max = "10";
    volts.value = String(stop.v);
    volts.title = "Voltage";
    volts.addEventListener("input", () => {
      const v = parseFloat(volts.value);
      if (Number.isFinite(v)) stop.v = v;
      renderRgbLedPreview();
    });
    const unit = document.createElement("span");
    unit.textContent = "V";

    const color = document.createElement("input");
    color.type = "color";
    color.value = stop.c;
    color.addEventListener("input", () => {
      stop.c = color.value;
      renderRgbLedPreview();
    });

    const fadeLabel = document.createElement("label");
    fadeLabel.className = "rgb-led-stop-fade";
    fadeLabel.hidden = !free;
    const fade = document.createElement("input");
    fade.type = "checkbox";
    fade.checked = stop.fade;
    fade.addEventListener("change", () => {
      stop.fade = fade.checked;
      renderRgbLedPreview();
    });
    fadeLabel.appendChild(fade);
    fadeLabel.appendChild(document.createTextNode(" Fade to next"));

    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "btn-secondary";
    remove.textContent = "✕";
    remove.title = "Remove stop";
    remove.disabled = pendingRgbLedStops.length <= 1;
    remove.addEventListener("click", () => {
      pendingRgbLedStops.splice(idx, 1);
      renderRgbLedStops();
    });

    row.append(volts, unit, color, fadeLabel, remove);
    rgbLedStopList.appendChild(row);
  });
  rgbLedAddStop.disabled = pendingRgbLedStops.length >= RGB_LED_MAX_STOPS;
  renderRgbLedPreview();
}

rgbLedModeSelect.addEventListener("change", () => {
  const mode = rgbLedModeSelect.value;
  if (mode !== "free") pendingRgbLedStops.forEach((stop) => { stop.fade = mode === "gradient"; });
  renderRgbLedStops();
});

rgbLedAddStop.addEventListener("click", () => {
  if (pendingRgbLedStops.length >= RGB_LED_MAX_STOPS) return;
  const last = pendingRgbLedStops[pendingRgbLedStops.length - 1];
  pendingRgbLedStops.push({
    v: last ? Math.min(10, last.v + 1) : 0,
    c: "#ffffff",
    fade: rgbLedModeSelect.value === "gradient",
  });
  renderRgbLedStops();
});

async function openRgbLedSettings(widgetId) {
  const state = rgbLedStates.get(widgetId);
  if (!state) return;
  pendingRgbLedId = widgetId;
  pendingRgbLedStops = state.stops.map((stop) => ({ ...stop }));

  const info = await fetchLedStripInfo();
  pendingRgbLedCount = info ? info.count : null;
  if (pendingRgbLedCount === 0) {
    rgbLedIndexInput.disabled = true;
    rgbLedIndexInput.removeAttribute("max");
    rgbLedIndexHint.textContent = "No LED string defined in Settings - set its pin and LED count there first.";
  } else {
    rgbLedIndexInput.disabled = false;
    if (pendingRgbLedCount === null) rgbLedIndexInput.removeAttribute("max");
    else rgbLedIndexInput.max = String(pendingRgbLedCount);
    rgbLedIndexHint.textContent = pendingRgbLedCount === null
      ? "Could not read the LED string settings."
      : `The string has ${pendingRgbLedCount} LEDs (1-${pendingRgbLedCount}).`;
  }
  rgbLedIndexInput.value = String(state.ledIndex + 1);
  rgbLedModeSelect.value = rgbLedModeOf(pendingRgbLedStops);
  renderRgbLedStops();
  rgbLedSettingsOverlay.classList.remove("hidden");
}

function closeRgbLedSettings() {
  rgbLedSettingsOverlay.classList.add("hidden");
  pendingRgbLedId = null;
}

document.getElementById("rgbLedSettingsCancel").addEventListener("click", closeRgbLedSettings);

document.getElementById("rgbLedSettingsSave").addEventListener("click", () => {
  if (!pendingRgbLedId) return;
  const state = rgbLedStates.get(pendingRgbLedId);
  if (!state) return;
  if (!rgbLedIndexInput.disabled) {
    const position = parseInt(rgbLedIndexInput.value, 10);
    const max = pendingRgbLedCount ?? 256;
    if (!Number.isInteger(position) || position < 1 || position > max) {
      showToast(`LED position must be between 1 and ${max}`);
      return;
    }
    state.ledIndex = position - 1;
  }
  state.stops = normalizeRgbLedStops(pendingRgbLedStops);
  updateRgbLedDisplay(pendingRgbLedId);
  savePresetNow();
  closeRgbLedSettings();
});

async function fetchI2cDevices() {
  try {
    const res = await fetch("/api/system/i2c");
    if (!res.ok) return [];
    const data = await res.json();
    return Array.isArray(data.devices) ? data.devices : [];
  } catch (err) {
    return [];
  }
}

function updateCvIoLabel(widgetId) {
  const state = cvInStates.get(widgetId) || cvOutStates.get(widgetId);
  const el = document.getElementById(widgetId);
  const label = el?.querySelector('[data-role="cv-io-label"]');
  if (!label || !state) return;
  label.textContent =
    state.port >= 0
      ? `I2C${state.port} @ 0x${state.address.toString(16).toUpperCase()} ch${state.channel + 1}`
      : "Not configured";
}

const cvInSettingsOverlay = document.getElementById("cvInSettingsOverlay");
const cvInDeviceSelect = document.getElementById("cvInDeviceSelect");
const cvInChannelSelect = document.getElementById("cvInChannelSelect");
let pendingCvInId = null;

async function openCvInSettings(widgetId) {
  const state = cvInStates.get(widgetId);
  if (!state) return;
  pendingCvInId = widgetId;

  const devices = (await fetchI2cDevices()).filter((d) => d.name === "ADS1115");
  cvInDeviceSelect.innerHTML = "";
  devices.forEach((d) => {
    const opt = document.createElement("option");
    opt.value = `${d.bus}:${d.address}`;
    opt.textContent = `I2C${d.bus} @ ${d.address}`;
    cvInDeviceSelect.appendChild(opt);
  });
  if (devices.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "No ADS1115 found";
    cvInDeviceSelect.appendChild(opt);
  }
  const currentKey = state.port >= 0 ? `${state.port}:0x${state.address.toString(16).padStart(2, "0").toUpperCase()}` : "";
  if ([...cvInDeviceSelect.options].some((o) => o.value === currentKey)) cvInDeviceSelect.value = currentKey;
  cvInChannelSelect.value = String(state.channel || 0);
  cvInSettingsOverlay.classList.remove("hidden");
}

function closeCvInSettings() {
  cvInSettingsOverlay.classList.add("hidden");
  pendingCvInId = null;
}

document.getElementById("cvInSettingsCancel").addEventListener("click", closeCvInSettings);

document.getElementById("cvInSettingsSave").addEventListener("click", () => {
  if (!pendingCvInId) return;
  const state = cvInStates.get(pendingCvInId);
  const [portStr, addressStr] = (cvInDeviceSelect.value || "").split(":");
  if (!state || portStr === undefined || portStr === "" || !addressStr) {
    showToast("Select an ADS1115 device first");
    return;
  }
  state.port = parseInt(portStr, 10);
  state.address = parseInt(addressStr, 16);
  state.channel = parseInt(cvInChannelSelect.value, 10) || 0;
  updateCvIoLabel(pendingCvInId);
  savePresetNow();
  closeCvInSettings();
});

const cvOutSettingsOverlay = document.getElementById("cvOutSettingsOverlay");
const cvOutDeviceSelect = document.getElementById("cvOutDeviceSelect");
const cvOutChannelSelect = document.getElementById("cvOutChannelSelect");
let pendingCvOutId = null;

async function openCvOutSettings(widgetId) {
  const state = cvOutStates.get(widgetId);
  if (!state) return;
  pendingCvOutId = widgetId;

  const devices = (await fetchI2cDevices()).filter((d) => d.name === "GP8413");
  cvOutDeviceSelect.innerHTML = "";
  devices.forEach((d) => {
    const opt = document.createElement("option");
    opt.value = `${d.bus}:${d.address}`;
    opt.textContent = `I2C${d.bus} @ ${d.address}`;
    cvOutDeviceSelect.appendChild(opt);
  });
  if (devices.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "No GP8413 found";
    cvOutDeviceSelect.appendChild(opt);
  }
  const currentKey = state.port >= 0 ? `${state.port}:0x${state.address.toString(16).padStart(2, "0").toUpperCase()}` : "";
  if ([...cvOutDeviceSelect.options].some((o) => o.value === currentKey)) cvOutDeviceSelect.value = currentKey;
  cvOutChannelSelect.value = String(state.channel || 0);
  cvOutSettingsOverlay.classList.remove("hidden");
}

function closeCvOutSettings() {
  cvOutSettingsOverlay.classList.add("hidden");
  pendingCvOutId = null;
}

document.getElementById("cvOutSettingsCancel").addEventListener("click", closeCvOutSettings);

document.getElementById("cvOutSettingsSave").addEventListener("click", () => {
  if (!pendingCvOutId) return;
  const state = cvOutStates.get(pendingCvOutId);
  const [portStr, addressStr] = (cvOutDeviceSelect.value || "").split(":");
  if (!state || portStr === undefined || portStr === "" || !addressStr) {
    showToast("Select a GP8413 device first");
    return;
  }
  const port = parseInt(portStr, 10);
  const address = parseInt(addressStr, 16);
  const channel = parseInt(cvOutChannelSelect.value, 10) || 0;

  // Uniqueness rule (CV Out only, per user decision - reading the same ADC channel twice via
  // two CV In widgets is harmless, but two CV Out widgets writing the same DAC channel is a
  // real conflict - only one device can drive that physical output). Enforced client-side
  // only: compare config VALUES across every other existing CV Out instance, not widget type
  // (contrast with OUTPUT_SINGLETON_TYPES, which blocks by type, not by config).
  const conflict = [...cvOutStates.entries()].some(
    ([wid, other]) => wid !== pendingCvOutId && other.port === port && other.address === address && other.channel === channel
  );
  if (conflict) {
    showToast(`I2C${port} @ 0x${address.toString(16).toUpperCase()} channel ${channel + 1} is already used by another CV Out`);
    return;
  }

  state.port = port;
  state.address = address;
  state.channel = channel;
  updateCvIoLabel(pendingCvOutId);
  savePresetNow();
  closeCvOutSettings();
});

function updateCvStatusDisplay(entries) {
  if (!Array.isArray(entries)) return;
  const missingIds = new Set(entries.filter((e) => e && e[1] === false).map((e) => e[0]));
  document
    .querySelectorAll(
      '.widget-instance[data-widget-type="cv-in"], .widget-instance[data-widget-type="cv-out"], ' +
      '.widget-instance[data-widget-type="digital-in"], .widget-instance[data-widget-type="digital-out"]'
    )
    .forEach((el) => {
      const badge = el.querySelector('[data-role="cv-missing"]');
      if (badge && !missingIds.has(el.id)) badge.remove();
    });
  // Refresh every Digital In/Out widget's own "no longer configured" label (not just the small
  // ❗ badge above) - every configured widget is covered by this tick regardless of whether it
  // appears in `entries`: Digital In is always sent (see SynthEngine.h's cvStatus scan), Digital
  // Out only when missing, so "not in entries" reliably means "still configured" for both.
  for (const [widgetId, state] of digitalInStates) {
    state.found = !missingIds.has(widgetId);
    updateGpioIoLabel(widgetId);
  }
  for (const [widgetId, state] of digitalOutStates) {
    state.found = !missingIds.has(widgetId);
    updateGpioIoLabel(widgetId);
  }
  missingIds.forEach((widgetId) => {
    const el = document.getElementById(widgetId);
    const header = el ? el.querySelector(".widget-instance-header") : null;
    if (!header || header.querySelector('[data-role="cv-missing"]')) return;
    const badge = document.createElement("span");
    badge.className = "widget-device-missing-badge";
    badge.dataset.role = "cv-missing";
    badge.textContent = "❗";
    badge.title = "Device not responding";
    const closeBtn = header.querySelector(".widget-instance-close");
    if (closeBtn) header.insertBefore(badge, closeBtn);
    else header.appendChild(badge);
  });
  entries.forEach((entry) => {
    if (!entry || typeof entry[0] !== "string" || !Number.isFinite(entry[2])) return;
    // Digital In's live level shows directly on the widget's own label (unlike CV In, which
    // has no such per-widget readout) - entry[2] carries 0/CV_MAX_VOLTS = LOW/HIGH, same
    // wire format CV In's volts already use (see SynthEngine.h's cvStatus scan).
    const digitalInState = digitalInStates.get(entry[0]);
    if (digitalInState) {
      digitalInState.lastLevel = entry[2] >= CV_MAX_VOLTS * 0.5;
      updateGpioIoLabel(entry[0]);
    }
    connections
      .filter((connection) => connection.from.widgetId === entry[0] && connection.from.portId === "value")
      .forEach((connection) => {
        const target = document.getElementById(connection.to.widgetId);
        if (target && ["debug", "voltmeter", "rgb-led"].includes(target.dataset.widgetType)) {
          withDeviceSyncMuted(() => deliverToWidget(connection.to.widgetId, connection.to.portId, entry[2]));
        }
      });
  });
}

// Device-wide Master Clock (MasterClock.h on the firmware side) - every
// Clock widget's pulse just taps this shared tempo, so they all stay in
// sync; the browser mirrors it for display/preview only, the firmware is
// authoritative (see the periodic "masterClock" WS broadcast handled by
// applyMasterClockBroadcast() below).
let masterClockBpm = 120;
let masterClockMaxBpm = 240;
let masterClockRoundBpm = false;
let masterClockCc = null;
let masterClockChannel = null;
let masterClockLearnActive = false;

function updateMasterClockBadge() {
  const label = document.getElementById("masterClockBpmLabel");
  if (label) label.textContent = formatNumber(masterClockBpm);
}

function updateMasterClockLearnUi() {
  const ccDisplay = document.getElementById("masterClockCcDisplay");
  if (ccDisplay) {
    ccDisplay.value = masterClockCc === null ? "" : `${masterClockCc} (Ch ${masterClockChannel + 1})`;
  }
  const learnButton = document.getElementById("masterClockLearnButton");
  if (learnButton && !masterClockLearnActive) learnButton.textContent = "Learn";
}

function restartAllClockTimers() {
  clockStates.forEach((_, widgetId) => restartClockTimer(widgetId));
}

function setMasterClockBpm(bpm) {
  let next = Math.max(0, Number(bpm) || 0);
  if (masterClockRoundBpm) next = Math.round(next);
  masterClockBpm = next;
  updateMasterClockBadge();
  restartAllClockTimers();
}

// Mirrors the periodic "masterClock" WS broadcast (synth_web_api.hpp's
// broadcast_status_tick()) - keeps this tab (and every other open tab) in
// sync with the firmware's authoritative state, including a Learn/Save done
// from a different tab.
function applyMasterClockBroadcast(data) {
  masterClockMaxBpm = Number(data.maxBpm) || 240;
  masterClockRoundBpm = data.round === true;
  masterClockCc = typeof data.cc === "number" && data.cc >= 0 ? data.cc : null;
  masterClockChannel = typeof data.channel === "number" && data.channel >= 0 ? data.channel : null;
  setMasterClockBpm(data.bpm);
  updateMasterClockLearnUi();
}

// Same "waiting for the next CC" pattern as routeMidiToKnobs/Buttons/
// Switches, but for the single device-wide Master Clock rather than a
// per-widget target.
function routeMidiToMasterClock(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;
  if (masterClockLearnActive) {
    masterClockCc = data.control;
    masterClockChannel = data.channel;
    masterClockLearnActive = false;
    updateMasterClockLearnUi();
    sendLiveParamUpdate("@masterClock", { cc: masterClockCc, channel: masterClockChannel });
    return;
  }
  if (masterClockCc === data.control && masterClockChannel === data.channel) {
    setMasterClockBpm((data.value / 127) * masterClockMaxBpm);
  }
}

const masterClockBadge = document.getElementById("masterClockBadge");
const masterClockSettingsOverlay = document.getElementById("masterClockSettingsOverlay");
const masterClockBpmInput = document.getElementById("masterClockBpmInput");
const masterClockMaxBpmInput = document.getElementById("masterClockMaxBpmInput");
const masterClockRoundBpmInput = document.getElementById("masterClockRoundBpmInput");
const masterClockLearnButton = document.getElementById("masterClockLearnButton");

function openMasterClockSettings() {
  masterClockBpmInput.value = String(masterClockBpm);
  masterClockMaxBpmInput.value = String(masterClockMaxBpm);
  masterClockRoundBpmInput.checked = masterClockRoundBpm;
  updateMasterClockLearnUi();
  masterClockSettingsOverlay.classList.remove("hidden");
}

function closeMasterClockSettings() {
  masterClockSettingsOverlay.classList.add("hidden");
  masterClockLearnActive = false;
  updateMasterClockLearnUi();
}

masterClockBadge?.addEventListener("click", openMasterClockSettings);
masterClockSettingsOverlay?.addEventListener("click", (e) => {
  if (e.target === masterClockSettingsOverlay) closeMasterClockSettings();
});
document.getElementById("masterClockSettingsCancel")?.addEventListener("click", closeMasterClockSettings);
masterClockLearnButton?.addEventListener("click", () => {
  masterClockLearnActive = true;
  masterClockLearnButton.textContent = "Waiting…";
});
document.getElementById("masterClockSettingsSave")?.addEventListener("click", () => {
  masterClockRoundBpm = masterClockRoundBpmInput.checked;
  masterClockMaxBpm = Math.max(1, Number(masterClockMaxBpmInput.value) || 240);
  setMasterClockBpm(masterClockBpmInput.value);
  sendLiveParamUpdate("@masterClock", {
    bpm: masterClockBpm,
    maxBpm: masterClockMaxBpm,
    round: masterClockRoundBpm,
  });
  closeMasterClockSettings();
});

// Preset-navigation MIDI learn (Settings dialog): device-global like Master
// Clock above, but the actual edge-triggered preset step runs natively on
// the firmware (see synth_manager.hpp's handleDeviceSettingsMidi()) so it
// keeps working with no browser tab open - this side only needs to capture
// the learned CC while "Waiting…" and mirror it for display.
let presetPrevCc = null;
let presetPrevChannel = null;
let presetNextCc = null;
let presetNextChannel = null;
let presetPrevLearnActive = false;
let presetNextLearnActive = false;

const presetPrevCcDisplay = document.getElementById("presetPrevCcDisplay");
const presetNextCcDisplay = document.getElementById("presetNextCcDisplay");
const presetPrevLearnBtn = document.getElementById("presetPrevLearnBtn");
const presetNextLearnBtn = document.getElementById("presetNextLearnBtn");

function updatePresetNavLearnUi() {
  if (presetPrevCcDisplay) presetPrevCcDisplay.value = presetPrevCc === null ? "" : `${presetPrevCc} (Ch ${presetPrevChannel + 1})`;
  if (presetNextCcDisplay) presetNextCcDisplay.value = presetNextCc === null ? "" : `${presetNextCc} (Ch ${presetNextChannel + 1})`;
  if (presetPrevLearnBtn && !presetPrevLearnActive) presetPrevLearnBtn.textContent = "Learn";
  if (presetNextLearnBtn && !presetNextLearnActive) presetNextLearnBtn.textContent = "Learn";
}

function routeMidiToPresetNav(data) {
  if (data.type !== "midi" || data.midiType !== "control_change") return;
  if (presetPrevLearnActive) {
    presetPrevCc = data.control;
    presetPrevChannel = data.channel;
    presetPrevLearnActive = false;
    updatePresetNavLearnUi();
    sendLiveParamUpdate("@deviceSettings", { prevCc: presetPrevCc, prevChannel: presetPrevChannel });
  } else if (presetNextLearnActive) {
    presetNextCc = data.control;
    presetNextChannel = data.channel;
    presetNextLearnActive = false;
    updatePresetNavLearnUi();
    sendLiveParamUpdate("@deviceSettings", { nextCc: presetNextCc, nextChannel: presetNextChannel });
  }
}

presetPrevLearnBtn?.addEventListener("click", () => {
  presetPrevLearnActive = true;
  presetNextLearnActive = false;
  presetPrevLearnBtn.textContent = "Waiting…";
  updatePresetNavLearnUi();
});
presetNextLearnBtn?.addEventListener("click", () => {
  presetNextLearnActive = true;
  presetPrevLearnActive = false;
  presetNextLearnBtn.textContent = "Waiting…";
  updatePresetNavLearnUi();
});

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
  if (bpmLabel) bpmLabel.textContent = `${formatNumber(masterClockBpm)} BPM`;
}

// "frequency" output: same 0-maxBpm-to-0-10V scale the Master Clock dialog's
// own MIDI-CC learn already uses (masterClockMaxBpm), just the other
// direction - mirrors nodes/ClockNode.h's render() exactly.
function propagateClockFrequency(widgetId) {
  const frequencyVolts = masterClockMaxBpm > 0 ? clamp((masterClockBpm / masterClockMaxBpm) * CV_MAX_VOLTS, 0, CV_MAX_VOLTS) : 0;
  propagate(widgetId, "frequency", frequencyVolts);
}

function fireClockPulse(widgetId) {
  const state = clockStates.get(widgetId);
  if (!state) return;

  propagate(widgetId, "pulse", GATE_ON_VOLTS);
  const el = document.getElementById(widgetId);
  // Blink the real "Pulse" output port dot itself, not a separate decorative
  // one - same as every other widget's output LED.
  const dot = el ? el.querySelector('[data-port-id="pulse"][data-port-dir="out"]') : null;
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
  propagateClockFrequency(widgetId);
  if (masterClockBpm <= 0) return;

  const intervalMs = 60000 / masterClockBpm;
  state.tickTimer = setInterval(() => fireClockPulse(widgetId), intervalMs);
}



const lfoStates = new Map();
const LFO_TICK_MS = 30;
const LFO_MAX_RATE_HZ = 500;
const lfoSettingsOverlay = document.getElementById("lfoSettingsOverlay");
const lfoMaxRateInput = document.getElementById("lfoMaxRateInput");
let pendingLfoId = null;

const LFO_SLIDER_CONFIG = {
  rate: { min: 0, max: LFO_MAX_RATE_HZ, step: 0.01 },
  gain: { min: 0, max: 10, step: 0.01 },
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
  if (portId === "gain") return `${formatNumber(value)} V`;
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
    case "sawtooth-down":
      return 1 - 2 * (phase - Math.floor(phase));
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

  
  
  
  
  
  
  const halfSwing = state.gain / 2;
  const center = state.zeroPointFixed ? state.zeroPoint : halfSwing;
  
  
  
  
  const phaseOffset = state.phase / 360;

  let raw;
  if (state.wavetable) {
    // Same cycle/one-shot bookkeeping as the classic shapes below, only the
    // value comes from the morphed wavetable bank.
    if (!(state.oneShot && state.oneShotDone)) {
      state.cyclePos = wrapPhase(state.cyclePos + state.rate * dt);
      if (state.oneShot) {
        state.oneShotAccum += state.rate * dt;
        if (state.oneShotAccum >= 1) {
          state.oneShotDone = true;
          state.oneShotAccum = 0;
          state.cyclePos = 0;
        }
      }
    }
    raw = sampleBank(lfoWavetableBank(state), wrapPhase(state.cyclePos + phaseOffset), state.morph / CV_MAX_VOLTS);
  } else if (state.waveType === "linear-up" || state.waveType === "linear-down") {
    // Free-running: repeats like a sawtooth. One Shot: one non-wrapping ramp
    // from the last reset, then hold at the end value.
    if (!(state.oneShot && state.oneShotDone)) {
      state.linearPos += state.rate * dt;
      if (state.oneShot) {
        state.linearPos = Math.min(1, state.linearPos);
        state.oneShotAccum += state.rate * dt;
        if (state.oneShotAccum >= 1) {
          state.oneShotDone = true;
          state.oneShotAccum = 0;
        }
      } else {
        state.linearPos = wrapPhase(state.linearPos);
      }
    }
    const pos = state.oneShot ? clamp(state.linearPos + phaseOffset, 0, 1) : wrapPhase(state.linearPos + phaseOffset);
    raw = state.waveType === "linear-up" ? 2 * pos - 1 : 1 - 2 * pos;
  } else if (state.oneShot && state.oneShotDone) {
    
    
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

  if (state.wavetable) updateLfoWavetablePreview(widgetId);

  const value = clamp(center + raw * halfSwing, 0, CV_MAX_VOLTS);
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

// --- Wavetable widget ----------------------------------------------------

function lfoWavetableBank(state) {
  if (!state.wavetables) state.wavetables = createDefaultBank();
  return state.wavetables;
}

function updateLfoWavetablePreview(widgetId) {
  const state = lfoStates.get(widgetId);
  const canvas = document.getElementById(widgetId)?.querySelector('[data-role="lfo-wavetable-preview"]');
  if (!state || !canvas || !state.wavetable) return;
  // Playhead: the table read position (cycle position + phase offset), same
  // value tickLfo() feeds into sampleBank().
  drawWavetablePreview(canvas, lfoWavetableBank(state), state.morph, wrapPhase(state.cyclePos + state.phase / 360));
}

// Live audition of editor changes on the device: POST the whole bank to
// /api/wavetable (swapped into the running graph, no preset save / graph
// rebuild). At most one request in flight; edits arriving meanwhile only
// keep the newest bank, sent as soon as the previous request returns.
const lfoWavetableLiveSend = new Map(); // widgetId -> { inFlight, pending }
const WAVETABLE_LIVE_MIN_GAP_MS = 80;

function postLfoWavetableLive(widgetId, tables) {
  let entry = lfoWavetableLiveSend.get(widgetId);
  if (!entry) {
    entry = { inFlight: false, pending: null };
    lfoWavetableLiveSend.set(widgetId, entry);
  }
  entry.pending = tables;
  if (entry.inFlight) return;
  const sendNext = () => {
    const next = entry.pending;
    entry.pending = null;
    if (!next) {
      entry.inFlight = false;
      return;
    }
    entry.inFlight = true;
    // While the device's web server is busy receiving/parsing the bank, its
    // periodic WS status broadcasts arrive late - keep the connection
    // watchdog from mistaking that for a lost device (see
    // suspendWsWatchdog()), for the whole stroke plus a grace period.
    suspendWsWatchdog(WS_WATCHDOG_BUSY_GRACE_MS);
    fetch(`/api/wavetable?widget=${encodeURIComponent(widgetId)}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ wavetables: encodeBank(next) }),
    })
      .catch(() => {}) // no device (offline editing) - the browser preview still plays
      .finally(() => {
        suspendWsWatchdog(WS_WATCHDOG_BUSY_GRACE_MS);
        // Short breather between requests so the device's single httpd
        // task also gets to its WS traffic in between.
        setTimeout(sendNext, WAVETABLE_LIVE_MIN_GAP_MS);
      });
  };
  sendNext();
}

function setLfoMorph(widgetId, volts) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.morph = clamp(Number(volts) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, "morph", state.morph);
  const el = document.getElementById(widgetId);
  const slider = el?.querySelector('[data-role="lfo-morph-slider"]');
  if (slider) slider.value = String(state.morph);
  const value = el?.querySelector('[data-role="lfo-morph-value"]');
  if (value) value.textContent = `${formatNumber(state.morph)} V`;
  updateLfoWavetablePreview(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { morph: state.morph });
}

function openLfoWavetableEditor(widgetId) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  const original = lfoWavetableBank(state).map((t) => Float32Array.from(t));
  const label = document.getElementById(widgetId)?.querySelector(".widget-instance-label")?.textContent || "LFO";
  const preview = (tables, live = true) => {
    state.wavetables = tables;
    audioEngine.setParam(widgetId, "wavetables", tables);
    updateLfoWavetablePreview(widgetId);
    if (live && state.wavetable) postLfoWavetableLive(widgetId, tables);
  };
  openWavetableEditor({
    title: `Wavetable Editor – ${label}`,
    tables: original,
    onPreview: preview,
    onCancel: () => preview(original),
    onApply: (tables) => {
      // Undo snapshot of the pre-edit bank (the live previews already
      // replaced it in state).
      preview(original, false);
      pushHistorySnapshot();
      // No preset save here (a save rebuilds the whole graph on the device
      // and takes too long): the device already plays the edited bank via
      // the live path - this final post just makes sure it has the very
      // last state. It is written to flash with the next regular preset
      // save (serializePreset() includes state.wavetables).
      preview(tables, true);
    },
  });
}

function setLfoWaveType(widgetId, waveType) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.waveType = waveType;
  state.linearPos = 0;
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
  // Auto mode computes the offset itself (half amplitude) - the manual
  // Offset slider has nothing to do in that mode.
  const slider = el.querySelector("[data-role='lfo-zeropoint-port-slider']");
  if (slider) slider.disabled = !state.zeroPointFixed;
}

function setLfoZeroPointMode(widgetId, fixed) {
  const state = lfoStates.get(widgetId);
  if (!state) return;
  state.zeroPointFixed = fixed;
  audioEngine.setParam(widgetId, "zeroPointFixed", fixed);
  updateLfoZeroModeButtons(widgetId);
  sendLiveParamUpdate(widgetId, { zeroPointFixed: fixed });
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

  ["rate", "gain", "phase"].forEach((portId) => {
    const slider = el.querySelector(`[data-role="lfo-${portId}-slider"]`);
    if (slider) slider.value = String(state[portId]);
    const valueSpan = el.querySelector(`[data-role="lfo-${portId}-value"]`);
    if (valueSpan) valueSpan.textContent = formatLfoValue(portId, state[portId]);
  });

  updateLfoZeroModeButtons(widgetId);
  const zpPortSlider = el.querySelector('[data-role="lfo-zeropoint-port-slider"]');
  if (zpPortSlider) zpPortSlider.value = String(state.zeroPoint);
  const zpPortValue = el.querySelector('[data-role="lfo-zeropoint-port-value"]');
  if (zpPortValue) zpPortValue.textContent = `${formatNumber(state.zeroPoint)} V`;
  const oneShotToggle = el.querySelector('[data-role="lfo-oneshot-switch"]');
  if (oneShotToggle) oneShotToggle.checked = state.oneShot === true;
  const globalToggle = el.querySelector('[data-role="lfo-global-switch"]');
  if (globalToggle) globalToggle.checked = state.global === true;
  const morphSlider = el.querySelector('[data-role="lfo-morph-slider"]');
  if (morphSlider) morphSlider.value = String(state.morph);
  const morphValue = el.querySelector('[data-role="lfo-morph-value"]');
  if (morphValue) morphValue.textContent = `${formatNumber(state.morph)} V`;
  updateLfoWavetablePreview(widgetId);
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
    const state = oscilloscopeStates.get(el.id) || createOscilloscopeState();
    drawOscilloscope(canvas, waveform1, waveform2, cvMode, state);
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
  document.querySelectorAll('.widget-instance[data-widget-type="filter"], .widget-instance[data-widget-type="curtis-filter"], .widget-instance[data-widget-type="comb-filter"]').forEach((el) => {
    const canvas = el.querySelector('[data-role="filter-spectrum-canvas"]');
    if (!canvas) return;
    const spectrum = audioEngine.getFilterSpectrum(el.id);
    const response = audioEngine.getFilterFrequencyResponse(el.id);
    if (!spectrum || !response) return;
    if (el.dataset.widgetType === "comb-filter") {
      drawGraphicEqPreview(canvas, spectrum, response, [], audioEngine.ctx.sampleRate);
    } else {
      drawSpectrum(canvas, spectrum);
      drawEqCurve(canvas, response);
    }
  });
  document.querySelectorAll('.widget-instance[data-widget-type="graphic-eq"]').forEach((el) => {
    const canvas = el.querySelector('[data-role="graphic-eq-spectrum-canvas"]');
    if (!canvas) return;
    const spectrum = audioEngine.getGraphicEqSpectrum(el.id);
    const response = audioEngine.getGraphicEqFrequencyResponse(el.id);
    const state = graphicEqStates.get(el.id);
    if (!spectrum || !response || !state) return;
    drawGraphicEqPreview(canvas, spectrum, response, state.frequencies, audioEngine.ctx?.sampleRate || 44100);
  });
  document.querySelectorAll('.widget-instance[data-widget-type="midi-in"]').forEach((el) => {
    updateMidiInVoiceSlotsLabel(el.id);
  });
  if (oscilloscopesVisible) {
    document
      .querySelectorAll(OSCILLOSCOPE_WIDGET_TYPES.map((t) => `.widget-instance[data-widget-type="${t}"]`).join(", "))
      .forEach((el) => {
        const canvas = el.querySelector('[data-role="osc-scope-canvas"]');
        if (!canvas) return;
        const waveform = audioEngine.getOscilloscopeWaveform(el.id, canvas.dataset.scopePort || "value");
        if (waveform) drawOscillatorScopePanel(canvas, waveform);
      });
  }
  requestAnimationFrame(tickAudioMonitors);
}









function tickDebugVoltmeterAudioSources() {
  document
    .querySelectorAll('.widget-instance[data-widget-type="debug"], .widget-instance[data-widget-type="voltmeter"], ' +
      '.widget-instance[data-widget-type="rgb-led"]')
    .forEach((el) => {
      const conn = connections.find((c) => c.to.widgetId === el.id && c.to.portId === "in");
      if (!conn) return;
      if (getPortKind(conn.from.widgetId, conn.from.portId, "out") !== "audio") return;
      const value = audioEngine.getPortValue(conn.from.widgetId, conn.from.portId);
      if (value === null) return;
      withDeviceSyncMuted(() => deliverToWidget(el.id, "in", value));
    });
}
setInterval(tickDebugVoltmeterAudioSources, 50);







const AUDIO_SIGNAL_WIDGET_TYPES = new Set([
  "oscillator", "fm-oscillator", "hammond", "wave-player", "soundfont-player", "gain", "filter", "comb-filter",
  "esp-eq", "graphic-eq",
  "mixer", "bitcrusher", "wave-folder", "pan", "sample-hold", "burst-generator", "chorus", "delay", "reverb-light", "audio-output-link", "audio-input-link", "switch", "multiplexer", "output-switch", "output-multiplexer",
  ...LINKED_INPUT_TYPES, ...LINKED_OUTPUT_TYPES,
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

const SCOPE_SHORT_WINDOW_SAMPLES = 1024;
const SCOPE_LONG_WINDOW_SAMPLES = 2048;
const SCOPE_PRETRIGGER_RATIO = 0.15;

function scopeWindowStart(waveform, level, windowSamples) {
  const visibleSamples = Math.min(windowSamples, waveform.length);
  const preTrigger = Math.round(visibleSamples * SCOPE_PRETRIGGER_RATIO);
  const latestEdge = waveform.length - (visibleSamples - preTrigger);
  let triggerIndex = -1;

  // The latest usable rising edge keeps the display close to "now" while
  // retaining a small pre-trigger section. The selected window is always
  // wholly inside the analyser buffer, so it must never wrap.
  for (let i = preTrigger; i <= latestEdge; i += 1) {
    if (waveform[i - 1] < level && waveform[i] >= level) triggerIndex = i;
  }
  return triggerIndex >= 0
    ? triggerIndex - preTrigger
    : Math.max(0, waveform.length - visibleSamples);
}

function drawOscilloscopeTrace(ctx2d, waveform, w, toY, startIndex, sampleCount) {
  ctx2d.beginPath();
  const step = w / Math.max(1, sampleCount - 1);
  for (let i = 0; i < sampleCount; i += 1) {
    const x = i * step;
    const y = toY(waveform[startIndex + i]);
    if (i === 0) ctx2d.moveTo(x, y);
    else ctx2d.lineTo(x, y);
  }
  ctx2d.stroke();
}

function drawOscilloscope(canvas, waveform1, waveform2, cvMode, state) {
  const ctx2d = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx2d.clearRect(0, 0, w, h);

  
  
  
  
  
  // Audio signals use the engine's nominal +/-5 V domain. Using Web Audio's
  // conventional +/-1 range here clipped every normal full-level waveform.
  // CV stays wider so regular 0-10 V control signals remain fully visible.
  const range = cvMode ? SCOPE_CV_DISPLAY_VOLTS : SCOPE_AUDIO_DISPLAY_VOLTS;
  const toY = (v) => (1 - clamp(v, -range, range) / range) * (h / 2);
  const triggerWaveform = state.triggerChannel === "in2" && waveform2 ? waveform2 : waveform1;
  const requestedSamples = state.timebase === "long" ? SCOPE_LONG_WINDOW_SAMPLES : SCOPE_SHORT_WINDOW_SAMPLES;
  const sampleCount = Math.min(requestedSamples, waveform1.length, waveform2?.length ?? waveform1.length);
  const startIndex = scopeWindowStart(triggerWaveform, state.triggerLevel, sampleCount);

  if (cvMode) {
    ctx2d.strokeStyle = "rgba(255, 255, 255, 0.15)";
    ctx2d.lineWidth = 1;
    ctx2d.beginPath();
    ctx2d.moveTo(0, h / 2);
    ctx2d.lineTo(w, h / 2);
    ctx2d.stroke();
  }

  if (state.triggerLevel >= -range && state.triggerLevel <= range) {
    ctx2d.strokeStyle = "rgba(248, 113, 113, 0.65)";
    ctx2d.lineWidth = 1;
    ctx2d.setLineDash([4, 3]);
    ctx2d.beginPath();
    ctx2d.moveTo(0, toY(state.triggerLevel));
    ctx2d.lineTo(w, toY(state.triggerLevel));
    ctx2d.stroke();
    ctx2d.setLineDash([]);
  }

  ctx2d.lineWidth = 1.5;
  ctx2d.strokeStyle = SCOPE_CH1_COLOR;
  drawOscilloscopeTrace(ctx2d, waveform1, w, toY, startIndex, sampleCount);
  if (waveform2) {
    ctx2d.strokeStyle = SCOPE_CH2_COLOR;
    drawOscilloscopeTrace(ctx2d, waveform2, w, toY, startIndex, sampleCount);
  }
}

// Mini fixed trace (no trigger-level UI) for the oscillator/fm-oscillator
// widgets' own inline scope panel - always auto-triggers at 0V.
function drawOscillatorScopePanel(canvas, waveform) {
  const ctx2d = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx2d.clearRect(0, 0, w, h);
  const range = SCOPE_AUDIO_DISPLAY_VOLTS;
  const toY = (v) => (1 - clamp(v, -range, range) / range) * (h / 2);
  const sampleCount = Math.min(1024, waveform.length);
  const startIndex = scopeWindowStart(waveform, 0, sampleCount);
  ctx2d.strokeStyle = "rgba(255, 255, 255, 0.15)";
  ctx2d.lineWidth = 1;
  ctx2d.beginPath();
  ctx2d.moveTo(0, h / 2);
  ctx2d.lineTo(w, h / 2);
  ctx2d.stroke();
  ctx2d.lineWidth = 1.5;
  ctx2d.strokeStyle = SCOPE_CH1_COLOR;
  drawOscilloscopeTrace(ctx2d, waveform, w, toY, startIndex, sampleCount);
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

function graphicEqLogPosition(hz, sampleRate) {
  const minHz = 20;
  const maxHz = Math.min(sampleRate / 2 - 1, 20000);
  return clamp((Math.log(clamp(hz, minHz, maxHz)) - Math.log(minHz)) /
    (Math.log(maxHz) - Math.log(minHz)), 0, 1);
}

function drawGraphicEqPreview(canvas, spectrum, magResponse, frequencies, sampleRate) {
  const ctx2d = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  const plotBottom = h;
  const minHz = 20;
  const maxHz = Math.min(sampleRate / 2 - 1, 20000);
  const xForHz = (hz) => graphicEqLogPosition(hz, sampleRate) * w;

  ctx2d.clearRect(0, 0, w, h);
  ctx2d.fillStyle = "rgba(148, 163, 184, 0.12)";
  [20, 100, 1000, 10000, 20000].forEach((hz) => {
    if (hz > maxHz) return;
    const x = xForHz(hz);
    ctx2d.fillRect(x, 0, 1, plotBottom);
  });

  ctx2d.fillStyle = "#4ade80";
  for (let i = 1; i < spectrum.length; i += 1) {
    const frequency = (i * sampleRate) / (2 * spectrum.length);
    if (frequency < minHz || frequency > maxHz) continue;
    const nextFrequency = (Math.min(i + 1, spectrum.length - 1) * sampleRate) / (2 * spectrum.length);
    const x = xForHz(frequency);
    const width = Math.max(1, xForHz(nextFrequency) - x);
    const barHeight = (spectrum[i] / 255) * plotBottom;
    ctx2d.fillRect(x, plotBottom - barHeight, width, barHeight);
  }

  const curveRangeDb = 24;
  ctx2d.strokeStyle = "#facc15";
  ctx2d.lineWidth = 2;
  ctx2d.beginPath();
  for (let i = 0; i < magResponse.length; i += 1) {
    const frequency = minHz * Math.pow(maxHz / minHz, i / Math.max(1, magResponse.length - 1));
    const db = clamp(20 * Math.log10(Math.max(magResponse[i], 1e-6)), -curveRangeDb, curveRangeDb);
    const x = xForHz(frequency);
    const y = plotBottom / 2 - (db / curveRangeDb) * (plotBottom / 2 - 2);
    if (i === 0) ctx2d.moveTo(x, y);
    else ctx2d.lineTo(x, y);
  }
  ctx2d.stroke();

  ctx2d.lineWidth = 1;
  frequencies.forEach((frequency) => {
    const x = xForHz(frequency);
    ctx2d.strokeStyle = "rgba(250, 204, 21, 0.65)";
    ctx2d.beginPath();
    ctx2d.moveTo(x, 0);
    ctx2d.lineTo(x, plotBottom);
    ctx2d.stroke();
  });
}

requestAnimationFrame(tickAudioMonitors);

const clockSettingsOverlay = document.getElementById("clockSettingsOverlay");
const clockPulseMsInput = document.getElementById("clockPulseMsInput");
let pendingClockId = null;

function openClockSettings(widgetId) {
  const state = clockStates.get(widgetId);
  if (!state) return;
  pendingClockId = widgetId;
  clockPulseMsInput.value = state.pulseMs;
  clockSettingsOverlay.classList.remove("hidden");
}

function closeClockSettings() {
  clockSettingsOverlay.classList.add("hidden");
  pendingClockId = null;
}


document.getElementById("clockSettingsCancel").addEventListener("click", closeClockSettings);

document.getElementById("clockSettingsSave").addEventListener("click", () => {
  if (!pendingClockId) return;
  const state = clockStates.get(pendingClockId);
  if (state) {
    let ms = parseFloatFlexible(clockPulseMsInput.value);
    if (Number.isNaN(ms) || ms <= 0) ms = 100;
    state.pulseMs = ms;
    // Live-push only, no savePresetNow() here - pulseMs never changes this
    // widget's ports/topology (unlike e.g. Mixer's inputCount or Step
    // Arpeggiator's stepCount, whose own settings-Save DOES call
    // savePresetNow() directly, justifiably: a graph rebuild is already
    // unavoidable there). A plain parameter change must never force a
    // rebuild/audio dropout on its own - see markDirty()'s comment. Reaches
    // the saved preset file next time something actually persists (Save
    // Preset, or any add/remove elsewhere), same as every other parameter.
    sendLiveParamUpdateThrottled(pendingClockId, { pulseMs: state.pulseMs });
  }
  closeClockSettings();
});



const oscillatorStates = new Map();
const fmOscillatorStates = new Map();

const hammondStates = new Map();
const HAMMOND_DEFAULT_DRAWBARS = [10, 10, 10, 0, 0, 0, 0, 0, 0]; // volts, 888 000 000
const HAMMOND_DEFAULT_RATIOS = [0.5, 1.5, 1, 2, 3, 4, 5, 6, 8];   // classic Hammond footages
const HAMMOND_FOOTAGES = ["16′", "5⅓′", "8′", "4′", "2⅔′", "2′", "1⅗′", "1⅓′", "1′"];

// CV Step-Arpeggiator: 16 pitch-CV knobs are always allocated (0-10V,
// default 5V/C4 - see nodes/StepArpeggiatorNode.h's StepArpState), only the
// first stepCount (8 or 16) are ever wired/rendered - shrinking the switch
// hides step9..16 without discarding their stored values (same pattern as
// Mixer's fixed 8-slot "gains" array, see renderMixerPorts).
const stepArpStates = new Map();
const STEP_ARP_MAX_STEPS = 16;
// Hardcoded (not CV_MAX_VOLTS / 2): CV_MAX_VOLTS itself is declared much
// later in this file as a top-level const - fine to forward-reference from
// inside a function body (evaluated lazily at call time, same pattern
// clockBpm/pitchCvToNoteName rely on), but NOT as another top-level const
// initializer, which runs immediately and would hit the temporal dead zone.
const STEP_ARP_DEFAULT_VOLTS = 5; // 5V = C4, see CvConventions.h

// Nearest chromatic note name for a 1V/octave pitch-CV value (C4 at 5V,
// matches cv::hzFromPitchCv's anchor - see CvConventions.h). Reuses
// WAVE_PLAYER_NOTE_NAMES/PITCH_CV_ANCHOR_NOTE/PITCH_CV_VOLTS_PER_OCTAVE
// declared further down this file - safe to forward-reference from inside a
// function body (same pattern clockBpm already relies on for CV_MAX_VOLTS).
function pitchCvToNoteName(volts) {
  const note = clamp(
    Math.round(((volts - CV_MAX_VOLTS / 2) / PITCH_CV_VOLTS_PER_OCTAVE) * 12 + PITCH_CV_ANCHOR_NOTE),
    0,
    127
  );
  const octave = Math.floor(note / 12) - 1;
  return `${WAVE_PLAYER_NOTE_NAMES[note % 12]}${octave}`;
}

// Step count (8/16) and gateOut pulse width are structural/occasional
// settings, not continuous-drag values - same dblclick-opened-modal +
// Save-triggers-a-full-resave pattern as Clock's own pulseMs/minBpm/maxBpm
// (see openClockSettings above) and Mixer's inputCount, rather than the
// live-WS-paramUpdate path the per-step knobs use.
const stepArpSettingsOverlay = document.getElementById("stepArpSettingsOverlay");
const stepArpStepCountInput = document.getElementById("stepArpStepCountInput");
const stepArpPulseTimeMaxInput = document.getElementById("stepArpPulseTimeMaxInput");
const stepArpRetriggerGateOutInput = document.getElementById("stepArpRetriggerGateOutInput");
const stepArpMaxSemitoneOffsetInput = document.getElementById("stepArpMaxSemitoneOffsetInput");
const stepArpClockMaxBpmInput = document.getElementById("stepArpClockMaxBpmInput");
const stepArpClockRoundBpmInput = document.getElementById("stepArpClockRoundBpmInput");
let pendingStepArpId = null;

function openStepArpSettings(widgetId) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  pendingStepArpId = widgetId;
  stepArpStepCountInput.value = String(state.stepCount);
  stepArpPulseTimeMaxInput.value = String(state.pulseTimeMaxSec);
  stepArpMaxSemitoneOffsetInput.value = String(state.maxSemitoneOffset);
  stepArpRetriggerGateOutInput.checked = state.retriggerGateOut === true;
  stepArpClockMaxBpmInput.value = String(state.clockMaxBpm);
  stepArpClockRoundBpmInput.checked = state.clockRoundBpm === true;
  stepArpSettingsOverlay.classList.remove("hidden");
}

function closeStepArpSettings() {
  stepArpSettingsOverlay.classList.add("hidden");
  pendingStepArpId = null;
}


document.getElementById("stepArpSettingsCancel").addEventListener("click", closeStepArpSettings);

document.getElementById("stepArpSettingsSave").addEventListener("click", () => {
  if (!pendingStepArpId) return;
  const state = stepArpStates.get(pendingStepArpId);
  if (state) {
    const newCount = stepArpStepCountInput.value === "8" ? 8 : STEP_ARP_MAX_STEPS;
    let maxSec = parseFloatFlexible(stepArpPulseTimeMaxInput.value);
    if (Number.isNaN(maxSec) || maxSec <= state.pulseTimeMinSec) maxSec = state.pulseTimeMinSec + 1;
    const countChanged = newCount !== state.stepCount;

    if (newCount < state.stepCount) {
      for (let i = newCount + 1; i <= state.stepCount; i += 1) {
        removeConnectionsForPort(pendingStepArpId, `step${i}`, "in");
      }
    }
    state.stepCount = newCount;
    // Just the range the "pulseTime" slider's 0-10V covers - does not
    // itself change the current pulseMs value (same as Delay's own
    // timeMin/timeMax settings never touching delayTimeMs), only where the
    // slider knob sits to represent it (see updateStepArpPulseTimeVisual).
    // pulseTimeMinSec (0V end) is no longer settings-dialog-editable - stays
    // at its build-time default (0.001s), only the 10V end is exposed.
    state.pulseTimeMaxSec = maxSec;
    let maxSemitoneOffset = parseFloatFlexible(stepArpMaxSemitoneOffsetInput.value);
    if (Number.isNaN(maxSemitoneOffset) || maxSemitoneOffset < 0) maxSemitoneOffset = 60;
    state.maxSemitoneOffset = clamp(maxSemitoneOffset, 0, 60);
    state.retriggerGateOut = stepArpRetriggerGateOutInput.checked === true;
    let maxBpm = parseFloatFlexible(stepArpClockMaxBpmInput.value);
    if (Number.isNaN(maxBpm) || maxBpm <= 0) maxBpm = 120;
    state.clockMaxBpm = maxBpm;
    state.clockRoundBpm = stepArpClockRoundBpmInput.checked === true;

    if (countChanged) {
      // Structural: stepCount changes which "stepN" ports/knobs exist (and,
      // shrinking, just deleted cables on the hidden ones above) - same
      // class of change as Mixer's inputCount, a rebuild is unavoidable
      // either way, so savePresetNow() here costs nothing extra.
      const el = document.getElementById(pendingStepArpId);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderStepArpPorts(pendingStepArpId, portsEl);
      drawConnections();
      savePresetNow();
    } else {
      // Pure parameter - live-push, no forced rebuild/dropout (see
      // markDirty()'s comment). Still reaches the saved preset file next
      // time something actually persists.
      updateStepArpVisual(pendingStepArpId);
      stepArpRestartInternalClock(pendingStepArpId);
      sendLiveParamUpdateThrottled(pendingStepArpId, {
        pulseTimeMinSec: state.pulseTimeMinSec,
        pulseTimeMaxSec: state.pulseTimeMaxSec,
        maxSemitoneOffset: state.maxSemitoneOffset,
        // 0/1, not a JS boolean - see Clock's own "roundBpm" live-push for
        // why (Graph::applyLiveParams' generic path only fires for JSON
        // numbers; nodes::steparp::setParam's `value >= 0.5f` check already
        // makes a dedicated cJSON_IsBool special case there unnecessary).
        retriggerGateOut: state.retriggerGateOut ? 1 : 0,
        clockMaxBpm: state.clockMaxBpm,
        clockRoundBpm: state.clockRoundBpm ? 1 : 0,
      });
    }
  }
  closeStepArpSettings();
});

const drumSequencerStates = new Map();
const DRUM_DEFAULT_STEPS = 16;
const DRUM_MAX_TRACKS = 8;
const DRUM_STEP_COUNTS = [4, 8, 12, 16];
const DRUM_MAX_PATTERNS = 8;

const drumSequencerSettingsOverlay = document.getElementById("drumSequencerSettingsOverlay");
const drumSequencerStepCountInput = document.getElementById("drumSequencerStepCountInput");
const drumSequencerTrackCountInput = document.getElementById("drumSequencerTrackCountInput");
const drumSequencerClockMaxBpmInput = document.getElementById("drumSequencerClockMaxBpmInput");
const drumSequencerClockRoundBpmInput = document.getElementById("drumSequencerClockRoundBpmInput");
let pendingDrumSequencerId = null;

function openDrumSequencerSettings(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  pendingDrumSequencerId = widgetId;
  drumSequencerStepCountInput.value = String(state.stepCount);
  drumSequencerTrackCountInput.value = String(state.trackCount);
  drumSequencerClockMaxBpmInput.value = String(state.clockMaxBpm);
  drumSequencerClockRoundBpmInput.checked = state.clockRoundBpm === true;
  drumSequencerSettingsOverlay.classList.remove("hidden");
}

function closeDrumSequencerSettings() {
  drumSequencerSettingsOverlay.classList.add("hidden");
  pendingDrumSequencerId = null;
}

function reshapeDrumSequencer(state, stepCount, trackCount) {
  const oldSteps = state.stepCount;
  const newSteps = stepCount;
  const copyTracks = Math.min(state.trackCount, trackCount);
  const copySteps = Math.min(oldSteps, newSteps);
  state.patterns = state.patterns.map((old) => {
    const active = new Array(newSteps * trackCount).fill(false);
    const velocities = new Array(newSteps * trackCount).fill(CV_MAX_VOLTS);
    for (let track = 0; track < copyTracks; track += 1) {
      for (let step = 0; step < copySteps; step += 1) {
        active[track * newSteps + step] = old.active[track * oldSteps + step] === true;
        velocities[track * newSteps + step] = old.velocities[track * oldSteps + step];
      }
    }
    return { active, velocities };
  });
  state.stepCount = stepCount;
  state.trackCount = trackCount;
  state.currentStep = 0;
  state.lastStep = -1;
}

document.getElementById("drumSequencerSettingsCancel").addEventListener("click", closeDrumSequencerSettings);

document.getElementById("drumSequencerSettingsSave").addEventListener("click", () => {
  if (!pendingDrumSequencerId) return;
  const widgetId = pendingDrumSequencerId;
  const state = drumSequencerStates.get(widgetId);
  if (state) {
    const requestedSteps = Number(drumSequencerStepCountInput.value);
    const stepCount = DRUM_STEP_COUNTS.includes(requestedSteps) ? requestedSteps : DRUM_DEFAULT_STEPS;
    const trackCount = clamp(parseInt(drumSequencerTrackCountInput.value, 10) || 1, 1, DRUM_MAX_TRACKS);
    const structural = stepCount !== state.stepCount || trackCount !== state.trackCount;
    if (trackCount < state.trackCount) {
      for (let track = trackCount + 1; track <= state.trackCount; track += 1) {
        removeConnectionsForPort(widgetId, `trigger${track}`, "out");
        removeConnectionsForPort(widgetId, `velocity${track}`, "out");
        clearTimeout(state.triggerTimers[track - 1]);
      }
    }
    if (structural) reshapeDrumSequencer(state, stepCount, trackCount);

    let maxBpm = parseFloatFlexible(drumSequencerClockMaxBpmInput.value);
    if (!Number.isFinite(maxBpm) || maxBpm <= 0) maxBpm = 120;
    state.clockMaxBpm = clamp(maxBpm, 0.1, 10000);
    state.clockRoundBpm = drumSequencerClockRoundBpmInput.checked === true;

    if (structural) {
      const portsEl = document.getElementById(widgetId)?.querySelector(".widget-ports");
      if (portsEl) renderDrumSequencerPorts(widgetId, portsEl);
      drawConnections();
      savePresetNow();
    } else {
      updateDrumSequencerVisual(widgetId);
      drumSequencerRestartInternalClock(widgetId);
      sendLiveParamUpdate(widgetId, {
        clockMaxBpm: state.clockMaxBpm,
        clockRoundBpm: state.clockRoundBpm ? 1 : 0,
      });
    }
  }
  closeDrumSequencerSettings();
});

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

function updateWavePlayerGainVisual(widgetId) {
  const state = wavePlayerStates.get(widgetId);
  if (!state) return;
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "gain" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = document.querySelector(`#${widgetId} [data-role="wave-player-gain-slider"]`);
  if (slider) {
    slider.value = String(state.gain);
    slider.disabled = audioConnected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="wave-player-gain-value"]`);
  if (valueSpan) {
    valueSpan.textContent = formatNumber(state.gain);
  }
}

function updateOscillatorFeedbackVisual(widgetId) {
  const state = oscillatorStates.get(widgetId);
  if (!state) return;
  const connected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "feedback");
  const slider = document.querySelector(`#${widgetId} [data-role="oscillator-feedback-slider"]`);
  if (slider) {
    slider.value = String(state.feedback);
    slider.disabled = connected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="oscillator-feedback-value"]`);
  if (valueSpan) valueSpan.textContent = formatNumber(state.feedback);
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

// fm-oscillator: parallel set of the six functions above, keyed by
// fmOscillatorStates instead of oscillatorStates - see the "fm-oscillator"
// createWidget/createPortRow blocks for how these get wired to the UI.
function updateFmOscillatorFreqLabel(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const freqLabel = el.querySelector('[data-role="osc-freq-label"]');
  if (freqLabel) freqLabel.textContent = `${formatNumber(state.freq)} Hz`;
}

function recomputeFmOscillatorFreq(widgetId, voiceIndex) {
  const state = fmOscillatorStates.get(widgetId);
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
  updateFmOscillatorFreqLabel(widgetId);
}

function updateFmOscillatorTuneVisual(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  const tuneAudioConnected = connections.some(
    (c) => c.to.widgetId === widgetId && c.to.portId === "tune" && getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = document.querySelector(`#${widgetId} [data-role="fm-oscillator-tune-slider"]`);
  if (slider) {
    slider.value = String(state.tuneVolts);
    slider.disabled = tuneAudioConnected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="fm-oscillator-tune-value"]`);
  if (valueSpan) {
    valueSpan.textContent = formatNumber(state.tuneVolts);
  }
}

function updateFmOscillatorGainVisual(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "gain" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = document.querySelector(`#${widgetId} [data-role="fm-oscillator-gain-slider"]`);
  if (slider) {
    slider.value = String(state.gain);
    slider.disabled = audioConnected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="fm-oscillator-gain-value"]`);
  if (valueSpan) {
    valueSpan.textContent = formatNumber(state.gain);
  }
}

function updateFmOscillatorIndexVisual(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "fmIndex" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const slider = document.querySelector(`#${widgetId} [data-role="fm-oscillator-fmindex-slider"]`);
  if (slider) {
    slider.value = String(state.fmIndex);
    slider.disabled = audioConnected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="fm-oscillator-fmindex-value"]`);
  if (valueSpan) {
    valueSpan.textContent = formatNumber(state.fmIndex);
  }
}

function updateFmOscillatorFeedbackVisual(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  const connected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "feedback");
  const slider = document.querySelector(`#${widgetId} [data-role="fm-oscillator-feedback-slider"]`);
  if (slider) {
    slider.value = String(state.feedback);
    slider.disabled = connected;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="fm-oscillator-feedback-value"]`);
  if (valueSpan) valueSpan.textContent = formatNumber(state.feedback);
}

function updateFmOscillatorWaveButtons(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".osc-wave-btn").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.wave === state.waveType);
  });
}

// "Duty Cycle" only shapes WaveType::Square (see DspPrimitives.h's
// PhaseOsc::sampleAt) - stays visible for every waveform (unlike the
// hidden-row first attempt), just disabled (greyed out, non-interactive)
// whenever it wouldn't currently have an audible effect: a non-square
// waveform, or a cable already driving the port. Called whenever waveType
// changes (setFmOscillatorWaveType) and everywhere else the other
// fm-oscillator sliders get resynced (connection changes, preset load).
function updateFmOscillatorDutyCycleVisual(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  const connected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "dutyCycle");
  const dutyDisabled = connected || state.waveType !== "square";
  const row = document.querySelector(`#${widgetId} [data-role="fm-oscillator-dutycycle-row"]`);
  if (row) row.classList.toggle("disabled", dutyDisabled);
  const slider = document.querySelector(`#${widgetId} [data-role="fm-oscillator-dutycycle-slider"]`);
  if (slider) {
    slider.value = String(state.dutyCycle);
    slider.disabled = dutyDisabled;
  }
  const valueSpan = document.querySelector(`#${widgetId} [data-role="fm-oscillator-dutycycle-value"]`);
  if (valueSpan) valueSpan.textContent = formatNumber(state.dutyCycle);
}

function updateFmOscillatorSuperSawVisual(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  // Rows always visible; greyed out (like Duty Cycle) unless SuperSaw is selected.
  const active = state.waveType === "supersaw";
  [
    ["superSawDetune", "supersaw-detune"],
    ["superSawMix", "supersaw-mix"],
  ].forEach(([param, role]) => {
    const connected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === param);
    const row = document.querySelector(`#${widgetId} [data-role="fm-oscillator-${role}-row"]`);
    if (row) row.classList.toggle("disabled", !active);
    const slider = document.querySelector(`#${widgetId} [data-role="fm-oscillator-${role}-slider"]`);
    if (slider) {
      slider.value = String(state[param]);
      slider.disabled = connected || !active;
    }
    const valueSpan = document.querySelector(`#${widgetId} [data-role="fm-oscillator-${role}-value"]`);
    if (valueSpan) valueSpan.textContent = formatNumber(state[param]);
  });
}

// --- Oscillator wavetable ---------------------------------------------------
// Same bank format/editor as the Wavetable widget. The bank lives in the widget
// state, goes to the device live (/api/wavetable, postLfoWavetableLive) and is
// written to flash only with a regular preset save (WIDGET_ENGINES getParams).

function fmOscillatorWavetableBank(state) {
  if (!state.wavetables) state.wavetables = createDefaultBank();
  return state.wavetables;
}

function updateFmOscillatorWavetableVisual(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const active = state.waveType === "wavetable";
  // Row always visible; greyed out (like Duty Cycle) unless Wavetable is selected.
  const row = el.querySelector('[data-role="fm-oscillator-morph-row"]');
  if (row) row.classList.toggle("disabled", !active);
  const connected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "morph");
  const slider = el.querySelector('[data-role="fm-oscillator-morph-slider"]');
  if (slider) {
    slider.value = String(state.morph);
    slider.disabled = connected || !active;
  }
  const valueSpan = el.querySelector('[data-role="fm-oscillator-morph-value"]');
  if (valueSpan) valueSpan.textContent = formatNumber(state.morph);
  const canvas = el.querySelector('[data-role="fm-wavetable-preview"]');
  if (canvas) {
    canvas.hidden = !active;
    // Static picture of the morphed wave - no playhead, it would cycle at audio rate.
    if (active) drawWavetablePreview(canvas, fmOscillatorWavetableBank(state), state.morph);
  }
}

function setFmOscillatorMorph(widgetId, volts) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  state.morph = clamp(Number(volts) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, "morph", state.morph);
  updateFmOscillatorWavetableVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { morph: state.morph });
}

function openFmWavetableEditor(widgetId) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  const original = fmOscillatorWavetableBank(state).map((t) => Float32Array.from(t));
  const label = document.getElementById(widgetId)?.querySelector(".widget-instance-label")?.textContent || "Oscillator";
  const preview = (tables, live = true) => {
    state.wavetables = tables;
    audioEngine.setParam(widgetId, "wavetables", tables);
    updateFmOscillatorWavetableVisual(widgetId);
    if (live) postLfoWavetableLive(widgetId, tables);
  };
  openWavetableEditor({
    title: `Wavetable Editor – ${label}`,
    tables: original,
    onPreview: preview,
    onCancel: () => preview(original),
    onApply: (tables) => {
      preview(original, false);
      pushHistorySnapshot();
      // Written to flash with the next regular preset save (serializePreset()).
      preview(tables, true);
    },
  });
}

function setFmOscillatorWaveType(widgetId, waveType) {
  const state = fmOscillatorStates.get(widgetId);
  if (!state) return;
  state.waveType = waveType;
  audioEngine.setParam(widgetId, "waveType", waveType);
  if (waveType === "wavetable") {
    // The device may not hold a bank for this widget yet (the preset had another waveform):
    // send it along; until it arrives the device plays a sine.
    const bank = fmOscillatorWavetableBank(state);
    audioEngine.setParam(widgetId, "wavetables", bank);
    audioEngine.setParam(widgetId, "morph", state.morph);
    postLfoWavetableLive(widgetId, bank);
  }
  updateFmOscillatorWaveButtons(widgetId);
  updateFmOscillatorDutyCycleVisual(widgetId);
  updateFmOscillatorSuperSawVisual(widgetId);
  updateFmOscillatorWavetableVisual(widgetId);
  sendLiveParamUpdate(widgetId, { waveType });
}





function mapWaveformValue(value) {
  const idx = clamp(Math.floor(value / (CV_MAX_VOLTS / WAVE_TYPE_BUTTONS.length)), 0, WAVE_TYPE_BUTTONS.length - 1);
  return WAVE_TYPE_BUTTONS[idx].id;
}

function mapFmWaveformValue(value) {
  const idx = clamp(Math.floor(value / (CV_MAX_VOLTS / FM_WAVE_TYPE_BUTTONS.length)), 0, FM_WAVE_TYPE_BUTTONS.length - 1);
  return FM_WAVE_TYPE_BUTTONS[idx].id;
}



const audioOutStates = new Map();
const audioOutputLinkStates = new Map();
const audioInputLinkStates = new Map();
// Freely-placeable linked-io widgets (unrelated to the fixed 8-bus
// audio-output-link/audio-input-link pair above). linkedInputStates:
// { linkId, linkName }. linkedOutputStates: { sourceLinkId }.
const linkedInputStates = new Map();
const linkedOutputStates = new Map();
const activeLinkedControlRoutes = new Set();
const linkedAudioRouteIds = new Set();
let pendingLinkedInputId = null;
let pendingLinkedOutputId = null;
let pendingAudioLinkId = null;

function generateLinkId() {
  let id;
  do {
    id = "link-" + Math.random().toString(36).slice(2, 8);
  } while ([...linkedInputStates.values()].some((s) => s.linkId === id));
  return id;
}

// A freshly placed Link In starts out unnamed. The first cable plugged into it
// names it after that source ("Midi-In Gate"), so it is identifiable in the Link
// Out source list without opening the settings dialog first. An existing name is
// never overwritten.
function autoNameLinkedInput(widgetId, fromWidgetId, fromPortId, toPortId = "in") {
  const state = linkedInputStates.get(widgetId);
  if (!state) return;
  const sourceEl = document.getElementById(fromWidgetId);
  if (!sourceEl) return;
  const widgetLabel = sourceEl.querySelector(".widget-instance-label")?.textContent.trim();
  const portLabel = (WIDGET_TYPES[sourceEl.dataset.widgetType]?.outputs || [])
    .find((port) => port.id === fromPortId)?.label?.trim();
  const base = [widgetLabel, portLabel].filter(Boolean).join(" ");
  if (!base) return;
  const portIndex = state.linkCount === 1 ? 0 : Math.max(0, Number(toPortId.slice(2)) - 1);
  if (!state.labels[portIndex]) state.labels[portIndex] = base;
  if (state.linkName) return;
  const taken = new Set(
    [...linkedInputStates.entries()]
      .filter(([id]) => id !== widgetId)
      .map(([, other]) => other.linkName)
      .filter(Boolean)
  );
  let name = base;
  let suffix = 2;
  while (taken.has(name)) {
    name = `${base} ${suffix}`;
    suffix += 1;
  }
  state.linkName = name;
}

function updateLinkedInputVisual(widgetId) {
  const el = document.getElementById(widgetId);
  const state = linkedInputStates.get(widgetId);
  if (!el || !state) return;
  el.querySelectorAll(".port-row-in").forEach((row, index) => {
    const label = row.querySelector(".linked-io-port-label");
    if (label) label.textContent = state.labels?.[index] || String(index + 1);
  });
  renderLinkedIoLayout(widgetId);
}

function updateLinkedOutputVisual(widgetId) {
  const el = document.getElementById(widgetId);
  const state = linkedOutputStates.get(widgetId);
  if (!el || !state) return;
  const status = el.querySelector('[data-role="linked-io-group-name"]');
  if (!status) return;
  const portLabels = el.querySelectorAll(".port-row-out .linked-io-port-label");
  if (!state.sourceLinkId) {
    status.textContent = "Not connected";
    status.removeAttribute("title");
    status.classList.remove("missing");
    portLabels.forEach((label, index) => { label.textContent = String(index + 1); });
    renderLinkedIoLayout(widgetId);
    return;
  }
  const sourceEntry = [...linkedInputStates.entries()].find(([, source]) => source.linkId === state.sourceLinkId);
  if (sourceEntry) {
    const [, source] = sourceEntry;
    status.textContent = "";
    status.removeAttribute("title");
    status.classList.remove("missing");
    portLabels.forEach((label, index) => { label.textContent = source.labels?.[index] || String(index + 1); });
  } else {
    status.textContent = "Missing link: " + state.sourceLinkId;
    status.removeAttribute("title");
    status.classList.add("missing");
    portLabels.forEach((label, index) => { label.textContent = String(index + 1); });
  }
  renderLinkedIoLayout(widgetId);
}

function renderLinkedIoLayout(widgetId) {
  const el = document.getElementById(widgetId);
  const type = el?.dataset.widgetType;
  const state = isLinkedInputType(type) ? linkedInputStates.get(widgetId) : linkedOutputStates.get(widgetId);
  const ports = el?.querySelector(".widget-ports");
  if (!el || !state || !ports) return;
  const side = ["top", "bottom", "left", "right"].includes(state.portSide)
    ? state.portSide
    : (isLinkedInputType(type) ? "left" : "right");
  ports.className = `widget-ports linked-io-ports linked-io-ports-${side}`;
  ports.style.setProperty("--linked-io-count", state.linkCount);
  el.style.setProperty("--linked-io-count", state.linkCount);
  const horizontal = side === "top" || side === "bottom";
  el.style.width = `${horizontal ? Math.max(170, state.linkCount * 72) : 190}px`;
  const body = el.querySelector(".linked-io-body");
  if (body) body.style.minHeight = `${horizontal ? 52 : Math.max(44, state.linkCount * 28)}px`;
}

function refreshLinkedIO() {
  document.querySelectorAll('.widget-instance[data-widget-type^="linked-input"]').forEach((el) => updateLinkedInputVisual(el.id));
  document.querySelectorAll('.widget-instance[data-widget-type^="linked-output"]').forEach((el) => updateLinkedOutputVisual(el.id));

  // A link can change between control and audio when its input cable or its
  // selected source changes. Remove only routes that were really connected
  // in WebAudio, then rebuild the matching subset in the new mode.
  linkedAudioRouteIds.forEach((id) => {
    const connection = connections.find((c) => c.id === id);
    if (connection) {
      audioEngine.disconnect(
        connection.from.widgetId,
        connection.from.portId,
        connection.to.widgetId,
        connection.to.portId,
      );
    }
  });
  linkedAudioRouteIds.clear();
  audioEngine._wireLinkedIO();
  connections.filter(connectionTouchesLinkedIO).forEach(connectIfAudio);
}

function forwardLinkedInputValue(widgetId, portId, value, voiceIndex) {
  const source = linkedInputStates.get(widgetId);
  if (!source?.linkId) return;
  const portIndex = source.linkCount === 1 ? 0 : Math.max(0, Number(portId.slice(2)) - 1);
  const outputPortId = linkedIoPortId("out", portIndex, source.linkCount);
  const routeKey = `${widgetId}:${portIndex}:${voiceIndex ?? "global"}`;
  if (activeLinkedControlRoutes.has(routeKey)) return;

  activeLinkedControlRoutes.add(routeKey);
  try {
    linkedOutputStates.forEach((target, targetWidgetId) => {
      if (target.sourceLinkId === source.linkId) {
        propagate(targetWidgetId, outputPortId, value, voiceIndex);
      }
    });
  } finally {
    activeLinkedControlRoutes.delete(routeKey);
  }
}

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

function getAudioLinkLabels(widgetId = null) {
  const masterState = audioOutputLinkStates.values().next().value;
  const inputState = widgetId ? audioInputLinkStates.get(widgetId) : null;
  return masterState?.labels || inputState?.labels || Array(LINK_BUS_COUNT).fill("");
}

function formatAudioLinkPortLabel(index, label) {
  return label ? `${index + 1}: ${label}` : String(index + 1);
}

function updateAudioOutputLinkVisual(widgetId) {
  const el = document.getElementById(widgetId);
  if (!el) return;
  const labels = getAudioLinkLabels();
  el.querySelectorAll(".port-row-in").forEach((row) => {
    const match = /^in([1-8])$/.exec(row.dataset.portId || "");
    const label = row.querySelector(".port-label");
    if (match && label) {
      const index = Number(match[1]) - 1;
      label.textContent = formatAudioLinkPortLabel(index, labels[index]);
      label.title = label.textContent;
    }
  });
}

function updateAudioInputLinkVisual(widgetId) {
  const state = audioInputLinkStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const labels = getAudioLinkLabels(widgetId);
  el.querySelectorAll(".port-row-in, .port-row-out").forEach((row) => {
    const match = /^(?:gain|out)([1-8])$/.exec(row.dataset.portId || "");
    const label = row.querySelector(".port-label");
    if (match && label) {
      const index = Number(match[1]) - 1;
      label.textContent = formatAudioLinkPortLabel(index, labels[index]);
      label.title = label.textContent;
    }
  });
  state.gains.forEach((gain, channel) => {
    const slider = el.querySelector(`[data-role="audio-input-link-gain-${channel}"]`);
    const value = el.querySelector(`[data-role="audio-input-link-gain-value-${channel}"]`);
    if (slider) slider.value = String(gain);
    if (value) value.textContent = `${formatNumber(gain)} V`;
  });
}

function setAudioLinkLabels(labels) {
  const normalized = Array.from({ length: LINK_BUS_COUNT }, (_, index) => (
    typeof labels[index] === "string" ? labels[index].trim() : ""
  ));
  audioOutputLinkStates.forEach((state) => { state.labels = [...normalized]; });
  audioInputLinkStates.forEach((state) => { state.labels = [...normalized]; });
  document.querySelectorAll('.widget-instance[data-widget-type="audio-output-link"]').forEach((el) => updateAudioOutputLinkVisual(el.id));
  document.querySelectorAll('.widget-instance[data-widget-type="audio-input-link"]').forEach((el) => updateAudioInputLinkVisual(el.id));
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



function setGainGlobal(widgetId, on) {
  const state = gainStates.get(widgetId);
  if (!state) return;
  state.global = on;
  audioEngine.setParam(widgetId, "global", on);
  updateGainVisual(widgetId);
  sendLiveParamUpdate(widgetId, { global: on });
}




function updateGainVisual(widgetId) {
  const state = gainStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const curveConnected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "curve");
  const curveSlider = el.querySelector('[data-role="gain-curve-slider"]');
  if (curveSlider) {
    curveSlider.value = String(state.curve);
    curveSlider.disabled = curveConnected;
  }
  const curveValue = el.querySelector('[data-role="gain-curve-value"]');
  if (curveValue) curveValue.textContent = `${formatNumber(state.curve)} V`;

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
    valueSpan.textContent = `${formatNumber(state.amount)} V`;
  }
  const globalToggle = el.querySelector('[data-role="gain-global-switch"]');
  if (globalToggle) globalToggle.checked = state.global === true;
}



const filterStates = new Map();
const combFilterStates = new Map();

function formatCombFilterValue(state, port) {
  const value = state[port];
  if (port === "time") return `${formatNumber(value * 10)} ms`;
  if (port === "feedback") return `${formatNumber(value * 9.5)} %`;
  if (port === "mix") return `${formatNumber(value * 10)} %`;
  if (port === "polarity") return value >= 5 ? "−" : "+";
  if (port === "damping") return value >= 10 ? "Off" : `${Math.round(20 * 1000 ** (value / 10))} Hz`;
  return `${formatNumber(value)} V`;
}

function updateCombFilterVisual(widgetId) {
  const state = combFilterStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  COMB_FILTER_PORTS.forEach((port) => {
    const slider = el.querySelector(`[data-role="comb-${port}-slider"]`);
    const value = el.querySelector(`[data-role="comb-${port}-value"]`);
    if (slider) slider.value = String(state[port]);
    if (value) value.textContent = formatCombFilterValue(state, port);
  });
  const toggle = el.querySelector('[data-role="comb-polarity-switch"]');
  if (toggle) {
    toggle.checked = state.polarity >= 5;
    toggle.disabled = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "polarity");
  }
}

// graphic-eq's non-CV scalars - band center frequencies (settings-dialog
// only, NOT CV-controllable) and the final output trim (raw 0-10V CV, see
// the "gain" port slider). Per-band GAIN lives in effectCvStates instead
// (see EFFECT_CV_PARAMS["graphic-eq"]) since that's the CV-controllable part.
const graphicEqStates = new Map();
const ESP_EFFECT_DEFAULTS = {
  "esp-eq": { filterType: "peak", frequency: 1000, q: 1, gainDb: 0, bypass: false },
};
const espEffectStates = new Map();

const ESP_EFFECT_SLIDER_CONFIG = {
  "esp-eq": {
    frequency: { min: 20, max: 22049, step: 1, unit: " Hz" },
    q: { min: 0.1, max: 20, step: 0.1, unit: "" },
    gainDb: { min: -15, max: 15, step: 0.1, unit: " dB" },
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
  none: "None",
  lowpass: "Lowpass",
  highpass: "Highpass",
  bandpass: "Bandpass",
  notch: "Notch",
  lowshelf: "Low Shelf",
  highshelf: "High Shelf",
  allpass: "Allpass",
};

// Same icon-button treatment as the oscillator's wave buttons (WAVE_TYPE_BUTTONS
// above, shared .osc-wave-btn styling) - each entry draws its frequency response
// in its own colour. "none" passes the signal through untouched, so it gets a
// flat line in a deliberately neutral grey.
const FILTER_TYPE_BUTTONS = [
  {
    id: "none",
    label: "None",
    color: "#64748b",
    glow: "rgba(100, 116, 139, 0.45)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,50 L 90,50"/></svg>`,
  },
  {
    id: "lowpass",
    label: "LP",
    color: "#06b6d4",
    glow: "rgba(6, 182, 212, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 12,30 L 45,30 Q 62,30 72,55 T 88,85"/></svg>`,
  },
  {
    id: "highpass",
    label: "HP",
    color: "#ec4899",
    glow: "rgba(236, 72, 153, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 12,85 L 28,85 Q 38,55 55,30 L 88,30"/></svg>`,
  },
  {
    id: "bandpass",
    label: "BP",
    color: "#10b981",
    glow: "rgba(16, 185, 129, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,85 L 25,85 C 38,85 43,20 50,20 C 57,20 62,85 75,85 L 90,85"/></svg>`,
  },
  {
    id: "notch",
    label: "Notch",
    color: "#f59e0b",
    glow: "rgba(245, 158, 11, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,30 L 35,30 C 43,30 46,85 50,85 C 54,85 57,30 65,30 L 90,30"/></svg>`,
  },
  {
    id: "lowshelf",
    label: "LS",
    color: "#a855f7",
    glow: "rgba(168, 85, 247, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,24 L 32,24 C 46,24 54,62 68,62 L 90,62"/></svg>`,
  },
  {
    id: "highshelf",
    label: "HS",
    color: "#eab308",
    glow: "rgba(234, 179, 8, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,62 L 32,62 C 46,62 54,24 68,24 L 90,24"/></svg>`,
  },
  {
    // Allpass leaves the magnitude flat and only turns the phase, so the icon
    // shows both: the flat response on top, the phase swing underneath.
    id: "allpass",
    label: "AP",
    color: "#3b82f6",
    glow: "rgba(59, 130, 246, 0.55)",
    icon: `<svg ${WAVE_ICON_ATTRS} stroke-width="8"><path d="M 10,28 L 90,28"/><path d="M 10,58 C 30,58 34,88 50,88 C 66,88 70,58 90,58" opacity=".55"/></svg>`,
  },
];

const FILTER_SHELF_TYPES = ["lowshelf", "highshelf"];

// Filter steepness in 6 dB (= one pole) steps, selectable by button or by CV on
// the "slope" port - same discrete 0-10V mapping as "filterType".
const FILTER_SLOPE_BUTTONS = [6, 12, 18, 24, 30, 36].map((db) => ({ id: db, label: `${db}` }));

function filterSlopeFromCv(volts) {
  const index = clamp(Math.floor(clamp(Number(volts) || 0, 0, CV_MAX_VOLTS) / (CV_MAX_VOLTS / FILTER_SLOPE_BUTTONS.length)), 0, FILTER_SLOPE_BUTTONS.length - 1);
  return FILTER_SLOPE_BUTTONS[index].id;
}

function filterSlopeToCv(slope) {
  const index = Math.max(0, FILTER_SLOPE_BUTTONS.findIndex((entry) => entry.id === slope));
  return ((index + 0.5) * CV_MAX_VOLTS) / FILTER_SLOPE_BUTTONS.length;
}

function filterSlopeLabel(slope) {
  return `${slope ?? 12} dB`;
}

// Presets written before the CEM3320 gained the full type list stored its type
// and its slope in one string ("lp24", "bp12", ...).
const CURTIS_LEGACY_FILTER_TYPES = {
  lp24: { filterType: "lowpass", slope: 24 },
  lp12: { filterType: "lowpass", slope: 12 },
  bp12: { filterType: "bandpass", slope: 12 },
  hp12: { filterType: "highpass", slope: 12 },
};

function filterTypeFromCv(volts) {
  const index = clamp(Math.floor(clamp(Number(volts) || 0, 0, CV_MAX_VOLTS) / (CV_MAX_VOLTS / FILTER_TYPE_BUTTONS.length)), 0, FILTER_TYPE_BUTTONS.length - 1);
  return FILTER_TYPE_BUTTONS[index].id;
}

function filterTypeToCv(filterType) {
  const index = Math.max(0, FILTER_TYPE_BUTTONS.findIndex((entry) => entry.id === filterType));
  return ((index + 0.5) * CV_MAX_VOLTS) / FILTER_TYPE_BUTTONS.length;
}

function filterTypeLabel(filterType) {
  return FILTER_TYPE_LABELS[filterType] || "None";
}

function setFilterType(widgetId, filterType) {
  const state = filterStates.get(widgetId);
  if (!state) return;
  state.filterType = filterType;
  audioEngine.setParam(widgetId, "filterType", filterType);
  updateFilterVisual(widgetId);
  sendLiveParamUpdate(widgetId, { filterType });
}

function setFilterSlope(widgetId, slope) {
  const state = filterStates.get(widgetId);
  if (!state) return;
  state.slope = FILTER_SLOPE_BUTTONS.some((entry) => entry.id === slope) ? slope : 12;
  audioEngine.setParam(widgetId, "slope", state.slope);
  updateFilterVisual(widgetId);
  sendLiveParamUpdate(widgetId, { slope: state.slope });
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

  el.querySelectorAll("[data-filter-type]").forEach((btn) => {
    btn.classList.toggle("active", btn.dataset.filterType === state.filterType);
  });

  const slopeConnected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "slope");
  el.querySelectorAll("[data-filter-slope]").forEach((btn) => {
    btn.classList.toggle("active", Number(btn.dataset.filterSlope) === (state.slope ?? 12));
    btn.disabled = slopeConnected || state.filterType === "none";
  });
  const slopeSlider = el.querySelector('[data-role="filter-slope-slider"]');
  if (slopeSlider) {
    slopeSlider.value = String(filterSlopeToCv(state.slope ?? 12));
    slopeSlider.disabled = slopeConnected;
  }
  const slopeValue = el.querySelector('[data-role="filter-slope-value"]');
  if (slopeValue) slopeValue.textContent = filterSlopeLabel(state.slope ?? 12);

  // Shelf types ignore Q entirely - the "resonance" port drives their shelf gain
  // instead (see audio-engine.js's filter case), so the row says what it does.
  const resonanceLabel = el.querySelector('.port-row-in[data-port-id="resonance"] .port-label');
  if (resonanceLabel) {
    resonanceLabel.textContent = FILTER_SHELF_TYPES.includes(state.filterType) ? "Shelf Gain" : "Resonance";
  }

  const globalToggle = el.querySelector('[data-role="filter-global-switch"]');
  if (globalToggle) globalToggle.checked = state.global === true;
  const typeConnected = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "filterType");
  el.querySelectorAll("[data-filter-type]").forEach((btn) => {
    btn.disabled = typeConnected;
  });
  const typeSlider = el.querySelector('[data-role="filter-type-slider"]');
  if (typeSlider) {
    typeSlider.value = String(filterTypeToCv(state.filterType));
    typeSlider.disabled = typeConnected;
  }
  const typeValue = el.querySelector('[data-role="filter-type-value"]');
  if (typeValue) typeValue.textContent = filterTypeLabel(state.filterType);

  
  
  
  
  
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
    audioEngine.setParam(pendingFilterId, "coeffStride", state.coeffStride);
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



const graphicEqSettingsOverlay = document.getElementById("graphicEqSettingsOverlay");
const graphicEqSettingsError = document.getElementById("graphicEqSettingsError");
const graphicEqFreqInputs = GRAPHIC_EQ_DEFAULT_FREQS.map((_, i) =>
  document.getElementById(`graphicEqFreq${i + 1}Input`));
let pendingGraphicEqId = null;

function setGraphicEqSettingsError(message = "") {
  if (!graphicEqSettingsError) return;
  graphicEqSettingsError.textContent = message;
  graphicEqSettingsError.style.display = message ? "block" : "none";
}

function openGraphicEqSettings(widgetId) {
  const state = graphicEqStates.get(widgetId);
  if (!state) return;
  pendingGraphicEqId = widgetId;
  setGraphicEqSettingsError();
  graphicEqFreqInputs.forEach((input, i) => { input.value = state.frequencies[i]; });
  graphicEqSettingsOverlay.classList.remove("hidden");
}

function closeGraphicEqSettings() {
  graphicEqSettingsOverlay.classList.add("hidden");
  pendingGraphicEqId = null;
}

document.getElementById("graphicEqSettingsCancel").addEventListener("click", closeGraphicEqSettings);

document.getElementById("graphicEqSettingsSave").addEventListener("click", () => {
  if (!pendingGraphicEqId) return;
  const state = graphicEqStates.get(pendingGraphicEqId);
  if (state) {
    const frequencies = graphicEqFreqInputs.map((input) => {
      const hz = parseFloatFlexible(input.value);
      return hz;
    });
    const rangeIndex = frequencies.findIndex((hz) => !Number.isFinite(hz) || hz < 20 || hz > 20000);
    if (rangeIndex >= 0) {
      setGraphicEqSettingsError(`Band ${rangeIndex + 1} muss zwischen 20 und 20000 Hz liegen.`);
      return;
    }
    const integerIndex = frequencies.findIndex((hz) => !Number.isInteger(hz));
    if (integerIndex >= 0) {
      setGraphicEqSettingsError(`Band ${integerIndex + 1} muss eine ganze Zahl sein.`);
      return;
    }
    const orderIndex = frequencies.findIndex((hz, i) => i > 0 && hz <= frequencies[i - 1]);
    if (orderIndex >= 0) {
      setGraphicEqSettingsError(`Band ${orderIndex + 1} muss höher als Band ${orderIndex} liegen.`);
      return;
    }
    state.frequencies = frequencies;
    state.frequencies.forEach((hz, i) => audioEngine.setParam(pendingGraphicEqId, `freq${i + 1}`, hz));
    updateGraphicEqVisual(pendingGraphicEqId);
    sendLiveParamUpdate(pendingGraphicEqId, {
      frequencies: state.frequencies,
      freq1: state.frequencies[0], freq2: state.frequencies[1], freq3: state.frequencies[2], freq4: state.frequencies[3],
      freq5: state.frequencies[4], freq6: state.frequencies[5], freq7: state.frequencies[6], freq8: state.frequencies[7],
    });
  }
  closeGraphicEqSettings();
});



const detuneStates = new Map();

// state.unit: "semitone" (100-cent grid) or "cent" (1-cent grid) - the
// settings-dialog choice (see openDetuneSettings). state.maxOffsetCents is
// always stored in cents regardless of the display unit, matching the
// firmware preset field (nodes/DetuneNode.h).
function detuneStepCents(state) {
  return state.unit === "cent" ? 1 : 100;
}

// Mirrors nodes/DetuneNode.h's detuneOffsetVolts() exactly: the "amount"
// knob's full 0-10V travel is rescaled to +/-maxOffsetCents (5V = 0), then
// rounded to the step grid - same convention as StepArpeggiator's per-step
// pitch knobs.
function detuneOffsetCents(state) {
  const normalized = (state.amount - CV_MAX_VOLTS / 2) / (CV_MAX_VOLTS / 2);
  const rawCents = normalized * state.maxOffsetCents;
  const stepCents = detuneStepCents(state);
  return stepCents > 0 ? Math.round(rawCents / stepCents) * stepCents : rawCents;
}

function detuneOffsetVolts(state) {
  return detuneOffsetCents(state) / 1200;
}

function detuneOffsetLabel(state) {
  const cents = detuneOffsetCents(state);
  if (state.unit === "cent") return `${cents > 0 ? "+" : ""}${cents} ct`;
  const semitones = cents / 100;
  return `${semitones > 0 ? "+" : ""}${semitones} st`;
}

function detuneOutputs(state) {
  return { out: state.cv + detuneOffsetVolts(state) };
}






function recomputeDetune(widgetId, cv, voiceIndex) {
  const state = detuneStates.get(widgetId);
  if (!state) return;
  if (cv !== undefined) state.cv = cv;
  if (state.audioMode) return;
  const { out } = detuneOutputs(state);
  propagate(widgetId, "out", roundFloat(out, 4), voiceIndex);
}

function updateDetuneMode(widgetId) {
  const state = detuneStates.get(widgetId);
  if (!state) return;
  updateDualModeAudioNode(widgetId, "detune", state, ["cv"], {
    amount: state.amount,
    maxOffsetCents: state.maxOffsetCents,
    stepCents: detuneStepCents(state),
  });
  recomputeDetune(widgetId);
}

function updateDetuneVisual(widgetId) {
  const state = detuneStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;

  const slider = el.querySelector('[data-role="detune-amount-slider"]');
  if (slider) slider.value = String(state.amount);
  const valueSpan = el.querySelector('[data-role="detune-amount-value"]');
  if (valueSpan) valueSpan.textContent = detuneOffsetLabel(state);
}

let pendingDetuneId = null;
const detuneSettingsOverlay = document.getElementById("detuneSettingsOverlay");
const detuneMaxOffsetInput = document.getElementById("detuneMaxOffsetInput");
const detuneUnitSelect = document.getElementById("detuneUnitSelect");

function openDetuneSettings(widgetId) {
  const state = detuneStates.get(widgetId);
  if (!state) return;
  pendingDetuneId = widgetId;
  detuneUnitSelect.value = state.unit;
  // Displayed in whatever unit is currently selected - maxOffsetCents
  // itself always stays stored in cents (see detuneStepCents's comment).
  detuneMaxOffsetInput.value = String(state.unit === "cent" ? state.maxOffsetCents : state.maxOffsetCents / 100);
  detuneSettingsOverlay.classList.remove("hidden");
}

function closeDetuneSettings() {
  detuneSettingsOverlay.classList.add("hidden");
  pendingDetuneId = null;
}

document.getElementById("detuneSettingsCancel").addEventListener("click", closeDetuneSettings);

document.getElementById("detuneSettingsSave").addEventListener("click", () => {
  if (!pendingDetuneId) return;
  const state = detuneStates.get(pendingDetuneId);
  if (state) {
    const unit = detuneUnitSelect.value === "cent" ? "cent" : "semitone";
    let entered = parseFloatFlexible(detuneMaxOffsetInput.value);
    if (Number.isNaN(entered) || entered < 0) entered = unit === "cent" ? 1200 : 12;
    state.unit = unit;
    state.maxOffsetCents = unit === "cent" ? entered : entered * 100;
    updateDetuneVisual(pendingDetuneId);
    if (state.audioMode) {
      audioEngine.setParam(pendingDetuneId, "maxOffsetCents", state.maxOffsetCents);
      audioEngine.setParam(pendingDetuneId, "stepCents", detuneStepCents(state));
    }
    recomputeDetune(pendingDetuneId);
    sendLiveParamUpdateThrottled(pendingDetuneId, {
      maxOffsetCents: state.maxOffsetCents,
      stepCents: detuneStepCents(state),
    });
  }
  closeDetuneSettings();
});

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

  const appendZonePort = (zoneIndex, portId, label) => {
    const row = createPortRow(widgetId, { id: portId, label }, "out", "midi-split");
    row.classList.add("midi-split-zone-output");
    row.style.setProperty("--midi-split-zone-color", MIDI_SPLIT_ZONE_COLORS[zoneIndex]);
    portsEl.appendChild(row);
    return row;
  };

  WIDGET_TYPES["midi-split"].inputs.forEach((port) => {
    portsEl.appendChild(createPortRow(widgetId, port, "in", "midi-split"));
  });

  for (let z = 1; z <= state.zoneCount; z += 1) {
    const midiRow = appendZonePort(z - 1, `zone${z}Midi`, `Zone ${z} MIDI`);
    const dot = document.createElement("span");
    dot.className = "adsr-gate-dot midi-split-zone-gate-dot";
    dot.dataset.role = `midi-split-zone${z}-midi-dot`;
    midiRow.insertBefore(dot, midiRow.querySelector(".port-label"));
    appendZonePort(z - 1, `zone${z}Pitch`, `Zone ${z} Pitch`);
    appendZonePort(z - 1, `zone${z}Note`, `Zone ${z} Note`);
    appendZonePort(z - 1, `zone${z}Velocity`, `Zone ${z} Vel`);
    appendZonePort(z - 1, `zone${z}Gate`, `Zone ${z} Gate`);
  }
  layoutPortsGrid(portsEl);
}



const chorusStates = new Map();
const bitcrusherStates = new Map();
const waveFolderStates = new Map();
const panStates = new Map();
const sampleHoldStates = new Map();

const WAVE_FOLDER_SLIDER_PORTS = ["drive", "symmetry", "gain", "mix"];

function formatWaveFolderValue(portId, value) {
  const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  if (portId === "drive") return `${formatNumber(1 + 7 * volts / CV_MAX_VOLTS)}×`;
  if (portId === "symmetry") return `${formatNumber(volts - CV_MAX_VOLTS / 2)} V`;
  return `${formatNumber(volts / CV_MAX_VOLTS * 100)} %`;
}

function updateWaveFolderVisual(widgetId) {
  const state = waveFolderStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  WAVE_FOLDER_SLIDER_PORTS.forEach((portId) => {
    const slider = el.querySelector(`[data-role="wave-folder-${portId}-slider"]`);
    if (slider) slider.value = String(state[portId]);
    const value = el.querySelector(`[data-role="wave-folder-${portId}-value"]`);
    if (value) value.textContent = formatWaveFolderValue(portId, state[portId]);
  });
}

function setWaveFolderParam(widgetId, portId, value) {
  const state = waveFolderStates.get(widgetId);
  if (!state || !WAVE_FOLDER_SLIDER_PORTS.includes(portId)) return;
  state[portId] = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, portId, state[portId]);
  updateWaveFolderVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
}

const envelopeFollowerStates = new Map();
const ENVELOPE_FOLLOWER_SLIDER_PORTS = ["attack", "release", "preGain"];

// Mirrors EnvelopeFollowerNode.h's cv::cvToRange() log-curve mapping - same
// 3 ranges, so the slider readout shows exactly what the DSP is using.
function formatEnvelopeFollowerValue(portId, value) {
  const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  if (portId === "attack") return `${formatNumber(cvToRange(volts, 1, 500, "log"))} ms`;
  if (portId === "release") return `${formatNumber(cvToRange(volts, 10, 2000, "log"))} ms`;
  return `${formatNumber(cvToRange(volts, 0.1, 10, "log"))}×`;
}

function updateEnvelopeFollowerVisual(widgetId) {
  const state = envelopeFollowerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  ENVELOPE_FOLLOWER_SLIDER_PORTS.forEach((portId) => {
    const slider = el.querySelector(`[data-role="envelope-follower-${portId}-slider"]`);
    if (slider) slider.value = String(state[portId]);
    const value = el.querySelector(`[data-role="envelope-follower-${portId}-value"]`);
    if (value) value.textContent = formatEnvelopeFollowerValue(portId, state[portId]);
  });
}

function setEnvelopeFollowerParam(widgetId, portId, value) {
  const state = envelopeFollowerStates.get(widgetId);
  if (!state || !ENVELOPE_FOLLOWER_SLIDER_PORTS.includes(portId)) return;
  state[portId] = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, portId, state[portId]);
  updateEnvelopeFollowerVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
}

const envelopeFollowerSettingsOverlay = document.getElementById("envelopeFollowerSettingsOverlay");
const envelopeFollowerCoeffStrideSelect = document.getElementById("envelopeFollowerCoeffStrideSelect");
let pendingEnvelopeFollowerId = null;

function openEnvelopeFollowerSettings(widgetId) {
  const state = envelopeFollowerStates.get(widgetId);
  if (!state) return;
  pendingEnvelopeFollowerId = widgetId;
  envelopeFollowerCoeffStrideSelect.value = String(state.coeffStride ?? 4);
  envelopeFollowerSettingsOverlay.classList.remove("hidden");
}

function closeEnvelopeFollowerSettings() {
  envelopeFollowerSettingsOverlay.classList.add("hidden");
  pendingEnvelopeFollowerId = null;
}

document.getElementById("envelopeFollowerSettingsCancel").addEventListener("click", closeEnvelopeFollowerSettings);

document.getElementById("envelopeFollowerSettingsSave").addEventListener("click", () => {
  if (!pendingEnvelopeFollowerId) return;
  const state = envelopeFollowerStates.get(pendingEnvelopeFollowerId);
  if (state) {
    const coeffStride = parseInt(envelopeFollowerCoeffStrideSelect.value, 10);
    state.coeffStride = Number.isNaN(coeffStride) ? 4 : clamp(coeffStride, 1, 16);
    audioEngine.setParam(pendingEnvelopeFollowerId, "coeffStride", state.coeffStride);
    sendLiveParamUpdate(pendingEnvelopeFollowerId, { coeffStride: state.coeffStride });
  }
  closeEnvelopeFollowerSettings();
});

const burstGeneratorStates = new Map();
const BURST_GENERATOR_SLIDER_PORTS = ["count", "time", "decay"];

// Mirrors BurstGeneratorNode.h's cv::cvToRange() mapping. "time"'s real-world
// range depends on mode (burst: 5-300ms inter-pulse spacing; delay: 1-2000ms
// delay), so its display must read the widget's current mode, not just its
// own CV value.
function formatBurstGeneratorValue(portId, value, isDelayMode) {
  const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  if (portId === "count") return `${Math.round(cvToRange(volts, 1, 8, "linear"))}`;
  if (portId === "time") {
    return isDelayMode
      ? `${formatNumber(cvToRange(volts, 1, 2000, "log"))} ms`
      : `${formatNumber(cvToRange(volts, 5, 300, "log"))} ms`;
  }
  return `${formatNumber(cvToRange(volts, 0, 0.9, "linear") * 100)} %`;
}

function updateBurstGeneratorVisual(widgetId) {
  const state = burstGeneratorStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const isDelayMode = state.mode >= CV_MAX_VOLTS / 2;
  const select = el.querySelector('[data-role="burst-generator-mode-select"]');
  if (select) select.value = isDelayMode ? "delay" : "burst";
  BURST_GENERATOR_SLIDER_PORTS.forEach((portId) => {
    const slider = el.querySelector(`[data-role="burst-generator-${portId}-slider"]`);
    if (slider) slider.value = String(state[portId]);
    const value = el.querySelector(`[data-role="burst-generator-${portId}-value"]`);
    if (value) value.textContent = formatBurstGeneratorValue(portId, state[portId], isDelayMode);
  });
}

function setBurstGeneratorParam(widgetId, portId, value) {
  const state = burstGeneratorStates.get(widgetId);
  if (!state || !BURST_GENERATOR_SLIDER_PORTS.includes(portId)) return;
  state[portId] = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, portId, state[portId]);
  updateBurstGeneratorVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
}

function setBurstGeneratorMode(widgetId, modeStr) {
  const state = burstGeneratorStates.get(widgetId);
  if (!state) return;
  state.mode = modeStr === "delay" ? CV_MAX_VOLTS : 0;
  audioEngine.setParam(widgetId, "mode", state.mode);
  updateBurstGeneratorVisual(widgetId);
  sendLiveParamUpdate(widgetId, { mode: state.mode });
}

function formatPanValue(value) {
  const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  const pct = Math.round((volts - CV_MAX_VOLTS / 2) / (CV_MAX_VOLTS / 2) * 100);
  if (pct === 0) return "C";
  return pct < 0 ? `${-pct}% L` : `${pct}% R`;
}

function updatePanVisual(widgetId) {
  const state = panStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const slider = el.querySelector('[data-role="pan-pan-slider"]');
  if (slider) slider.value = String(state.pan);
  const value = el.querySelector('[data-role="pan-pan-value"]');
  if (value) value.textContent = formatPanValue(state.pan);
}

function setPanParam(widgetId, value) {
  const state = panStates.get(widgetId);
  if (!state) return;
  state.pan = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, "pan", state.pan);
  updatePanVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { pan: state.pan });
}

function updateSampleHoldVisual(widgetId) {
  const state = sampleHoldStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const thresholdWired = connections.some(
    (c) => c.to.widgetId === widgetId && c.to.portId === "threshold"
  );
  const slider = el.querySelector('[data-role="sample-hold-threshold-slider"]');
  if (slider) {
    slider.value = String(state.threshold);
    slider.disabled = thresholdWired;
  }
  const value = el.querySelector('[data-role="sample-hold-threshold-value"]');
  if (value) value.textContent = `${formatNumber(state.threshold)} V`;
}

function setSampleHoldThreshold(widgetId, value) {
  const state = sampleHoldStates.get(widgetId);
  if (!state) return;
  state.threshold = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
  audioEngine.setParam(widgetId, "threshold", state.threshold);
  updateSampleHoldVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { threshold: state.threshold });
}

function updateBitcrusherVisual(widgetId) {
  const state = bitcrusherStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const slider = el.querySelector('[data-role="bitcrusher-crush-slider"]');
  if (slider) slider.value = String(state.crush);
  const value = el.querySelector('[data-role="bitcrusher-crush-value"]');
  if (value) value.textContent = formatNumber(state.crush);
  const bypass = el.querySelector('[data-role="bitcrusher-bypass-switch"]');
  if (bypass) bypass.checked = state.bypass;
}

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
  "reverb-light": [
    { portId: "roomSize", paramName: "roomSize", label: "Room Size", unit: "", defaultMin: 0, defaultMax: 1, defaultCurve: "linear", defaultValue: 0.5 },
    { portId: "damping", paramName: "damping", label: "Damping", unit: "", defaultMin: 0, defaultMax: 1, defaultCurve: "linear", defaultValue: 0.5 },
    { portId: "wetLevel", paramName: "wetLevelDb", label: "Wet Level", unit: "dB", defaultMin: -96, defaultMax: 0, defaultCurve: "linear", defaultValue: -6 },
    { portId: "dryLevel", paramName: "dryLevelDb", label: "Dry Level", unit: "dB", defaultMin: -96, defaultMax: 0, defaultCurve: "linear", defaultValue: 0 },
  ],
  // 8-band graphic EQ: each band's gain is a bipolar CV (0-10V, 5V = 0 dB
  // center, min/max -15..15 dB matches esp-eq's own gainDb clamp range) -
  // frequency is NOT here, it's settings-dialog-only (see graphicEqStates/
  // openGraphicEqSettings), not CV-controllable.
  "graphic-eq": GRAPHIC_EQ_DEFAULT_FREQS.map((hz, i) => ({
    portId: `band${i + 1}`,
    paramName: `gain${i + 1}`,
    label: formatGraphicEqHzLabel(hz),
    unit: "dB",
    defaultMin: -15,
    defaultMax: 15,
    defaultCurve: "linear",
    defaultValue: 0,
  })),
};



function isReverbType(type) {
  return type === "reverb-light";
}

// Types whose "bypass" flows through the generic effectCvStates/
// makeEffectCvEngine machinery below (reverb-light already did; delay/
// graphic-eq get bypass the same way). Chorus/esp-eq have their OWN state maps
// (chorusStates/espEffectStates) and do NOT go through this engine - see
// hasBypass() below for the full cross-type set used by the shared
// toggle-switch UI.
function isEffectCvBypassType(type) {
  return isReverbType(type) || type === "delay" || type === "graphic-eq";
}

// Every widget type that gets a Bypass toggle switch (see the port-row
// loop below and BYPASS_STATE_MAPS just after it) - reverb-light/delay
// share effectCvStates via isEffectCvBypassType(); chorus/esp-eq each keep
// their own state map.
function hasBypass(type) {
  return (
    isEffectCvBypassType(type) ||
    type === "bitcrusher" ||
    type === "chorus" ||
    type === "esp-eq"
  );
}

// Resolves the right per-widget state Map for a bypass-capable type (see
// hasBypass() above) - used by the shared toggle-switch UI so it doesn't
// need a type-by-type if/else at every call site.
function bypassStateMapFor(type) {
  if (isEffectCvBypassType(type)) return effectCvStates;
  if (type === "bitcrusher") return bitcrusherStates;
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

function updateGraphicEqVisual(widgetId) {
  updateEffectCvVisual(widgetId);
  const el = document.getElementById(widgetId);
  const state = graphicEqStates.get(widgetId);
  if (!el || !state) return;
  const gainSlider = el.querySelector('[data-role="graphic-eq-gain-slider"]');
  if (gainSlider) gainSlider.value = String(state.outputGain);
  const gainValue = el.querySelector('[data-role="graphic-eq-gain-value"]');
  if (gainValue) gainValue.textContent = formatNumber(state.outputGain);
  state.frequencies.forEach((hz, i) => {
    const row = el.querySelector(`[data-port-id="band${i + 1}"] .port-label`);
    if (row) row.textContent = formatGraphicEqHzLabel(hz);
    const label = el.querySelector(`[data-role="graphic-eq-band-labels"] .graphic-eq-band-label:nth-child(${i + 1})`);
    if (label) {
      label.textContent = formatGraphicEqAxisLabel(hz);
      label.style.left = `${graphicEqLogPosition(hz, audioEngine.ctx?.sampleRate || 44100) * 100}%`;
    }
  });
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
  const effectCvTitleLabel = type === "delay" ? "Delay" : "Reverb";
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



const OUTPUT_MULTIPLEXER_MIN_OUTPUTS = 2;
const OUTPUT_MULTIPLEXER_MAX_OUTPUTS = 16;
const outputMultiplexerStates = new Map();

function outputMultiplexerHasCvConnection(widgetId) {
  return connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "select");
}

function setOutputMultiplexerManualIndex(widgetId, clientY) {
  const state = outputMultiplexerStates.get(widgetId);
  const selector = document.getElementById(widgetId)?.querySelector(".multiplexer-switch");
  if (!state || !selector || outputMultiplexerHasCvConnection(widgetId)) return;
  const rect = selector.getBoundingClientRect();
  if (rect.height <= 0) return;
  const fraction = clamp((clientY - rect.top) / rect.height, 0, 0.999999);
  const idx = clamp(Math.floor(fraction * state.outputCount), 0, state.outputCount - 1);
  if (idx === state.manualSelect && idx === state.activeIndex) return;
  state.manualSelect = idx;
  sendLiveParamUpdateThrottled(widgetId, { select: idx });
  outputMultiplexerSetActiveIndex(widgetId, idx);
}

function renderOutputMultiplexerPorts(widgetId, portsEl) {
  const state = outputMultiplexerStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  portsEl.appendChild(createPortRow(widgetId, { id: "in", label: "In" }, "in", "output-multiplexer"));
  portsEl.appendChild(createPortRow(widgetId, { id: "select", label: "Select (CV)" }, "in", "output-multiplexer"));

  for (let i = 1; i <= state.outputCount; i += 1) {
    const row = createPortRow(widgetId, { id: `out${i}`, label: `Out ${i}` }, "out", "output-multiplexer");
    row.classList.add("mux-out-row");
    row.dataset.muxIndex = String(i - 1);
    if (i - 1 === state.activeIndex) row.classList.add("active");
    portsEl.appendChild(row);
  }
  layoutPortsGrid(portsEl);
}

// Sets live-active output index from connected "select" CV, refreshes the
// highlighted port row + selector - mirrors multiplexerSetActiveIndex().
// Deliberately does NOT re-propagate the current "in" value to the newly
// active output: that would overwrite the output's own held value with
// whatever happens to be on "in" right now, even though nothing actually
// changed there - the newly active output must keep showing whatever it
// last actually received (matches the firmware's held[] freeze). Only a
// genuine change on "in" (see deliverToWidget's "output-multiplexer" case)
// may ever propagate.
function outputMultiplexerSetActiveIndex(widgetId, idx) {
  const state = outputMultiplexerStates.get(widgetId);
  if (!state) return;
  const nextIndex = clamp(Math.round(Number(idx) || 0), 0, Math.max(0, state.outputCount - 1));
  const changed = state.activeIndex !== nextIndex;
  state.activeIndex = nextIndex;
  if (changed) audioEngine.setParam(widgetId, "select", state.activeIndex);
  updateOutputMultiplexerVisual(widgetId);
}

function updateOutputMultiplexerVisual(widgetId) {
  const state = outputMultiplexerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const cvConnected = outputMultiplexerHasCvConnection(widgetId);
  const selector = el.querySelector(".multiplexer-switch");
  if (selector) selector.classList.toggle("cv-connected", cvConnected);
  if (cvConnected && Number.isFinite(state.cvVolts)) {
    state.activeIndex = multiplexerSelectFromCv(state.cvVolts, state.outputCount);
  }
  el.querySelectorAll(".mux-out-row").forEach((row) => {
    row.classList.toggle("active", parseInt(row.dataset.muxIndex, 10) === state.activeIndex);
  });

  const thumb = el.querySelector('[data-role="output-multiplexer-switch-thumb"]');
  const firstOutputRow = el.querySelector(".mux-out-row");
  if (thumb && firstOutputRow) {
    const rowHeight = firstOutputRow.offsetHeight || 26;
    thumb.style.transform = `translateY(${state.activeIndex * rowHeight}px)`;
  }
  updateOutputMultiplexerRouting(widgetId);
}

// Mirror of updateMultiplexerRouting(), geometrically inverted: the fixed
// endpoint is the single "in" port dot (left edge) instead of the single
// output dot (right edge), and the moving endpoint is still the selector
// thumb - the bezier-path math itself is untouched, only which DOM element
// feeds `start` vs `output` swaps.
function updateOutputMultiplexerRouting(widgetId) {
  const el = document.getElementById(widgetId);
  const content = el?.querySelector(".output-multiplexer-content");
  const path = el?.querySelector('[data-role="output-multiplexer-routing-path"]');
  const activeRow = el?.querySelector(".mux-out-row.active");
  const thumb = el?.querySelector('[data-role="output-multiplexer-switch-thumb"]');
  const inputDot = el?.querySelector('.port-row-in .port-dot[data-port-id="in"]');
  if (!content || !path || !activeRow || !thumb || !inputDot) return;
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
  const start = localPoint(inputDot.getBoundingClientRect(), 0.5, 0.5);
  const output = localPoint(thumb.getBoundingClientRect(), 0, 0.5);
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
// shows the friendlier 0-8 physical-register reading.
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
      if (port.id === "gain") {
        // Same relocate-to-bottom/distinct-accent treatment as every other
        // MAIN_GAIN_WIDGET_TYPES row - Hammond builds its own ports via
        // renderHammondPorts instead of the generic def.inputs loop, so it
        // has to tag this itself (see that loop's own gain-control-row
        // comment above).
        row.classList.add("gain-control-row");
        row.dataset.role = "main-gain-control";
      }
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

// Builds one clock/gate/pitch/direction/cv/gateOut row without createPortRow's
// own .port-row-in/-out class (dot + label are reused from it, the row is
// not) - see renderStepArpPorts's comment for why: the app-wide
// layoutPortsGrid() pass every widget gets after this function returns
// unconditionally (a) shoves every .port-row-in to the end of the ports
// list and (b) absolutely-positions every .port-row-out into a top-right
// slot sized for the usual "a few inputs, one output" node shape - both
// wrong for this node's two-column-top / knob-row-bottom layout. Plain rows
// (this function's own step-arp-io-line class) are invisible to that pass
// and keep the exact position/order this renderer puts them in, while the
// CSS mirrors .port-row's own padding/gap/font and .port-row-in/-out's own
// edge-flush negative-margin dot trick 1:1 so they still look identical to
// every other widget's port rows.
const STEP_ARP_DIRECTION_LABELS = { forward: "Forward", backward: "Backward", pingpong: "Ping-Pong", random: "Random" };

function stepArpDirectionLabel(volts) {
  return STEP_ARP_DIRECTION_LABELS[stepArpDirectionFromCv(volts)];
}

function buildStepArpIoLine(widgetId, port, direction) {
  const state = stepArpStates.get(widgetId);
  const sourceRow = createPortRow(widgetId, port, direction, "step-arpeggiator");
  const dot = sourceRow.querySelector(".port-dot"); // keeps its startConnectionDrag mousedown handler (output only)
  const label = sourceRow.querySelector(".port-label");
  const row = document.createElement("div");
  row.className = `step-arp-io-line step-arp-io-line-${direction}`;
  if (direction === "out") {
    row.appendChild(label);
    row.appendChild(dot);
    return row;
  }
  row.appendChild(dot);
  row.appendChild(label);

  // "direction" and "pulseTime" get an inline slider (local knob, same
  // "wired cable wins outright" convention as the sixteen step knobs) - the
  // other three fixed inputs (clock/gate/pitch) are cable-only, no local
  // control, unchanged.
  if (port.id === "direction" && state) {
    const slider = document.createElement("input");
    slider.type = "range";
    slider.className = "port-slider";
    slider.min = "0";
    slider.max = String(CV_MAX_VOLTS);
    slider.step = "0.01";
    slider.value = String(state.directionVolts);
    slider.dataset.role = "step-arp-direction-slider";
    slider.addEventListener("mousedown", (e) => e.stopPropagation());
    slider.addEventListener("click", (e) => e.stopPropagation());
    slider.addEventListener("input", () => setStepArpDirection(widgetId, parseFloat(slider.value)));
    row.appendChild(slider);

    const valueSpan = document.createElement("span");
    valueSpan.className = "port-value";
    valueSpan.dataset.role = "step-arp-direction-value";
    valueSpan.textContent = stepArpDirectionLabel(state.directionVolts);
    row.appendChild(valueSpan);
  }
  if (port.id === "pulseTime" && state) {
    const slider = document.createElement("input");
    slider.type = "range";
    slider.className = "port-slider";
    slider.min = "0";
    slider.max = String(CV_MAX_VOLTS);
    slider.step = "0.01";
    slider.value = String(rangeToCv(state.pulseMs / 1000, state.pulseTimeMinSec, state.pulseTimeMaxSec, "linear"));
    slider.dataset.role = "step-arp-pulsetime-slider";
    slider.addEventListener("mousedown", (e) => e.stopPropagation());
    slider.addEventListener("click", (e) => e.stopPropagation());
    slider.addEventListener("input", () => setStepArpPulseTime(widgetId, parseFloat(slider.value)));
    row.appendChild(slider);

    const valueSpan = document.createElement("span");
    valueSpan.className = "port-value";
    valueSpan.dataset.role = "step-arp-pulsetime-value";
    valueSpan.textContent = formatStepArpPulseTime(widgetId);
    row.appendChild(valueSpan);
  }
  if (port.id === "rate" && state) {
    const slider = document.createElement("input");
    slider.type = "range";
    slider.className = "port-slider";
    slider.min = "0";
    slider.max = String(CV_MAX_VOLTS);
    slider.step = "0.01";
    slider.value = String(state.clockCvVolts);
    slider.dataset.role = "step-arp-rate-slider";
    slider.addEventListener("mousedown", (e) => e.stopPropagation());
    slider.addEventListener("click", (e) => e.stopPropagation());
    slider.addEventListener("input", () => setStepArpRate(widgetId, parseFloat(slider.value)));
    row.appendChild(slider);

    const valueSpan = document.createElement("span");
    valueSpan.className = "port-value";
    valueSpan.dataset.role = "step-arp-rate-value";
    valueSpan.textContent = `${formatNumber(stepArpRateBpm(widgetId))} BPM`;
    row.appendChild(valueSpan);
  }
  return row;
}

// Local "direction" slider drag - see buildStepArpIoLine. A genuine cable
// still wins outright in the firmware (nodes/StepArpeggiatorNode.h's
// portSample() only ever falls back to this local value when unwired); the
// disabled styling here (see updateStepArpDirectionVisual) is purely a
// browser-side "don't bother turning this, it's overridden" cue.
function setStepArpDirection(widgetId, volts) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  state.directionVolts = clamp(volts, 0, CV_MAX_VOLTS);
  updateStepArpDirectionVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { direction: state.directionVolts });
}

// Local "pulseTime" slider drag. state.pulseMs is ALREADY the resolved,
// real-unit value (matches nodes/StepArpeggiatorNode.h's own local
// fallback field 1:1) - the slider's own 0-10V raw position is derived from
// it (rangeToCv) rather than stored, same as this file's existing
// effectCvStates.
function setStepArpPulseTime(widgetId, volts) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  const clampedVolts = clamp(volts, 0, CV_MAX_VOLTS);
  state.pulseMs = cvToRange(clampedVolts, state.pulseTimeMinSec, state.pulseTimeMaxSec, "linear") * 1000;
  updateStepArpPulseTimeVisual(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { pulseMs: state.pulseMs });
}

function formatStepArpPulseTime(widgetId) {
  const state = stepArpStates.get(widgetId);
  if (!state) return "";
  return `${Math.round(state.pulseMs)} ms`;
}

function updateStepArpDirectionVisual(widgetId) {
  const el = document.getElementById(widgetId);
  const state = stepArpStates.get(widgetId);
  if (!el || !state) return;
  const slider = el.querySelector('[data-role="step-arp-direction-slider"]');
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "direction" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  if (slider) {
    slider.value = String(state.directionVolts);
    slider.disabled = audioConnected;
  }
  const valueSpan = el.querySelector('[data-role="step-arp-direction-value"]');
  if (valueSpan) valueSpan.textContent = stepArpDirectionLabel(state.directionVolts);
}

function updateStepArpPulseTimeVisual(widgetId) {
  const el = document.getElementById(widgetId);
  const state = stepArpStates.get(widgetId);
  if (!el || !state) return;
  const volts = rangeToCv(state.pulseMs / 1000, state.pulseTimeMinSec, state.pulseTimeMaxSec, "linear");
  const slider = el.querySelector('[data-role="step-arp-pulsetime-slider"]');
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "pulseTime" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  if (slider) {
    slider.value = String(volts);
    slider.disabled = audioConnected;
  }
  const valueSpan = el.querySelector('[data-role="step-arp-pulsetime-value"]');
  if (valueSpan) valueSpan.textContent = formatStepArpPulseTime(widgetId);
}

// Resolved BPM of the internal fallback clock - pure function of state, safe
// to call before the widget is even in the DOM (see buildStepArpIoLine's
// initial "rate" row text, same reasoning as formatStepArpPulseTime above).
// Unrelated to the device-wide Master Clock (main.js's masterClockBpm) -
// this is Step Arpeggiator's own "rate" fallback, only used while its
// "clock" input is itself unwired (see StepArpeggiatorNode.h).
function stepArpRateBpm(widgetId) {
  const state = stepArpStates.get(widgetId);
  if (!state) return 0;
  const bpm = (state.clockCvVolts / CV_MAX_VOLTS) * state.clockMaxBpm;
  return state.clockRoundBpm ? Math.round(bpm) : bpm;
}

// Local "rate" slider drag - see buildStepArpIoLine. Same wired-cable-wins
// convention as direction/pulseTime; restarts the internal clock (below) so
// a tempo change takes effect immediately instead of waiting for the next
// tick's own interval to be recomputed.
function setStepArpRate(widgetId, volts) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  state.clockCvVolts = clamp(volts, 0, CV_MAX_VOLTS);
  updateStepArpRateVisual(widgetId);
  stepArpRestartInternalClock(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { clockCvVolts: state.clockCvVolts });
}

function updateStepArpRateVisual(widgetId) {
  const el = document.getElementById(widgetId);
  const state = stepArpStates.get(widgetId);
  if (!el || !state) return;
  const slider = el.querySelector('[data-role="step-arp-rate-slider"]');
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === "rate" &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  if (slider) {
    slider.value = String(state.clockCvVolts);
    slider.disabled = audioConnected;
  }
  const valueSpan = el.querySelector('[data-role="step-arp-rate-value"]');
  if (valueSpan) valueSpan.textContent = `${formatNumber(stepArpRateBpm(widgetId))} BPM`;
}

// Whether a genuine cable feeds "clock" right now - the internal fallback
// generator below only ever runs while this is false (plan/user request:
// "wenn nichts angeschlossen ist am Clockport, dann soll [die eigene Clock]
// genommen werden").
function stepArpClockPortWired(widgetId) {
  return connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "clock");
}

function stepArpStopInternalClock(widgetId) {
  const state = stepArpStates.get(widgetId);
  if (state && state.clockTickTimer) {
    clearInterval(state.clockTickTimer);
    state.clockTickTimer = null;
  }
}

// Editor-side preview twin of nodes/StepArpeggiatorNode.h's render() internal-
// clock branch: a setInterval standing in for that sample-accurate phase
// accumulator (same simplification Clock's own restartClockTimer already
// makes for its "pulse" output). Always tears down and rebuilds from
// scratch rather than adjusting the running interval - restarting IS the
// resync (its next tick becomes exactly one period from now), which is
// exactly what a Gate rising edge needs (see deliverToWidget's "gate"
// branch below) and is harmless everywhere else this gets called from
// (rate changes, settings-dialog saves, connection changes).
function stepArpRestartInternalClock(widgetId) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  stepArpStopInternalClock(widgetId);
  if (stepArpClockPortWired(widgetId)) return; // external cable always wins
  const bpm = stepArpRateBpm(widgetId);
  if (bpm <= 0) return;
  const intervalMs = 60000 / bpm;
  state.clockTickTimer = setInterval(() => {
    const el = document.getElementById(widgetId);
    if (!el || state.gateVolts < GATE_THRESHOLD_VOLTS) return;
    stepArpTriggerStep(widgetId, state, el);
  }, intervalMs);
}

// CV Step-Arpeggiator: a top two-column strip (clock/gate/pitch/direction on
// the left, cv/gateOut on the right - both edge-flush like every other
// widget's ports, see buildStepArpIoLine above) over a dense bottom row of
// sixteen small round knobs for the "stepN" CV inputs (NOT the usual
// vertical port-row list - see documentation/cv-step-arpeggiator-plan.md
// section "Renderer"), with THEIR OWN dots flush against the widget's
// bottom edge.
function renderStepArpPorts(widgetId, portsEl) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  const ioRow = document.createElement("div");
  ioRow.className = "step-arp-io-row";

  const inputsCol = document.createElement("div");
  inputsCol.className = "step-arp-io-col step-arp-io-col-in";
  WIDGET_TYPES["step-arpeggiator"].inputs
    .filter((port) => !port.id.startsWith("step"))
    .forEach((port) => inputsCol.appendChild(buildStepArpIoLine(widgetId, port, "in")));
  ioRow.appendChild(inputsCol);

  const outputsCol = document.createElement("div");
  outputsCol.className = "step-arp-io-col step-arp-io-col-out";
  WIDGET_TYPES["step-arpeggiator"].outputs.forEach((port) => {
    outputsCol.appendChild(buildStepArpIoLine(widgetId, port, "out"));
  });
  ioRow.appendChild(outputsCol);

  portsEl.appendChild(ioRow);

  const stepsRow = document.createElement("div");
  stepsRow.className = "step-arp-steps-row";
  for (let i = 0; i < state.stepCount; i += 1) {
    const portId = `step${i + 1}`;

    // Reuse createPortRow's own dot (same class/kind computation as every
    // other input port) rather than hand-rolling a second copy of that
    // logic - only the dot element itself is kept, the row/label are not.
    const dot = createPortRow(widgetId, { id: portId, label: `Step ${i + 1}` }, "in", "step-arpeggiator").querySelector(
      ".port-dot"
    );

    const knob = document.createElement("div");
    knob.className = "step-arp-knob";
    knob.dataset.role = `step-arp-knob-${i + 1}`;
    knob.title = `Step ${i + 1}`;
    const indicator = document.createElement("div");
    indicator.className = "step-arp-knob-indicator";
    indicator.dataset.role = `step-arp-knob-${i + 1}-indicator`;
    knob.appendChild(indicator);
    knob.addEventListener("mousedown", (e) => startStepArpKnobDrag(e, widgetId, i));
    // Double-click resets this step back to its 5V default - same shortcut
    // convention as the widget-level dblclick opening settings, just
    // stopped from bubbling there since this one means something else here.
    knob.addEventListener("dblclick", (e) => {
      e.preventDefault();
      e.stopPropagation();
      setStepArpValue(widgetId, i, STEP_ARP_DEFAULT_VOLTS);
      markDirty();
    });

    const valueSpan = document.createElement("div");
    valueSpan.className = "step-arp-value";
    valueSpan.dataset.role = `step-arp-value-${i + 1}`;

    const noteSpan = document.createElement("div");
    noteSpan.className = "step-arp-note";
    noteSpan.dataset.role = `step-arp-note-${i + 1}`;

    const dotRow = document.createElement("div");
    dotRow.className = "step-arp-dot-row";
    dotRow.appendChild(dot);

    const cell = document.createElement("div");
    cell.className = "step-arp-cell";
    cell.appendChild(knob);
    cell.appendChild(valueSpan);
    cell.appendChild(noteSpan);
    cell.appendChild(dotRow);
    stepsRow.appendChild(cell);
  }
  portsEl.appendChild(stepsRow);

  updateStepArpVisual(widgetId);
  updateStepArpActiveHighlight(widgetId);
  stepArpRestartInternalClock(widgetId);
}

// Same vertical-drag-to-value math as startKnobDrag (main.js), just
// operating directly in the 0-10V CV domain instead of a 0-127 raw+mapping
// indirection - these step knobs ARE already the pitch-CV value, no
// separate output-range mapping is needed.
function startStepArpKnobDrag(e, widgetId, index) {
  e.preventDefault();
  e.stopPropagation();
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  const startY = e.clientY;
  const startVolts = state.steps[index];
  let moved = false;

  function onMove(ev) {
    const dy = startY - ev.clientY;
    if (Math.abs(dy) > 2) moved = true;
    setStepArpValue(widgetId, index, startVolts + (dy / 254) * CV_MAX_VOLTS);
  }
  function onUp() {
    document.removeEventListener("mousemove", onMove);
    document.removeEventListener("mouseup", onUp);
    if (moved) markDirty();
  }
  document.addEventListener("mousemove", onMove);
  document.addEventListener("mouseup", onUp);
}

// Local knob drag AND a live cable both land here via deliverToWidget's
// step-arpeggiator branch - same pattern as Hammond's drawbarN (see
// renderHammondPorts above).
function setStepArpValue(widgetId, index, volts) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  const clamped = clamp(volts, 0, CV_MAX_VOLTS);
  state.steps[index] = clamped;
  updateStepArpKnobVisual(widgetId, index);
  sendLiveParamUpdateThrottled(widgetId, { [`step${index + 1}`]: clamped });
}

function updateStepArpKnobVisual(widgetId, index) {
  const state = stepArpStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const volts = state.steps[index];
  const indicator = el.querySelector(`[data-role="step-arp-knob-${index + 1}-indicator"]`);
  if (indicator) indicator.style.transform = `rotate(${-135 + (volts / CV_MAX_VOLTS) * 270}deg)`;
  // Unlike the direction/pulseTime local-slider-override checks elsewhere in
  // this file, "pitch" has no local knob to override - ANY cable here
  // (regardless of source kind) already changes what actually sounds, so the
  // display switches on connection alone, not just "audio"-kind ones.
  const pitchWired = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "pitch");
  const valueSpan = el.querySelector(`[data-role="step-arp-value-${index + 1}"]`);
  if (valueSpan) {
    valueSpan.classList.toggle("hidden", pitchWired);
    if (!pitchWired) valueSpan.textContent = `${formatNumber(volts)} V`;
  }
  const noteSpan = el.querySelector(`[data-role="step-arp-note-${index + 1}"]`);
  if (noteSpan) {
    if (pitchWired) {
      // Matches nodes/StepArpeggiatorNode.h's triggerStep() exactly: the
      // knob's full 0-10V travel is RESCALED to +/-maxSemitoneOffset (not
      // clamped), then rounded - this IS the offset that actually gets
      // added to Pitch, not just the raw knob position.
      const normalized = (volts - CV_MAX_VOLTS / 2) / (CV_MAX_VOLTS / 2);
      const semitones = Math.round(normalized * state.maxSemitoneOffset);
      noteSpan.textContent = `${semitones > 0 ? "+" : ""}${semitones}`;
    } else {
      noteSpan.textContent = pitchCvToNoteName(volts);
    }
  }
  // A wired step port overrides the local knob outright - same disable
  // convention as Hammond's drawbar sliders (see updateHammondDrawbarVisual).
  const audioConnected = connections.some(
    (c) =>
      c.to.widgetId === widgetId &&
      c.to.portId === `step${index + 1}` &&
      getPortKind(c.from.widgetId, c.from.portId, "out") === "audio"
  );
  const knob = el.querySelector(`[data-role="step-arp-knob-${index + 1}"]`);
  if (knob) knob.classList.toggle("step-arp-knob-disabled", audioConnected);
}

function updateStepArpVisual(widgetId) {
  const state = stepArpStates.get(widgetId);
  if (!state) return;
  for (let i = 0; i < state.stepCount; i += 1) {
    updateStepArpKnobVisual(widgetId, i);
  }
  updateStepArpDirectionVisual(widgetId);
  updateStepArpPulseTimeVisual(widgetId);
  updateStepArpRateVisual(widgetId);
}

// Highlights the cell of the step currently held at the "cv" output (see
// StepArpState::currentStep in nodes/StepArpeggiatorNode.h) - purely a
// display of stepArpStates' own client-side-simulated position, updated by
// the "clock"/"gate" branches of deliverToWidget below.
function updateStepArpActiveHighlight(widgetId) {
  const state = stepArpStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".step-arp-cell").forEach((cell, index) => {
    cell.classList.toggle("step-arp-cell-active", index === state.currentStep);
  });
}

// direction (0-10V, four fixed bands) - JS mirror of
// nodes::steparp::directionFromCv() in nodes/StepArpeggiatorNode.h.
function stepArpDirectionFromCv(volts) {
  if (volts < 2.5) return "forward";
  if (volts < 5) return "backward";
  if (volts < 7.5) return "pingpong";
  return "random";
}

// JS mirror of nodes::steparp::nextStepIndex() (same file) - advances
// state.currentStep (and state.pingPongForward for the Ping-Pong sub-state)
// to the step that will be held after THIS trigger. Mutates `state`
// in place, matching the C++ function's reference-parameter shape.
function stepArpNextIndex(state) {
  const stepCount = state.stepCount;
  const current = state.currentStep;
  if (stepCount <= 1) return 0;
  switch (stepArpDirectionFromCv(state.directionVolts)) {
    case "forward": {
      const next = current + 1;
      return next >= stepCount ? 0 : next;
    }
    case "backward":
      return current === 0 ? stepCount - 1 : current - 1;
    case "pingpong":
      if (state.pingPongForward) {
        if (current >= stepCount - 1) {
          state.pingPongForward = false;
          return current - 1;
        }
        return current + 1;
      }
      if (current === 0) {
        state.pingPongForward = true;
        return current + 1;
      }
      return current - 1;
    case "random":
    default:
      return Math.floor(Math.random() * stepCount);
  }
}

// Plays state.currentStep (sample-and-holds its CV + starts the gateOut
// pulse/LED), then advances state.currentStep to the FOLLOWING step - the
// editor-side mirror of nodes/StepArpeggiatorNode.h's triggerStep(), shared
// by both trigger sources: a qualifying clock edge, and the Gate's own
// rising edge (which plays step 1 immediately instead of waiting for the
// first clock edge afterward).
function stepArpTriggerStep(widgetId, state, el) {
  const stepVolts = state.steps[state.currentStep];
  // "pitch" - when wired, a step's own knob value is a +/-N-semitone offset
  // from its own 5V center, added on top of the incoming Pitch CV (0
  // semitones at the knob's 5V default + a 5V Pitch signal must produce
  // exactly 5V out, not 10V), the knob's full travel rescaled to
  // +/-maxSemitoneOffset and rounded to a whole semitone - see
  // nodes/StepArpeggiatorNode.h's triggerStep() for the matching firmware
  // math and updateStepArpKnobVisual for the matching knob display.
  const pitchWired = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "pitch");
  let offsetVolts = 0;
  if (pitchWired) {
    const normalized = (stepVolts - CV_MAX_VOLTS / 2) / (CV_MAX_VOLTS / 2);
    offsetVolts = Math.round(normalized * state.maxSemitoneOffset) / 12;
  }
  state.heldCv = clamp(pitchWired ? state.pitchVolts + offsetVolts : stepVolts, 0, CV_MAX_VOLTS);
  propagate(widgetId, "cv", state.heldCv);
  updateStepArpActiveHighlight(widgetId);

  const outDot = el.querySelector('[data-port-id="gateOut"][data-port-dir="out"]');
  setLedActive(outDot, true);
  propagate(widgetId, "gateOut", GATE_ON_VOLTS);
  clearTimeout(state.gateOutTimer);
  state.gateOutTimer = setTimeout(() => {
    setLedActive(outDot, false);
    propagate(widgetId, "gateOut", GATE_OFF_VOLTS);
  }, state.pulseMs);

  state.currentStep = stepArpNextIndex(state);
}

let drumPaintSession = null;

function drumSequencerTotalSteps(state) {
  return state.stepCount;
}

function drumSequencerCellIndex(state, track, step) {
  return track * drumSequencerTotalSteps(state) + step;
}

function drumSequencerActivePattern(state) {
  return state.patterns[state.activePatternIndex];
}

// Splits 0-10V into `count` equal bands - mirrors patternIndexFromCv in
// nodes/DrumSequencerNode.h so wired-CV pattern selection matches playback.
function drumPatternIndexFromCv(volts, count) {
  if (count <= 1) return 0;
  const idx = Math.floor(volts / (CV_MAX_VOLTS / count));
  return clamp(idx, 0, count - 1);
}

function drumSequencerEncodedCell(state, index) {
  const pattern = drumSequencerActivePattern(state);
  const volts = clamp(Number(pattern.velocities[index]) || 0, 0, CV_MAX_VOLTS);
  return pattern.active[index] ? volts : -(volts + 1);
}

function buildDrumSequencerPort(widgetId, port, direction) {
  const source = createPortRow(widgetId, port, direction, "drum-sequencer");
  const dot = source.querySelector(".port-dot");
  const label = source.querySelector(".port-label");
  const line = document.createElement("div");
  line.className = `drum-sequencer-port drum-sequencer-port-${direction}`;
  if (direction === "out") {
    line.appendChild(label);
    line.appendChild(dot);
  } else {
    line.appendChild(dot);
    line.appendChild(label);
  }
  return line;
}

function renderDrumSequencerPorts(widgetId, portsEl) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  portsEl.innerHTML = "";

  const inputs = document.createElement("div");
  inputs.className = "drum-sequencer-inputs";
  const clockLine = buildDrumSequencerPort(widgetId, { id: "clock", label: "Clock" }, "in");
  inputs.appendChild(clockLine);
  const gateLine = buildDrumSequencerPort(widgetId, { id: "gate", label: "Gate" }, "in");
  inputs.appendChild(gateLine);

  const patternLine = buildDrumSequencerPort(widgetId, { id: "pattern", label: "Pattern" }, "in");
  const patternSlider = document.createElement("input");
  patternSlider.type = "range";
  patternSlider.min = "0";
  patternSlider.max = String(Math.max(0, state.patterns.length - 1));
  patternSlider.step = "1";
  patternSlider.className = "port-slider drum-sequencer-pattern-select";
  patternSlider.dataset.role = "drum-sequencer-pattern-select";
  patternSlider.addEventListener("mousedown", (e) => e.stopPropagation());
  patternSlider.addEventListener("input", () => setDrumSequencerActivePattern(widgetId, Number(patternSlider.value)));
  const patternValue = document.createElement("span");
  patternValue.className = "port-value";
  patternValue.dataset.role = "drum-sequencer-pattern-value";
  patternLine.appendChild(patternSlider);
  patternLine.appendChild(patternValue);
  inputs.appendChild(patternLine);

  const rateLine = buildDrumSequencerPort(widgetId, { id: "rate", label: "Rate" }, "in");
  const rateSlider = document.createElement("input");
  rateSlider.type = "range";
  rateSlider.min = "0";
  rateSlider.max = String(CV_MAX_VOLTS);
  rateSlider.step = "0.05";
  rateSlider.className = "port-slider drum-sequencer-rate";
  rateSlider.dataset.role = "drum-sequencer-rate";
  rateSlider.addEventListener("mousedown", (e) => e.stopPropagation());
  rateSlider.addEventListener("input", () => setDrumSequencerRate(widgetId, Number(rateSlider.value)));
  const rateValue = document.createElement("span");
  rateValue.className = "port-value";
  rateValue.dataset.role = "drum-sequencer-rate-value";
  rateLine.appendChild(rateSlider);
  rateLine.appendChild(rateValue);
  inputs.appendChild(rateLine);

  const switchAtEndLine = buildDrumSequencerPort(widgetId, { id: "switchAtEnd", label: "Switch @ End" }, "in");
  const switchAtEndLabel = document.createElement("label");
  switchAtEndLabel.className = "switch";
  const switchAtEndToggle = document.createElement("input");
  switchAtEndToggle.type = "checkbox";
  switchAtEndToggle.checked = state.switchAtEnd === true;
  switchAtEndToggle.dataset.role = "drum-sequencer-switch-at-end";
  switchAtEndToggle.addEventListener("mousedown", (e) => e.stopPropagation());
  switchAtEndToggle.addEventListener("click", (e) => e.stopPropagation());
  switchAtEndToggle.addEventListener("change", () => setDrumSequencerSwitchAtEnd(widgetId, switchAtEndToggle.checked));
  const switchAtEndTrack = document.createElement("span");
  switchAtEndTrack.className = "switch-track";
  switchAtEndLabel.appendChild(switchAtEndToggle);
  switchAtEndLabel.appendChild(switchAtEndTrack);
  switchAtEndLine.appendChild(switchAtEndLabel);
  inputs.appendChild(switchAtEndLine);
  portsEl.appendChild(inputs);

  const layout = document.createElement("div");
  layout.className = "drum-sequencer-pattern-layout";
  const scroll = document.createElement("div");
  scroll.className = "drum-sequencer-scroll";
  const grid = document.createElement("div");
  grid.className = "drum-sequencer-grid";
  const totalSteps = drumSequencerTotalSteps(state);
  const widget = document.getElementById(widgetId);
  if (widget) widget.style.width = `${164 + totalSteps * 36}px`;
  grid.style.gridTemplateColumns = `40px repeat(${totalSteps}, 36px)`;

  const stepCorner = document.createElement("span");
  stepCorner.className = "drum-sequencer-track-label drum-sequencer-corner";
  grid.appendChild(stepCorner);
  for (let step = 0; step < totalSteps; step += 1) {
    const number = document.createElement("span");
    number.className = "drum-sequencer-step-number";
    number.textContent = String(step + 1);
    number.title = `Step ${step + 1}`;
    grid.appendChild(number);
  }

  for (let track = 0; track < state.trackCount; track += 1) {
    const label = document.createElement("span");
    label.className = "drum-sequencer-track-label";
    label.textContent = String(track + 1);
    grid.appendChild(label);
    for (let step = 0; step < totalSteps; step += 1) {
      const index = drumSequencerCellIndex(state, track, step);
      const cell = document.createElement("button");
      cell.type = "button";
      cell.className = "drum-sequencer-cell";
      cell.dataset.drumCell = String(index);
      cell.dataset.track = String(track);
      cell.dataset.step = String(step);
      cell.addEventListener("mousedown", (e) => startDrumSequencerPaint(e, widgetId, index));
      cell.addEventListener("click", (e) => {
        if (e.detail !== 0) return;
        pushHistorySnapshot();
        const pattern = drumSequencerActivePattern(state);
        setDrumSequencerCell(widgetId, index, !pattern.active[index], pattern.velocities[index]);
        markDirty();
      });
      cell.addEventListener("mouseenter", (e) => continueDrumSequencerPaint(e, widgetId, index));
      cell.addEventListener("wheel", (e) => adjustDrumSequencerVelocity(e, widgetId, index), { passive: false });
      grid.appendChild(cell);
    }
  }
  scroll.appendChild(grid);
  layout.appendChild(scroll);

  const outputs = document.createElement("div");
  outputs.className = "drum-sequencer-outputs";
  const outputHeader = document.createElement("div");
  outputHeader.className = "drum-sequencer-output-header";
  outputHeader.textContent = "T\nV";
  outputs.appendChild(outputHeader);
  for (let track = 1; track <= state.trackCount; track += 1) {
    const row = document.createElement("div");
    row.className = "drum-sequencer-output-pair";
    const pulseValue = document.createElement("button");
    pulseValue.type = "button";
    pulseValue.className = "drum-sequencer-pulse-value";
    pulseValue.dataset.drumPulseTrack = String(track - 1);
    pulseValue.addEventListener("click", (e) => {
      e.stopPropagation();
      openNumericValuePopup({
        label: `Track ${track} Trigger Pulse`, unit: "ms", value: state.trackPulseMs[track - 1],
        min: 0.1, max: 60000,
        onSave: (value) => {
          pushHistorySnapshot();
          state.trackPulseMs[track - 1] = drumSequencerPulseMs(value);
          updateDrumSequencerVisual(widgetId);
          sendLiveParamUpdate(widgetId, { trackPulseMs: [...state.trackPulseMs] });
          markDirty();
        },
      });
    });
    row.appendChild(pulseValue);
    row.appendChild(buildDrumSequencerPort(widgetId, { id: `trigger${track}`, label: "T" }, "out"));
    row.appendChild(buildDrumSequencerPort(widgetId, { id: `velocity${track}`, label: "V" }, "out"));
    outputs.appendChild(row);
  }
  layout.appendChild(outputs);
  portsEl.appendChild(layout);
  updateDrumSequencerVisual(widgetId);
  drumSequencerRestartInternalClock(widgetId);
}

function updateDrumSequencerCell(widgetId, index, targetCell) {
  const state = drumSequencerStates.get(widgetId);
  const cell = targetCell || document.getElementById(widgetId)?.querySelector(`[data-drum-cell="${index}"]`);
  if (!state || !cell) return;
  const pattern = drumSequencerActivePattern(state);
  const volts = clamp(Number(pattern.velocities[index]) || 0, 0, CV_MAX_VOLTS);
  cell.classList.toggle("active", pattern.active[index] === true);
  cell.style.setProperty("--velocity", String(volts / CV_MAX_VOLTS));
  const track = Number(cell.dataset.track);
  const step = Number(cell.dataset.step);
  cell.title = `Track ${track + 1}, Step ${step + 1}: ${volts.toFixed(1)} V`;
  cell.setAttribute("aria-label", cell.title);
  cell.setAttribute("aria-pressed", pattern.active[index] ? "true" : "false");
}

function updateDrumSequencerVisual(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".drum-sequencer-cell").forEach((cell) => {
    const index = Number(cell.dataset.drumCell);
    updateDrumSequencerCell(widgetId, index, cell);
  });
  updateDrumSequencerPlayhead(widgetId);
  el.querySelectorAll("[data-drum-pulse-track]").forEach((button) => {
    const track = Number(button.dataset.drumPulseTrack);
    button.textContent = `${Number(state.trackPulseMs[track].toFixed(1))} ms`;
    button.setAttribute("aria-label", `Track ${track + 1} trigger pulse: ${state.trackPulseMs[track]} ms`);
    button.title = "Click to change trigger pulse length";
  });
  const rateSlider = el.querySelector('[data-role="drum-sequencer-rate"]');
  const rateWired = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "rate");
  if (rateSlider) {
    rateSlider.value = String(state.clockCvVolts);
    rateSlider.disabled = rateWired;
  }
  const rateValue = el.querySelector('[data-role="drum-sequencer-rate-value"]');
  if (rateValue) rateValue.textContent = `${formatNumber(drumSequencerBpm(state))} BPM`;

  const patternSlider = el.querySelector('[data-role="drum-sequencer-pattern-select"]');
  const patternWired = connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "pattern");
  if (patternSlider) {
    patternSlider.max = String(Math.max(0, state.patterns.length - 1));
    patternSlider.value = String(state.activePatternIndex);
    patternSlider.disabled = patternWired;
  }
  const patternValue = el.querySelector('[data-role="drum-sequencer-pattern-value"]');
  if (patternValue) patternValue.textContent = `${state.activePatternIndex + 1}/${state.patterns.length}`;
  const switchAtEndToggle = el.querySelector('[data-role="drum-sequencer-switch-at-end"]');
  if (switchAtEndToggle) switchAtEndToggle.checked = state.switchAtEnd === true;
}

function updateDrumSequencerPlayhead(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  el.querySelectorAll(".drum-sequencer-cell.playing").forEach((cell) => cell.classList.remove("playing"));
  if (state.lastStep >= 0) {
    el.querySelectorAll(`[data-step="${state.lastStep}"]`).forEach((cell) => cell.classList.add("playing"));
  }
}

function setDrumSequencerCell(widgetId, index, active, velocity) {
  const state = drumSequencerStates.get(widgetId);
  const pattern = state && drumSequencerActivePattern(state);
  if (!pattern || index < 0 || index >= pattern.active.length) return;
  pattern.active[index] = active;
  if (velocity !== undefined) pattern.velocities[index] = clamp(velocity, 0, CV_MAX_VOLTS);
  updateDrumSequencerCell(widgetId, index);
  sendLiveParamUpdateThrottled(widgetId, { [`cell${index}`]: drumSequencerEncodedCell(state, index) });
}

function startDrumSequencerPaint(e, widgetId, index) {
  if (e.button !== 0) return;
  e.preventDefault();
  e.stopPropagation();
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  const pattern = drumSequencerActivePattern(state);
  pushHistorySnapshot();
  drumPaintSession = {
    widgetId,
    active: !pattern.active[index],
    velocity: pattern.velocities[index],
    visited: new Set(),
  };
  paintDrumSequencerCell(widgetId, index);
  const finish = () => {
    drumPaintSession = null;
    document.removeEventListener("mouseup", finish);
    markDirty();
  };
  document.addEventListener("mouseup", finish);
}

function paintDrumSequencerCell(widgetId, index) {
  if (!drumPaintSession || drumPaintSession.widgetId !== widgetId || drumPaintSession.visited.has(index)) return;
  drumPaintSession.visited.add(index);
  setDrumSequencerCell(
    widgetId,
    index,
    drumPaintSession.active,
    drumPaintSession.active ? drumPaintSession.velocity : undefined
  );
}

function continueDrumSequencerPaint(e, widgetId, index) {
  if (!drumPaintSession || (e.buttons & 1) === 0) return;
  paintDrumSequencerCell(widgetId, index);
}

function adjustDrumSequencerVelocity(e, widgetId, index) {
  e.preventDefault();
  e.stopPropagation();
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  const pattern = drumSequencerActivePattern(state);
  const delta = e.deltaY < 0 ? 0.1 : -0.1;
  setDrumSequencerCell(widgetId, index, pattern.active[index], Math.round(clamp(pattern.velocities[index] + delta, 0, CV_MAX_VOLTS) * 10) / 10);
  markDirty();
}

function drumSequencerStopTriggers(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  state.triggerTimers.forEach((timer, track) => {
    clearTimeout(timer);
    state.triggerTimers[track] = null;
    setLedActive(document.getElementById(widgetId)?.querySelector(`[data-port-id="trigger${track + 1}"][data-port-dir="out"]`), false);
    propagate(widgetId, `trigger${track + 1}`, GATE_OFF_VOLTS);
  });
}

function clearDrumSequencer(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  pushHistorySnapshot();
  const pattern = drumSequencerActivePattern(state);
  pattern.active.fill(false);
  pattern.velocities.fill(CV_MAX_VOLTS);
  state.currentStep = 0;
  state.lastStep = -1;
  state.heldVelocity.fill(0);
  drumSequencerStopTriggers(widgetId);
  state.triggerTimers.forEach((_timer, track) => {
    propagate(widgetId, `velocity${track + 1}`, 0);
  });
  updateDrumSequencerVisual(widgetId);
  drumSequencerRestartInternalClock(widgetId);
  savePresetNow();
}

function drumSequencerPulseMs(value, fallback = 50) {
  return typeof value === "number" && Number.isFinite(value) ? clamp(value, 0.1, 60000) : fallback;
}

function drumSequencerBpm(state) {
  const bpm = (state.clockCvVolts / CV_MAX_VOLTS) * state.clockMaxBpm;
  return state.clockRoundBpm ? Math.round(bpm) : bpm;
}

function setDrumSequencerRate(widgetId, volts) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  state.clockCvVolts = clamp(volts, 0, CV_MAX_VOLTS);
  updateDrumSequencerVisual(widgetId);
  drumSequencerRestartInternalClock(widgetId);
  sendLiveParamUpdateThrottled(widgetId, { clockCvVolts: state.clockCvVolts });
}

function drumSequencerStopInternalClock(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (state?.clockTickTimer) {
    clearInterval(state.clockTickTimer);
    state.clockTickTimer = null;
  }
}

function drumSequencerRestartInternalClock(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  drumSequencerStopInternalClock(widgetId);
  if (connections.some((c) => c.to.widgetId === widgetId && c.to.portId === "clock")) return;
  const bpm = drumSequencerBpm(state);
  if (bpm <= 0) return;
  // A step is a 16th note (4 steps per beat) - matches DrumSequencerNode.h's render().
  state.clockTickTimer = setInterval(() => drumSequencerTriggerStep(widgetId), 15000 / bpm);
}

function drumSequencerTriggerStep(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el || state.gateVolts < GATE_THRESHOLD_VOLTS) return;
  const totalSteps = drumSequencerTotalSteps(state);
  const playedStep = state.currentStep;
  state.lastStep = playedStep;
  const pattern = drumSequencerActivePattern(state);
  for (let track = 0; track < state.trackCount; track += 1) {
    const index = drumSequencerCellIndex(state, track, playedStep);
    if (!pattern.active[index]) continue;
    const velocity = pattern.velocities[index];
    state.heldVelocity[track] = velocity;
    propagate(widgetId, `velocity${track + 1}`, velocity);
    const triggerPort = `trigger${track + 1}`;
    const triggerDot = el.querySelector(`[data-port-id="${triggerPort}"][data-port-dir="out"]`);
    const startPulse = () => {
      setLedActive(triggerDot, true);
      propagate(widgetId, triggerPort, GATE_ON_VOLTS);
      state.triggerTimers[track] = setTimeout(() => {
        setLedActive(triggerDot, false);
        propagate(widgetId, triggerPort, GATE_OFF_VOLTS);
        state.triggerTimers[track] = null;
      }, state.trackPulseMs[track]);
    };
    if (state.triggerTimers[track]) {
      clearTimeout(state.triggerTimers[track]);
      setLedActive(triggerDot, false);
      propagate(widgetId, triggerPort, GATE_OFF_VOLTS);
      state.triggerTimers[track] = setTimeout(startPulse, 1);
    } else {
      startPulse();
    }
  }
  state.currentStep = (playedStep + 1) % totalSteps;
  if (state.currentStep === 0 && state.pendingPatternIndex !== -1) {
    state.activePatternIndex = state.pendingPatternIndex;
    state.pendingPatternIndex = -1;
    updateDrumSequencerVisual(widgetId);
  }
  updateDrumSequencerPlayhead(widgetId);
}

function setDrumSequencerActivePattern(widgetId, index) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  const clamped = clamp(Math.round(index), 0, state.patterns.length - 1);
  // Quantizing only makes sense while the sequencer is actually running
  // (gate high) - with no gate, there is no upcoming step boundary to wait
  // for, so switch right away (mirrors DrumSequencerNode.h's render()).
  if (state.switchAtEnd && state.gatePrev) {
    state.pendingPatternIndex = clamped;
  } else {
    state.activePatternIndex = clamped;
    state.pendingPatternIndex = -1;
    updateDrumSequencerVisual(widgetId);
  }
  sendLiveParamUpdateThrottled(widgetId, { activePatternIndex: clamped });
}

function setDrumSequencerSwitchAtEnd(widgetId, on) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  state.switchAtEnd = on === true;
  sendLiveParamUpdate(widgetId, { switchAtEnd: state.switchAtEnd ? 1 : 0 });
}

let drumPatternClipboard = null;
let drumPatternMenuTargetId = null;
const drumPatternContextMenu = document.getElementById("drumPatternContextMenu");

function closeDrumPatternMenu() {
  if (drumPatternContextMenu) drumPatternContextMenu.classList.add("hidden");
  drumPatternMenuTargetId = null;
}

function openDrumPatternMenu(widgetId, anchorBtn) {
  if (!drumPatternContextMenu) return;
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  drumPatternMenuTargetId = widgetId;
  const deleteBtn = drumPatternContextMenu.querySelector('[data-action="delete"]');
  if (deleteBtn) deleteBtn.disabled = state.patterns.length <= 1;
  const pasteBtn = drumPatternContextMenu.querySelector('[data-action="paste"]');
  if (pasteBtn) pasteBtn.disabled = !drumPatternClipboard;
  const addBtn = drumPatternContextMenu.querySelector('[data-action="add"]');
  if (addBtn) addBtn.disabled = state.patterns.length >= DRUM_MAX_PATTERNS;
  drumPatternContextMenu.classList.remove("hidden");
  const anchorRect = anchorBtn.getBoundingClientRect();
  const menuRect = drumPatternContextMenu.getBoundingClientRect();
  drumPatternContextMenu.style.left = `${Math.min(anchorRect.left, window.innerWidth - menuRect.width - 8)}px`;
  drumPatternContextMenu.style.top = `${Math.min(anchorRect.bottom + 4, window.innerHeight - menuRect.height - 8)}px`;
}

function addDrumPattern(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state || state.patterns.length >= DRUM_MAX_PATTERNS) return;
  pushHistorySnapshot();
  const cellCount = state.stepCount * state.trackCount;
  state.patterns.push({
    active: new Array(cellCount).fill(false),
    velocities: new Array(cellCount).fill(CV_MAX_VOLTS),
  });
  state.activePatternIndex = state.patterns.length - 1;
  state.pendingPatternIndex = -1;
  const portsEl = document.getElementById(widgetId)?.querySelector(".widget-ports");
  if (portsEl) renderDrumSequencerPorts(widgetId, portsEl);
  markDirty();
  savePresetNow();
}

function deleteDrumPattern(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state || state.patterns.length <= 1) return;
  pushHistorySnapshot();
  state.patterns.splice(state.activePatternIndex, 1);
  state.activePatternIndex = Math.min(state.activePatternIndex, state.patterns.length - 1);
  state.pendingPatternIndex = -1;
  const portsEl = document.getElementById(widgetId)?.querySelector(".widget-ports");
  if (portsEl) renderDrumSequencerPorts(widgetId, portsEl);
  markDirty();
  savePresetNow();
}

function copyDrumPattern(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state) return;
  const pattern = drumSequencerActivePattern(state);
  drumPatternClipboard = { active: [...pattern.active], velocities: [...pattern.velocities] };
}

function pasteDrumPattern(widgetId) {
  const state = drumSequencerStates.get(widgetId);
  if (!state || !drumPatternClipboard) return;
  const cellCount = state.stepCount * state.trackCount;
  if (drumPatternClipboard.active.length !== cellCount) return;
  pushHistorySnapshot();
  const pattern = drumSequencerActivePattern(state);
  pattern.active = [...drumPatternClipboard.active];
  pattern.velocities = [...drumPatternClipboard.velocities];
  updateDrumSequencerVisual(widgetId);
  markDirty();
  savePresetNow();
}

if (drumPatternContextMenu) {
  drumPatternContextMenu.addEventListener("click", (event) => {
    const action = event.target.closest("button")?.dataset.action;
    const widgetId = drumPatternMenuTargetId;
    closeDrumPatternMenu();
    if (!widgetId || !action) return;
    if (action === "add") addDrumPattern(widgetId);
    if (action === "delete") deleteDrumPattern(widgetId);
    if (action === "copy") copyDrumPattern(widgetId);
    if (action === "paste") pasteDrumPattern(widgetId);
    if (action === "clear") clearDrumSequencer(widgetId);
    if (action === "settings") openDrumSequencerSettings(widgetId);
  });
  document.addEventListener("click", (event) => {
    if (!event.target.closest("#drumPatternContextMenu, .drum-sequencer-pattern-menu-btn")) closeDrumPatternMenu();
  });
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



const outputMultiplexerSettingsOverlay = document.getElementById("outputMultiplexerSettingsOverlay");
const outputMultiplexerCountInput = document.getElementById("outputMultiplexerCountInput");
let pendingOutputMultiplexerId = null;

function openOutputMultiplexerSettings(widgetId) {
  const state = outputMultiplexerStates.get(widgetId);
  if (!state) return;
  pendingOutputMultiplexerId = widgetId;
  outputMultiplexerCountInput.value = state.outputCount;
  outputMultiplexerSettingsOverlay.classList.remove("hidden");
}

function closeOutputMultiplexerSettings() {
  outputMultiplexerSettingsOverlay.classList.add("hidden");
  pendingOutputMultiplexerId = null;
}


document.getElementById("outputMultiplexerSettingsCancel").addEventListener("click", closeOutputMultiplexerSettings);

document.getElementById("outputMultiplexerSettingsSave").addEventListener("click", () => {
  if (!pendingOutputMultiplexerId) return;
  const state = outputMultiplexerStates.get(pendingOutputMultiplexerId);
  if (state) {
    let count = parseInt(outputMultiplexerCountInput.value, 10);
    if (Number.isNaN(count)) count = OUTPUT_MULTIPLEXER_MIN_OUTPUTS;
    count = clamp(count, OUTPUT_MULTIPLEXER_MIN_OUTPUTS, OUTPUT_MULTIPLEXER_MAX_OUTPUTS);

    if (count < state.outputCount) {
      for (let i = count + 1; i <= state.outputCount; i += 1) {
        removeConnectionsForPort(pendingOutputMultiplexerId, `out${i}`, "out");
      }
    }
    state.outputCount = count;
    if (state.manualSelect > count - 1) state.manualSelect = count - 1;
    // outputCount changes the number of allocated outPorts[] buffers on the
    // firmware side (see GraphBuilder.h) - a live setParam can't grow that,
    // only a full preset rebuild can, hence savePresetNow() below instead of
    // a sendLiveParamUpdate here (same reasoning as Mixer/Multiplexer's own
    // inputCount, which also requires a rebuild rather than a live push).

    const el = document.getElementById(pendingOutputMultiplexerId);
    const portsEl = el ? el.querySelector(".widget-ports") : null;
    if (portsEl) renderOutputMultiplexerPorts(pendingOutputMultiplexerId, portsEl);

    outputMultiplexerSetActiveIndex(pendingOutputMultiplexerId, connections.some((c) => c.to.widgetId === pendingOutputMultiplexerId && c.to.portId === "select")
      ? state.activeIndex
      : state.manualSelect);
    drawConnections();
    savePresetNow();
  }
  closeOutputMultiplexerSettings();
});



const adsrStates = new Map();
const MAX_ENVELOPE_SECONDS = 4;
const ADSR_GRAPH_WIDTH = 240;
const ADSR_GRAPH_HEIGHT = 96;
const ADSR_GRAPH_PADDING = 10;
const ADSR_GRAPH_HOLD_WIDTH = 18;

const dxEnvelopeStates = new Map();
const DX_ENVELOPE_WIDTH = 240;
const DX_ENVELOPE_HEIGHT = 116;
const DX_ENVELOPE_PADDING = 10;
const DX_ENVELOPE_HOLD_WIDTH = 18;

function createDxEnvelopeDefaults() {
  return {
    level1: 10,
    level2: 8,
    level3: 7,
    level4: 0,
    time1: 0.1,
    time2: 0.25,
    time3: 0.4,
    time4: 0.6,
    time1MaxSeconds: MAX_ENVELOPE_SECONDS,
    time2MaxSeconds: MAX_ENVELOPE_SECONDS,
    time3MaxSeconds: MAX_ENVELOPE_SECONDS,
    time4MaxSeconds: MAX_ENVELOPE_SECONDS,
    time1Curve: "linear",
    time2Curve: "linear",
    time3Curve: "linear",
    time4Curve: "linear",
    livePhase: "idle",
    liveLevel: 0,
    liveSegment: 0,
    liveSegmentStartedAt: 0,
    liveSegmentStartLevel: 0,
    liveGate: false,
    liveVoiceIndex: null,
    graphCollapsed: false,
  };
}

function dxEnvelopeSliderVolts(state, paramName) {
  if (!state) return 0;
  if (paramName.startsWith("level")) return state[paramName];
  return secondsToCv(state[paramName], state[`${paramName}MaxSeconds`], state[`${paramName}Curve`]);
}

function formatDxEnvelopeValue(state, paramName) {
  if (!state) return "–";
  const unit = paramName.startsWith("level") ? "V" : "s";
  return `${formatNumber(state[paramName])} ${unit}`;
}

function dxEnvelopeGraphGeometry(state) {
  const plotWidth = DX_ENVELOPE_WIDTH - DX_ENVELOPE_PADDING * 2 - DX_ENVELOPE_HOLD_WIDTH;
  const segmentWidth = plotWidth / 4;
  const x = [DX_ENVELOPE_PADDING];
  for (let i = 1; i <= 3; i += 1) {
    const ratio = clamp(state[`time${i}`] / state[`time${i}MaxSeconds`], 0, 1);
    x.push(x[x.length - 1] + ratio * segmentWidth);
  }
  const releaseStart = x[3] + DX_ENVELOPE_HOLD_WIDTH;
  const releaseRatio = clamp(state.time4 / state.time4MaxSeconds, 0, 1);
  x.push(releaseStart + releaseRatio * segmentWidth);
  const yForLevel = (level) => DX_ENVELOPE_HEIGHT - DX_ENVELOPE_PADDING -
    clamp(level / CV_MAX_VOLTS, 0, 1) * (DX_ENVELOPE_HEIGHT - DX_ENVELOPE_PADDING * 2);
  return {
    segmentWidth,
    releaseStart,
    points: [
      { x: x[0], y: yForLevel(state.level4) },
      { x: x[1], y: yForLevel(state.level1) },
      { x: x[2], y: yForLevel(state.level2) },
      { x: x[3], y: yForLevel(state.level3) },
      { x: x[4], y: yForLevel(state.level4) },
    ],
  };
}

function createDxEnvelopeGraph(widgetId) {
  const svgNs = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(svgNs, "svg");
  svg.classList.add("dx-envelope-graph");
  svg.dataset.role = "dx-envelope-graph";
  svg.setAttribute("viewBox", `0 0 ${DX_ENVELOPE_WIDTH} ${DX_ENVELOPE_HEIGHT}`);
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", "DX envelope curve");

  [0.25, 0.5, 0.75].forEach((ratio) => {
    const line = document.createElementNS(svgNs, "line");
    line.classList.add("dx-envelope-grid-line");
    const y = DX_ENVELOPE_PADDING + ratio * (DX_ENVELOPE_HEIGHT - DX_ENVELOPE_PADDING * 2);
    line.setAttribute("x1", String(DX_ENVELOPE_PADDING));
    line.setAttribute("x2", String(DX_ENVELOPE_WIDTH - DX_ENVELOPE_PADDING));
    line.setAttribute("y1", String(y));
    line.setAttribute("y2", String(y));
    svg.appendChild(line);
  });

  const path = document.createElementNS(svgNs, "path");
  path.classList.add("dx-envelope-curve");
  path.dataset.role = "dx-envelope-curve";
  svg.appendChild(path);

  const hold = document.createElementNS(svgNs, "line");
  hold.classList.add("dx-envelope-hold");
  hold.dataset.role = "dx-envelope-hold";
  svg.appendChild(hold);

  for (let i = 1; i <= 4; i += 1) {
    const point = document.createElementNS(svgNs, "circle");
    point.classList.add("dx-envelope-point");
    point.dataset.pointIndex = String(i);
    point.setAttribute("r", "6");
    point.setAttribute("tabindex", "0");
    point.setAttribute("aria-label", `Level ${i}, Time ${i}`);
    svg.appendChild(point);
  }

  const livePoint = document.createElementNS(svgNs, "circle");
  livePoint.classList.add("dx-envelope-live-point");
  livePoint.dataset.role = "dx-envelope-live-point";
  livePoint.setAttribute("r", "4.5");
  livePoint.setAttribute("aria-hidden", "true");
  svg.appendChild(livePoint);

  let drag = null;
  svg.addEventListener("pointerdown", (event) => {
    const point = event.target.closest?.(".dx-envelope-point");
    if (!point || event.button !== 0) return;
    const state = dxEnvelopeStates.get(widgetId);
    if (!state) return;
    const index = Number(point.dataset.pointIndex);
    drag = {
      index,
      pointerId: event.pointerId,
      clientX: event.clientX,
      clientY: event.clientY,
      level: state[`level${index}`],
      time: state[`time${index}`],
    };
    event.preventDefault();
    event.stopPropagation();
    svg.setPointerCapture?.(event.pointerId);
    point.classList.add("dragging");
  });
  svg.addEventListener("pointermove", (event) => {
    if (!drag || drag.pointerId !== event.pointerId) return;
    const state = dxEnvelopeStates.get(widgetId);
    const rect = svg.getBoundingClientRect();
    if (!state || rect.width <= 0 || rect.height <= 0) return;
    event.preventDefault();
    event.stopPropagation();
    const graph = dxEnvelopeGraphGeometry(state);
    const maxSeconds = state[`time${drag.index}MaxSeconds`];
    const level = clamp(
      drag.level - ((event.clientY - drag.clientY) * DX_ENVELOPE_HEIGHT * CV_MAX_VOLTS) /
        (rect.height * (DX_ENVELOPE_HEIGHT - DX_ENVELOPE_PADDING * 2)),
      0,
      CV_MAX_VOLTS
    );
    const time = clamp(
      drag.time + ((event.clientX - drag.clientX) * DX_ENVELOPE_WIDTH * maxSeconds) /
        (rect.width * graph.segmentWidth),
      0.001,
      maxSeconds
    );
    deliverToWidget(widgetId, `level${drag.index}`, level);
    deliverToWidget(widgetId, `time${drag.index}`,
      secondsToCv(time, maxSeconds, state[`time${drag.index}Curve`]));
  });
  const stopDrag = (event) => {
    if (!drag || drag.pointerId !== event.pointerId) return;
    svg.querySelector(`[data-point-index="${drag.index}"]`)?.classList.remove("dragging");
    if (svg.hasPointerCapture?.(event.pointerId)) svg.releasePointerCapture(event.pointerId);
    drag = null;
    event.stopPropagation();
  };
  svg.addEventListener("pointerup", stopDrag);
  svg.addEventListener("pointercancel", stopDrag);
  return svg;
}

function dxEnvelopeLivePosition(state, geometry, now = performance.now()) {
  const [start, point1, point2, point3, point4] = geometry.points;
  if (state.livePhase === "idle") return start;
  if (state.livePhase === "sustain") return point3;

  const segment = state.livePhase === "release" ? 4 : state.liveSegment;
  const durationMs = Math.max(1, state[`time${segment}`] * 1000);
  const progress = clamp((now - state.liveSegmentStartedAt) / durationMs, 0, 1);
  let from;
  let to;
  if (segment === 1) {
    from = start;
    to = point1;
  } else if (segment === 2) {
    from = point1;
    to = point2;
  } else if (segment === 3) {
    from = point2;
    to = point3;
  } else {
    from = { x: geometry.releaseStart, y: point3.y };
    to = point4;
  }
  return {
    x: from.x + (to.x - from.x) * progress,
    y: from.y + (to.y - from.y) * progress,
  };
}

function advanceDxEnvelopeLiveState(state, now = performance.now()) {
  if (state.livePhase === "attack") {
    while (state.liveSegment <= 3) {
      const durationMs = Math.max(1, state[`time${state.liveSegment}`] * 1000);
      if (now < state.liveSegmentStartedAt + durationMs) break;
      state.liveSegmentStartedAt += durationMs;
      state.liveSegmentStartLevel = state[`level${state.liveSegment}`];
      state.liveLevel = state.liveSegmentStartLevel;
      state.liveSegment += 1;
    }
    if (state.liveSegment > 3) {
      state.livePhase = "sustain";
      state.liveLevel = state.level3;
    }
  } else if (state.livePhase === "release") {
    const durationMs = Math.max(1, state.time4 * 1000);
    if (now >= state.liveSegmentStartedAt + durationMs) {
      state.livePhase = "idle";
      state.liveLevel = state.level4;
    }
  }
}

function currentDxEnvelopeLiveLevel(state, now = performance.now()) {
  advanceDxEnvelopeLiveState(state, now);
  if (state.livePhase === "idle") return state.level4;
  if (state.livePhase === "sustain") return state.level3;
  const target = state.livePhase === "release" ? state.level4 : state[`level${state.liveSegment}`];
  const duration = state.livePhase === "release" ? state.time4 : state[`time${state.liveSegment}`];
  const progress = clamp((now - state.liveSegmentStartedAt) / Math.max(0.001, duration * 1000), 0, 1);
  return state.liveSegmentStartLevel + (target - state.liveSegmentStartLevel) * progress;
}

function setDxEnvelopeGate(state, gateOn, now = performance.now()) {
  const currentLevel = currentDxEnvelopeLiveLevel(state, now);
  state.liveLevel = currentLevel;
  state.liveSegmentStartLevel = currentLevel;
  state.liveSegmentStartedAt = now;
  if (gateOn) {
    state.livePhase = "attack";
    state.liveSegment = 1;
  } else {
    state.livePhase = "release";
    state.liveSegment = 4;
  }
}

function applyDxEnvelopeCollapsed(widgetId) {
  const state = dxEnvelopeStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const collapsed = !!state.graphCollapsed;
  el.querySelector(".dx-envelope-body")?.classList.toggle("collapsed", collapsed);
  const toggleBtn = el.querySelector('[data-role="dx-envelope-collapse-toggle"]');
  if (toggleBtn) {
    toggleBtn.textContent = collapsed ? "▸" : "▾";
    toggleBtn.setAttribute("aria-label", collapsed ? "Expand envelope graph" : "Collapse envelope graph");
    toggleBtn.setAttribute("aria-expanded", collapsed ? "false" : "true");
  }
}

function updateDxEnvelopeVisuals(widgetId) {
  const state = dxEnvelopeStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  for (let i = 1; i <= 4; i += 1) {
    [`level${i}`, `time${i}`].forEach((paramName) => {
      const slider = el.querySelector(`[data-role="dx-envelope-slider-${paramName}"]`);
      if (slider) slider.value = String(dxEnvelopeSliderVolts(state, paramName));
      const value = el.querySelector(`[data-role="dx-envelope-value-${paramName}"]`);
      if (value) value.textContent = formatDxEnvelopeValue(state, paramName);
    });
  }

  const svg = el.querySelector('[data-role="dx-envelope-graph"]');
  if (!svg) return;
  const geometry = dxEnvelopeGraphGeometry(state);
  const [start, point1, point2, point3, point4] = geometry.points;
  svg.querySelector('[data-role="dx-envelope-curve"]')?.setAttribute(
    "d",
    `M ${start.x} ${start.y} L ${point1.x} ${point1.y} L ${point2.x} ${point2.y} ` +
      `L ${point3.x} ${point3.y} L ${geometry.releaseStart} ${point3.y} L ${point4.x} ${point4.y}`
  );
  const hold = svg.querySelector('[data-role="dx-envelope-hold"]');
  hold?.setAttribute("x1", String(point3.x));
  hold?.setAttribute("x2", String(geometry.releaseStart));
  hold?.setAttribute("y1", String(point3.y));
  hold?.setAttribute("y2", String(point3.y));
  [point1, point2, point3, point4].forEach((point, index) => {
    const circle = svg.querySelector(`[data-point-index="${index + 1}"]`);
    circle?.setAttribute("cx", String(point.x));
    circle?.setAttribute("cy", String(point.y));
  });
  updateDxEnvelopeLivePoint(widgetId, performance.now(), geometry);
}

function updateDxEnvelopeLivePoint(widgetId, now = performance.now(), geometry = null) {
  const state = dxEnvelopeStates.get(widgetId);
  const svg = document.getElementById(widgetId)?.querySelector('[data-role="dx-envelope-graph"]');
  if (!state || !svg) return;
  const livePosition = dxEnvelopeLivePosition(state, geometry || dxEnvelopeGraphGeometry(state), now);
  const livePoint = svg.querySelector('[data-role="dx-envelope-live-point"]');
  livePoint?.setAttribute("cx", String(livePosition.x));
  livePoint?.setAttribute("cy", String(livePosition.y));
}

function tickDxEnvelopeLivePoints(now) {
  dxEnvelopeStates.forEach((state, widgetId) => {
    const wasActive = state.livePhase !== "idle";
    advanceDxEnvelopeLiveState(state, now);
    if (wasActive && state.livePhase === "idle") {
      updateDxEnvelopeVisuals(widgetId);
    } else if (wasActive || state.livePhase !== "idle" || state.level4 > 0) {
      updateDxEnvelopeLivePoint(widgetId, now);
    }
  });
  requestAnimationFrame(tickDxEnvelopeLivePoints);
}
requestAnimationFrame(tickDxEnvelopeLivePoints);

const dxEnvelopeSettingsOverlay = document.getElementById("dxEnvelopeSettingsOverlay");
let pendingDxEnvelopeId = null;

function openDxEnvelopeSettings(widgetId) {
  const state = dxEnvelopeStates.get(widgetId);
  if (!state) return;
  pendingDxEnvelopeId = widgetId;
  for (let i = 1; i <= 4; i += 1) {
    document.getElementById(`dxEnvelopeTime${i}MaxInput`).value = state[`time${i}MaxSeconds`];
    document.getElementById(`dxEnvelopeTime${i}CurveSelect`).value = state[`time${i}Curve`];
  }
  dxEnvelopeSettingsOverlay.classList.remove("hidden");
}

function closeDxEnvelopeSettings() {
  dxEnvelopeSettingsOverlay.classList.add("hidden");
  pendingDxEnvelopeId = null;
}

document.getElementById("dxEnvelopeSettingsCancel").addEventListener("click", closeDxEnvelopeSettings);
document.getElementById("dxEnvelopeSettingsSave").addEventListener("click", () => {
  const state = dxEnvelopeStates.get(pendingDxEnvelopeId);
  if (!state) return;
  const sliderVolts = [];
  for (let i = 1; i <= 4; i += 1) sliderVolts.push(dxEnvelopeSliderVolts(state, `time${i}`));
  const liveParams = {};
  for (let i = 1; i <= 4; i += 1) {
    const parsed = parseFloatFlexible(document.getElementById(`dxEnvelopeTime${i}MaxInput`).value);
    state[`time${i}MaxSeconds`] = Number.isFinite(parsed) && parsed > 0 ? parsed : MAX_ENVELOPE_SECONDS;
    state[`time${i}Curve`] = document.getElementById(`dxEnvelopeTime${i}CurveSelect`).value === "log"
      ? "log"
      : "linear";
    liveParams[`time${i}MaxSeconds`] = state[`time${i}MaxSeconds`];
    liveParams[`time${i}Curve`] = state[`time${i}Curve`];
    deliverToWidget(pendingDxEnvelopeId, `time${i}`, sliderVolts[i - 1]);
  }
  sendLiveParamUpdate(pendingDxEnvelopeId, liveParams);
  updateDxEnvelopeVisuals(pendingDxEnvelopeId);
  savePresetNow();
  closeDxEnvelopeSettings();
});



function adsrGraphGeometry(state) {
  const plotWidth = ADSR_GRAPH_WIDTH - ADSR_GRAPH_PADDING * 2 - ADSR_GRAPH_HOLD_WIDTH;
  const segmentWidth = plotWidth / 3;
  const attackRatio = clamp(state.attack / state.attackMaxSeconds, 0, 1);
  const decayRatio = clamp(state.decay / state.decayMaxSeconds, 0, 1);
  const releaseRatio = clamp(state.release / state.releaseMaxSeconds, 0, 1);
  const x0 = ADSR_GRAPH_PADDING;
  const x1 = x0 + attackRatio * segmentWidth;
  const x2 = x1 + decayRatio * segmentWidth;
  const holdEnd = x2 + ADSR_GRAPH_HOLD_WIDTH;
  const x3 = holdEnd + releaseRatio * segmentWidth;
  const yForLevel = (level) => ADSR_GRAPH_HEIGHT - ADSR_GRAPH_PADDING -
    clamp(level, 0, 1) * (ADSR_GRAPH_HEIGHT - ADSR_GRAPH_PADDING * 2);
  return {
    holdEnd,
    points: {
      start: { x: x0, y: yForLevel(0) },
      peak: { x: x1, y: yForLevel(1) },
      decayEnd: { x: x2, y: yForLevel(state.sustain) },
      releaseEnd: { x: x3, y: yForLevel(0) },
    },
  };
}

function createAdsrGraph(widgetId) {
  const svgNs = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(svgNs, "svg");
  svg.classList.add("adsr-graph");
  svg.dataset.role = "adsr-graph";
  svg.setAttribute("viewBox", `0 0 ${ADSR_GRAPH_WIDTH} ${ADSR_GRAPH_HEIGHT}`);
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", "ADSR envelope curve");

  [0.25, 0.5, 0.75].forEach((ratio) => {
    const line = document.createElementNS(svgNs, "line");
    line.classList.add("adsr-grid-line");
    const y = ADSR_GRAPH_PADDING + ratio * (ADSR_GRAPH_HEIGHT - ADSR_GRAPH_PADDING * 2);
    line.setAttribute("x1", String(ADSR_GRAPH_PADDING));
    line.setAttribute("x2", String(ADSR_GRAPH_WIDTH - ADSR_GRAPH_PADDING));
    line.setAttribute("y1", String(y));
    line.setAttribute("y2", String(y));
    svg.appendChild(line);
  });

  const path = document.createElementNS(svgNs, "path");
  path.classList.add("adsr-curve");
  path.dataset.role = "adsr-curve";
  svg.appendChild(path);

  const hold = document.createElementNS(svgNs, "line");
  hold.classList.add("adsr-hold");
  hold.dataset.role = "adsr-hold";
  svg.appendChild(hold);

  ["peak", "decay-end", "release-end"].forEach((pointId) => {
    const point = document.createElementNS(svgNs, "circle");
    point.classList.add("adsr-point");
    point.dataset.pointId = pointId;
    point.setAttribute("r", "6");
    point.setAttribute("tabindex", "0");
    point.setAttribute("aria-label", pointId.replace("-", " "));
    svg.appendChild(point);
  });

  const livePoint = document.createElementNS(svgNs, "circle");
  livePoint.classList.add("adsr-live-point");
  livePoint.dataset.role = "adsr-live-point";
  livePoint.setAttribute("r", "4.5");
  livePoint.setAttribute("aria-hidden", "true");
  svg.appendChild(livePoint);

  let drag = null;
  svg.addEventListener("pointerdown", (event) => {
    const point = event.target.closest?.(".adsr-point");
    if (!point || event.button !== 0) return;
    const state = adsrStates.get(widgetId);
    if (!state) return;
    drag = {
      pointId: point.dataset.pointId,
      pointerId: event.pointerId,
      clientX: event.clientX,
      clientY: event.clientY,
      attack: state.attack,
      decay: state.decay,
      sustain: state.sustain,
      release: state.release,
    };
    event.preventDefault();
    event.stopPropagation();
    svg.setPointerCapture?.(event.pointerId);
    point.classList.add("dragging");
  });
  svg.addEventListener("pointermove", (event) => {
    if (!drag || drag.pointerId !== event.pointerId) return;
    const state = adsrStates.get(widgetId);
    const rect = svg.getBoundingClientRect();
    if (!state || rect.width <= 0 || rect.height <= 0) return;
    event.preventDefault();
    event.stopPropagation();
    const segmentWidth = (ADSR_GRAPH_WIDTH - ADSR_GRAPH_PADDING * 2 - ADSR_GRAPH_HOLD_WIDTH) / 3;
    const timeDelta = (event.clientX - drag.clientX) * ADSR_GRAPH_WIDTH / (rect.width * segmentWidth);
    if (drag.pointId === "peak") {
      deliverToWidget(widgetId, "att", secondsToCv(
        clamp(drag.attack + timeDelta * state.attackMaxSeconds, 0.001, state.attackMaxSeconds),
        state.attackMaxSeconds,
        state.attackCurve
      ));
    } else if (drag.pointId === "decay-end") {
      deliverToWidget(widgetId, "dec", secondsToCv(
        clamp(drag.decay + timeDelta * state.decayMaxSeconds, 0.001, state.decayMaxSeconds),
        state.decayMaxSeconds,
        state.decayCurve
      ));
      deliverToWidget(widgetId, "sus", clamp(
        drag.sustain - ((event.clientY - drag.clientY) * ADSR_GRAPH_HEIGHT) /
          (rect.height * (ADSR_GRAPH_HEIGHT - ADSR_GRAPH_PADDING * 2)),
        0,
        1
      ) * CV_MAX_VOLTS);
    } else {
      deliverToWidget(widgetId, "rel", secondsToCv(
        clamp(drag.release + timeDelta * state.releaseMaxSeconds, 0.001, state.releaseMaxSeconds),
        state.releaseMaxSeconds,
        state.releaseCurve
      ));
    }
  });
  const stopDrag = (event) => {
    if (!drag || drag.pointerId !== event.pointerId) return;
    svg.querySelector(`[data-point-id="${drag.pointId}"]`)?.classList.remove("dragging");
    if (svg.hasPointerCapture?.(event.pointerId)) svg.releasePointerCapture(event.pointerId);
    drag = null;
    event.stopPropagation();
  };
  svg.addEventListener("pointerup", stopDrag);
  svg.addEventListener("pointercancel", stopDrag);

  return svg;
}

function adsrLivePosition(state, geometry, now = performance.now()) {
  const { start, peak, decayEnd, releaseEnd } = geometry.points;
  if (state.livePhase === "idle") return start;
  if (state.livePhase === "sustain") return decayEnd;

  let from;
  let to;
  let duration;
  if (state.livePhase === "attack") {
    from = start;
    to = peak;
    duration = state.attack;
  } else if (state.livePhase === "decay") {
    from = peak;
    to = decayEnd;
    duration = state.decay;
  } else {
    from = { x: geometry.holdEnd, y: decayEnd.y };
    to = releaseEnd;
    duration = state.release;
  }
  const durationMs = Math.max(1, duration * 1000);
  const progress = clamp((now - state.liveSegmentStartedAt) / durationMs, 0, 1);
  return {
    x: from.x + (to.x - from.x) * progress,
    y: from.y + (to.y - from.y) * progress,
  };
}

function advanceAdsrLiveState(state, now = performance.now()) {
  if (state.livePhase === "attack") {
    const durationMs = Math.max(1, state.attack * 1000);
    if (now >= state.liveSegmentStartedAt + durationMs) {
      state.liveSegmentStartedAt += durationMs;
      state.liveSegmentStartLevel = 1;
      state.liveLevel = 1;
      state.livePhase = "decay";
    }
  }
  if (state.livePhase === "decay") {
    const durationMs = Math.max(1, state.decay * 1000);
    if (now >= state.liveSegmentStartedAt + durationMs) {
      state.livePhase = "sustain";
      state.liveLevel = state.sustain;
    }
  } else if (state.livePhase === "release") {
    const durationMs = Math.max(1, state.release * 1000);
    if (now >= state.liveSegmentStartedAt + durationMs) {
      state.livePhase = "idle";
      state.liveLevel = 0;
    }
  }
}

function currentAdsrLiveLevel(state, now = performance.now()) {
  advanceAdsrLiveState(state, now);
  if (state.livePhase === "idle") return 0;
  if (state.livePhase === "sustain") return state.sustain;
  const target = state.livePhase === "attack" ? 1 : state.livePhase === "decay" ? state.sustain : 0;
  const duration = state.livePhase === "attack"
    ? state.attack
    : state.livePhase === "decay" ? state.decay : state.release;
  const progress = clamp((now - state.liveSegmentStartedAt) / Math.max(0.001, duration * 1000), 0, 1);
  return state.liveSegmentStartLevel + (target - state.liveSegmentStartLevel) * progress;
}

function setAdsrGate(state, gateOn, now = performance.now()) {
  const currentLevel = currentAdsrLiveLevel(state, now);
  state.liveLevel = currentLevel;
  state.liveSegmentStartLevel = currentLevel;
  state.liveSegmentStartedAt = now;
  state.livePhase = gateOn ? "attack" : "release";
}

function applyAdsrCollapsed(widgetId) {
  const state = adsrStates.get(widgetId);
  const el = document.getElementById(widgetId);
  if (!state || !el) return;
  const collapsed = !!state.graphCollapsed;
  el.querySelector(".adsr-body")?.classList.toggle("collapsed", collapsed);
  const toggleBtn = el.querySelector('[data-role="adsr-collapse-toggle"]');
  if (toggleBtn) {
    toggleBtn.textContent = collapsed ? "▸" : "▾";
    toggleBtn.setAttribute("aria-label", collapsed ? "Expand envelope graph" : "Collapse envelope graph");
    toggleBtn.setAttribute("aria-expanded", collapsed ? "false" : "true");
  }
}

function updateAdsrLivePoint(widgetId, now = performance.now(), geometry = null) {
  const state = adsrStates.get(widgetId);
  const svg = document.getElementById(widgetId)?.querySelector('[data-role="adsr-graph"]');
  if (!state || !svg) return;
  const livePosition = adsrLivePosition(state, geometry || adsrGraphGeometry(state), now);
  const livePoint = svg.querySelector('[data-role="adsr-live-point"]');
  livePoint?.setAttribute("cx", String(livePosition.x));
  livePoint?.setAttribute("cy", String(livePosition.y));
}

function updateAdsrGraph(widgetId) {
  const state = adsrStates.get(widgetId);
  const el = document.getElementById(widgetId);
  const svg = el?.querySelector('[data-role="adsr-graph"]');
  if (!state || !svg) return;
  const geometry = adsrGraphGeometry(state);
  const { start, peak, decayEnd, releaseEnd } = geometry.points;
  svg.querySelector('[data-role="adsr-curve"]')?.setAttribute(
    "d",
    `M ${start.x} ${start.y} L ${peak.x} ${peak.y} L ${decayEnd.x} ${decayEnd.y} ` +
      `L ${geometry.holdEnd} ${decayEnd.y} L ${releaseEnd.x} ${releaseEnd.y}`
  );
  const hold = svg.querySelector('[data-role="adsr-hold"]');
  hold?.setAttribute("x1", String(decayEnd.x));
  hold?.setAttribute("x2", String(geometry.holdEnd));
  hold?.setAttribute("y1", String(decayEnd.y));
  hold?.setAttribute("y2", String(decayEnd.y));
  [["peak", peak], ["decay-end", decayEnd], ["release-end", releaseEnd]].forEach(([pointId, point]) => {
    const circle = svg.querySelector(`[data-point-id="${pointId}"]`);
    circle?.setAttribute("cx", String(point.x));
    circle?.setAttribute("cy", String(point.y));
  });
  updateAdsrLivePoint(widgetId, performance.now(), geometry);
}

function tickAdsrLivePoints(now) {
  adsrStates.forEach((state, widgetId) => {
    const wasActive = state.livePhase !== "idle";
    advanceAdsrLiveState(state, now);
    if (wasActive && state.livePhase === "idle") {
      updateAdsrGraph(widgetId);
    } else if (wasActive || state.livePhase !== "idle") {
      updateAdsrLivePoint(widgetId, now);
    }
  });
  requestAnimationFrame(tickAdsrLivePoints);
}
requestAnimationFrame(tickAdsrLivePoints);

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

  updateAdsrGraph(widgetId);
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
let pendingNumericEdit = null;

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

function openNumericValuePopup(edit) {
  pendingNumericEdit = edit;
  numericEditLabel.textContent = edit.label;
  numericEditUnit.textContent = edit.unit;
  numericEditInput.value = String(edit.value);
  numericEditOverlay.classList.remove("hidden");
  numericEditInput.focus();
  numericEditInput.select();
}

function openNumericEditPopup(slider) {
  if (slider.disabled) return;
  const row = slider.closest(".port-row, .audio-out-volume-row") || slider.parentElement;
  const label = row?.querySelector(".port-label");
  const valueEl = row?.querySelector(".port-value");
  openNumericValuePopup({
    label: label ? label.textContent : "Value",
    unit: valueEl ? unitFromValueText(valueEl.textContent) : "",
    value: slider.value,
    min: slider.min !== "" ? Number(slider.min) : -Infinity,
    max: slider.max !== "" ? Number(slider.max) : Infinity,
    onSave: (value) => {
      slider.value = String(value);
      slider.dispatchEvent(new Event("input", { bubbles: true }));
    },
  });
}

function closeNumericEditPopup() {
  numericEditOverlay.classList.add("hidden");
  pendingNumericEdit = null;
}

function saveNumericEditPopup() {
  const edit = pendingNumericEdit;
  if (!edit) return;
  const parsed = parseFloatFlexible(numericEditInput.value);
  if (Number.isFinite(parsed)) edit.onSave(clamp(parsed, edit.min, edit.max));
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

      const itemLabel = document.createElement("span");
      itemLabel.className = "widget-source-label";
      const pickerLabel = def.pickerLabel || def.label;
      itemLabel.textContent = def.icon ? `${def.icon} ${pickerLabel}` : pickerLabel;
      item.appendChild(itemLabel);

      const helpBtn = document.createElement("button");
      helpBtn.type = "button";
      helpBtn.className = "widget-source-help";
      helpBtn.textContent = "?";
      helpBtn.title = `${def.label} help`;
      helpBtn.setAttribute("aria-label", `${def.label} help`);
      helpBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        openWidgetHelp(type, def.label);
      });
      item.appendChild(helpBtn);

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
widgetPickerOverlay.addEventListener("click", (e) => {
  if (!e.target.closest(".widget-picker")) closeWidgetPicker();
});

// Widget help popup: shown in-page (no new tab) from the "?" button next to
// each widget in the Add Widget picker. Content is fetched from the same
// per-widget fragment files the standalone /help/widget.html page uses.
const widgetHelpOverlay = document.getElementById("widgetHelpOverlay");
const widgetHelpTitle = document.getElementById("widgetHelpTitle");
const widgetHelpContent = document.getElementById("widgetHelpContent");
let widgetHelpFetchToken = 0;

function openWidgetHelp(type, label) {
  widgetHelpTitle.textContent = `${label} Help`;
  widgetHelpContent.innerHTML = `<div class="help-placeholder">Loading…</div>`;
  widgetHelpOverlay.classList.remove("hidden");

  const token = ++widgetHelpFetchToken;
  fetch(`/help/widgets/${encodeURIComponent(type)}.html`)
    .then((res) => {
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return res.text();
    })
    .then((html) => {
      if (token !== widgetHelpFetchToken) return;
      widgetHelpContent.innerHTML = html;
    })
    .catch(() => {
      if (token !== widgetHelpFetchToken) return;
      widgetHelpContent.innerHTML = `<div class="help-placeholder">Help content for this widget is not written yet. This page will explain what the widget does, its inputs/outputs, and how to use it.</div>`;
    });
}

function closeWidgetHelp() {
  widgetHelpOverlay.classList.add("hidden");
}

document.getElementById("widgetHelpClose").addEventListener("click", closeWidgetHelp);
widgetHelpOverlay.addEventListener("click", (e) => {
  if (!e.target.closest(".widget-help-popup")) closeWidgetHelp();
});

function startWidgetPickerDrag(e) {
  if (e.target.closest("button")) return;
  e.preventDefault();
  const picker = widgetPickerOverlay.querySelector(".widget-picker");
  const rect = picker.getBoundingClientRect();
  const offsetX = e.clientX - rect.left;
  const offsetY = e.clientY - rect.top;
  const margin = 8;

  function onMove(moveEvent) {
    picker.style.left = `${clamp(moveEvent.clientX - offsetX, margin, window.innerWidth - rect.width - margin)}px`;
    picker.style.top = `${clamp(moveEvent.clientY - offsetY, margin, window.innerHeight - rect.height - margin)}px`;
  }
  function onUp() {
    document.removeEventListener("mousemove", onMove);
    document.removeEventListener("mouseup", onUp);
  }
  document.addEventListener("mousemove", onMove);
  document.addEventListener("mouseup", onUp);
}
document.getElementById("widgetPickerHeader").addEventListener("mousedown", startWidgetPickerDrag);

widgetPickerOverlay.addEventListener("contextmenu", (e) => {
  e.preventDefault();
  if (e.target.closest(".widget-picker")) return;
  clearSelection();
  const content = clientToContent(e.clientX, e.clientY);
  openWidgetPicker(snap(content.x), snap(content.y), e.clientX, e.clientY);
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

function handleCanvasContextMenu(e) {
  if (e.target.closest(".widget-instance") || e.target.closest(".connection-path")) return;
  e.preventDefault();
  clearSelection();
  const content = clientToContent(e.clientX, e.clientY);
  openWidgetPicker(snap(content.x), snap(content.y), e.clientX, e.clientY);
}

document.addEventListener("contextmenu", (event) => {
  if (event.target.closest(".widget-instance")) event.preventDefault();
});

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
    // Middle-button click on clear canvas fits every placed widget into view;
    // dragging the same button keeps its established pan behavior.
    if (!canvasDragState.moved) fitPageToView();
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

function bindCanvasEvents(page) {
  if (page.canvasEventsBound) return;
  page.canvasEventsBound = true;
  page.canvasEl.addEventListener("mousedown", handleCanvasMousedown);
  page.canvasEl.addEventListener("contextmenu", handleCanvasContextMenu);
  page.canvasEl.addEventListener("wheel", handleCanvasWheel, { passive: false });
}

bindCanvasEvents(pages.generator);
bindCanvasEvents(pages.fx);

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
  const linkIdMap = new Map();
  const deferredLinkedOutputs = [];
  const newIds = ghosts.map(({ item, dx, dy }) => {
    const widget = createWidget(item.type, item.label, snap(pos.x + dx), snap(pos.y + dy));
    idMap.set(item.id, widget.id);
    if (isLinkedInputType(item.type) && item.params) {
      const newLinkId = generateLinkId();
      linkIdMap.set(item.params.linkId, newLinkId);
      applyWidgetParams(item.type, widget.id, { ...item.params, linkId: newLinkId });
    } else if (isLinkedOutputType(item.type) && item.params) {
      deferredLinkedOutputs.push({ id: widget.id, type: item.type, params: item.params });
    } else if (item.params) {
      applyWidgetParams(item.type, widget.id, item.params);
    }
    return widget.id;
  });
  deferredLinkedOutputs.forEach(({ id, type, params }) => {
    const remapped = linkIdMap.get(params.sourceLinkId);
    applyWidgetParams(type, id, { ...params, sourceLinkId: remapped || params.sourceLinkId });
  });

  clipboard.connections.forEach((c) => {
    const fromId = idMap.get(c.from.widgetId);
    const toId = idMap.get(c.to.widgetId);
    if (!fromId || !toId) return;
    if (connectionExists(fromId, c.from.portId, toId, c.to.portId)) return;
    connectionCounter += 1;
    const connection = {
      id: `conn-${connectionCounter}`,
      from: { widgetId: fromId, portId: c.from.portId },
      to: { widgetId: toId, portId: c.to.portId },
      color: getConnectionColorForOutput(fromId, c.from.portId),
    };
    connections.push(connection);
    connectIfAudio(connection);
  });

  selectedWidgetIds = new Set(newIds);
  updateSelectionClasses();
  refreshControlGraph();
  refreshVoiceGroups();
  refreshLinkedIO();
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
        .filter((el) => el && !LINK_WIDGET_TYPES.includes(el.dataset.widgetType))
        .map((el) => ({
          id: el.id,
          type: el.dataset.widgetType,
          label: el.querySelector(".widget-instance-label")?.textContent ||
            (WIDGET_TYPES[el.dataset.widgetType] || {}).label || el.dataset.widgetType,
          x: parseInt(el.style.left, 10) || 0,
          y: parseInt(el.style.top, 10) || 0,
          // A paste is a widget clone, not a fresh widget. Capture every
          // type's serializable state - envelopes, oscillator settings,
          // effects, controls, etc. Linked I/O is remapped below, but its
          // params must still be copied here too.
          params: JSON.parse(JSON.stringify(getWidgetParams(el.dataset.widgetType, el.id))),
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
const DIRECT_CABLES_KEY = "espressynth.directCables";
let cableSagEnabled = localStorage.getItem(CABLE_SAG_KEY) === "1";
let orthogonalCablesEnabled = localStorage.getItem(ORTHOGONAL_CABLES_KEY) === "1";
let directCablesEnabled = localStorage.getItem(DIRECT_CABLES_KEY) === "1";

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

// Every jack gets a short, straight lead before a cable may turn.  Looking at
// the jack's nearest widget edge also covers special controls whose jacks sit
// on a top, bottom, or side edge instead of the normal input-left/output-right
// layout.
function portOutwardDirection(point, bounds, fallback) {
  const distances = [
    { distance: Math.abs(point.x - bounds.left), x: -1, y: 0 },
    { distance: Math.abs(bounds.right - point.x), x: 1, y: 0 },
    { distance: Math.abs(point.y - bounds.top), x: 0, y: -1 },
    { distance: Math.abs(bounds.bottom - point.y), x: 0, y: 1 },
  ];
  distances.sort((a, b) => a.distance - b.distance);
  const nearest = distances[0];
  // A port can be one sub-pixel inside a rounded card edge.  Keep its usual
  // flow direction when several edges are effectively equally close.
  const tied = distances.filter((edge) => edge.distance - nearest.distance < 1 / zoom);
  return tied.find((edge) => edge.x === fallback.x && edge.y === fallback.y) || nearest;
}

function offsetPoint(point, direction, distance) {
  return { x: point.x + direction.x * distance, y: point.y + direction.y * distance };
}

function orthogonalPath(p1, p2, fromWidgetEl, toWidgetEl) {
  if (fromWidgetEl && toWidgetEl) {
    const fromBounds = widgetBounds(fromWidgetEl);
    const toBounds = widgetBounds(toWidgetEl);
    // Coordinates are in canvas units. Dividing by zoom keeps normal leads
    // a fixed screen-pixel length, so a cable never appears to turn on its
    // jack. Both are generous rather than tight so a cable visibly leaves
    // its widget before bending instead of hugging the border.
    const minimumLead = 24 / zoom;
    const clearance = 44 / zoom;
    const fromDirection = portOutwardDirection(p1, fromBounds, { x: 1, y: 0 });
    const toDirection = portOutwardDirection(p2, toBounds, { x: -1, y: 0 });
    const horizontalGap = p2.x - p1.x;
    const facingPorts = fromDirection.x === 1 && toDirection.x === -1;
    // Two side-by-side, vertically stacked widgets can leave less than two
    // full 14px leads between their jacks.  Use the available narrow channel
    // instead of taking the long route around both cards; six screen pixels
    // still makes the outward lead and its bend clearly visible.
    const compactVerticalRoute = facingPorts && horizontalGap >= 18 / zoom;
    const endpointLead = compactVerticalRoute
      ? Math.min(minimumLead, horizontalGap / 3)
      : minimumLead;
    const fromLead = offsetPoint(p1, fromDirection, endpointLead);
    const toLead = offsetPoint(p2, toDirection, endpointLead);

    // Standard right-output to left-input route: both endpoint leads are
    // horizontal, then one vertical jog in clear space between widgets.
    if (facingPorts && fromLead.x <= toLead.x) {
      const middleX = (fromLead.x + toLead.x) / 2;
      return roundedOrthogonalPath([
        p1,
        fromLead,
        { x: middleX, y: fromLead.y },
        { x: middleX, y: toLead.y },
        toLead,
        p2,
      ]);
    }

    // Bottom/top output into a side input: use the two endpoint corridors
    // directly. Routing around both widgets creates a large box-shaped loop.
    if (fromDirection.y !== 0 && toDirection.x !== 0) {
      return roundedOrthogonalPath([
        p1,
        fromLead,
        { x: toLead.x, y: fromLead.y },
        toLead,
        p2,
      ]);
    }

    // Side output into a top/bottom input: mirrored case above.
    if (fromDirection.x !== 0 && toDirection.y !== 0) {
      return roundedOrthogonalPath([
        p1,
        fromLead,
        { x: fromLead.x, y: toLead.y },
        toLead,
        p2,
      ]);
    }

    // Output straight into an input directly above/below it (e.g. Knob <->
    // Link In, either stacking order - Knob's outputSide can flip which
    // edge its ports sit on). Both leads are vertical, one horizontal jog
    // in the gap between widgets. Same shape as the horizontal facingPorts
    // case, rotated; the sign of fromDirection.y picks which side is "out".
    const facingPortsVertical = fromDirection.y !== 0 && toDirection.y === -fromDirection.y;
    const verticalGapOk = facingPortsVertical &&
      (fromDirection.y === 1 ? fromLead.y <= toLead.y : fromLead.y >= toLead.y);
    if (verticalGapOk) {
      const middleY = (fromLead.y + toLead.y) / 2;
      return roundedOrthogonalPath([
        p1,
        fromLead,
        { x: fromLead.x, y: middleY },
        { x: toLead.x, y: middleY },
        toLead,
        p2,
      ]);
    }

    // Widgets are stacked, overlap, or their ports face a non-standard edge.
    // Route around their shared top/bottom outside edge.  Crucially, leads
    // stay in path: no branch may replace them with a vertical line at jack.
    const targetBelowSource = p2.y >= p1.y;
    const routeY = targetBelowSource
      ? Math.max(fromBounds.bottom, toBounds.bottom) + clearance
      : Math.min(fromBounds.top, toBounds.top) - clearance;
    const routeX = fromDirection.x < 0
      ? Math.min(fromBounds.left, toBounds.left) - clearance
      : Math.max(fromBounds.right, toBounds.right) + clearance;
    return roundedOrthogonalPath([
      p1,
      fromLead,
      { x: routeX, y: fromLead.y },
      { x: routeX, y: routeY },
      { x: toLead.x, y: routeY },
      toLead,
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
  if (directCablesEnabled) return `M ${p1.x} ${p1.y} L ${p2.x} ${p2.y}`;
  if (orthogonalCablesEnabled) return orthogonalPath(p1, p2, fromWidgetEl, toWidgetEl);
  const horizontalGap = p2.x - p1.x;
  const minimumLead = 10 / zoom;
  // Forward-facing ports: never let control handles cross each other. That
  // crossing was responsible for loops between close widgets.
  const dx = horizontalGap >= minimumLead * 2
    ? Math.max(minimumLead, Math.min(horizontalGap / 2, 80 / zoom))
    : Math.max(Math.abs(horizontalGap) / 2, 40 / zoom);
  if (cableSagEnabled) {
    const sagY = Math.max(p1.y, p2.y) + Math.min(120, 28 + Math.abs(p2.x - p1.x) * 0.08);
    return `M ${p1.x} ${p1.y} C ${p1.x + dx} ${sagY}, ${p2.x - dx} ${sagY}, ${p2.x} ${p2.y}`;
  }
  return `M ${p1.x} ${p1.y} C ${p1.x + dx} ${p1.y}, ${p2.x - dx} ${p2.y}, ${p2.x} ${p2.y}`;
}

function cableStyleFromFlags() {
  if (directCablesEnabled) return "direct";
  if (orthogonalCablesEnabled) return "orthogonal";
  if (cableSagEnabled) return "sag";
  return "normal";
}

function setCableStyle(style) {
  cableSagEnabled = style === "sag";
  orthogonalCablesEnabled = style === "orthogonal";
  directCablesEnabled = style === "direct";
  localStorage.setItem(CABLE_SAG_KEY, cableSagEnabled ? "1" : "0");
  localStorage.setItem(ORTHOGONAL_CABLES_KEY, orthogonalCablesEnabled ? "1" : "0");
  localStorage.setItem(DIRECT_CABLES_KEY, directCablesEnabled ? "1" : "0");
  const select = document.getElementById("menuCableStyle");
  if (select) select.value = style;
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

function connectionsShareOutput(a, b) {
  return Boolean(a && b) &&
    a.from.widgetId === b.from.widgetId &&
    a.from.portId === b.from.portId;
}

function drawConnections() {
  ensureConnectionColors();
  connectionsLayer.innerHTML = "";
  const selectedConnection = connections.find((conn) => conn.id === selectedConnectionId) || null;
  connections.forEach((conn) => {
    const fromDot = findPortDot(conn.from.widgetId, conn.from.portId, "out");
    const toDot = findPortDot(conn.to.widgetId, conn.to.portId, "in");
    if (!fromDot || !toDot) return;

    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("class", "connection-path");
    path.dataset.connId = conn.id;
    if (connectionsShareOutput(conn, selectedConnection)) path.classList.add("selected");
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
      selectedConnectionId = conn.id;
      connectionsLayer.querySelectorAll(".connection-path").forEach((candidatePath) => {
        const candidate = connections.find((item) => item.id === candidatePath.dataset.connId);
        candidatePath.classList.toggle("selected", connectionsShareOutput(candidate, conn));
      });
    });

    
    path.addEventListener("dblclick", (e) => {
      e.stopPropagation();
      e.preventDefault();
      pushHistorySnapshot();
      disconnectIfAudio(conn);
      connections = connections.filter((c) => c.id !== conn.id);
      if (connectionTouchesLinkedIO(conn)) refreshLinkedIO();
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
  // Connecting/disconnecting "pitch" flips a step knob's label between an
  // absolute note name and a relative semitone offset (see
  // updateStepArpKnobVisual) - refresh right away instead of waiting for
  // the next value to happen to propagate down that cable. Connecting/
  // disconnecting "clock" (or "rate") flips whether the internal fallback
  // clock should be running at all (see stepArpRestartInternalClock) -
  // same reasoning, refresh right away rather than waiting for a value.
  stepArpStates.forEach((_, widgetId) => {
    updateStepArpVisual(widgetId);
    stepArpRestartInternalClock(widgetId);
  });
  drumSequencerStates.forEach((_, widgetId) => {
    updateDrumSequencerVisual(widgetId);
    drumSequencerRestartInternalClock(widgetId);
  });
  sampleHoldStates.forEach((_, widgetId) => updateSampleHoldVisual(widgetId));
  waveFolderStates.forEach((_, widgetId) => updateWaveFolderVisual(widgetId));
  envelopeFollowerStates.forEach((_, widgetId) => updateEnvelopeFollowerVisual(widgetId));
  panStates.forEach((_, widgetId) => updatePanVisual(widgetId));
  refreshWiredSliderLocks();
}

// Hovering a Link In / Link Out reveals its wireless partner for as long as the
// cursor stays on it: a dashed, dimmed cable drawn in the colour of the cable that
// feeds the Link In. Purely decorative - never enters `connections`, and the next
// drawConnections() wipes it along with the rest of the layer.
function linkedIoHintPairs(widgetId) {
  const type = document.getElementById(widgetId)?.dataset.widgetType;
  if (isLinkedInputType(type)) {
    const sourceState = linkedInputStates.get(widgetId);
    const linkId = sourceState?.linkId;
    if (!linkId) return [];
    return [...linkedOutputStates.entries()]
      .filter(([, state]) => state.sourceLinkId === linkId)
      .flatMap(([outputId]) => Array.from({ length: sourceState.linkCount }, (_, index) => ({
        inputId: widgetId,
        inputPortId: linkedIoPortId("in", index, sourceState.linkCount),
        outputId,
        outputPortId: linkedIoPortId("out", index, sourceState.linkCount),
      })));
  }
  if (isLinkedOutputType(type)) {
    const sourceLinkId = linkedOutputStates.get(widgetId)?.sourceLinkId;
    if (!sourceLinkId) return [];
    const source = [...linkedInputStates.entries()].find(([, state]) => state.linkId === sourceLinkId);
    return source ? Array.from({ length: source[1].linkCount }, (_, index) => ({
      inputId: source[0],
      inputPortId: linkedIoPortId("in", index, source[1].linkCount),
      outputId: widgetId,
      outputPortId: linkedIoPortId("out", index, source[1].linkCount),
    })) : [];
  }
  return [];
}

function clearLinkedIoHint() {
  connectionsLayer.querySelectorAll(".connection-path-link-hint").forEach((path) => path.remove());
}

function showLinkedIoHint(widgetId) {
  clearLinkedIoHint();
  linkedIoHintPairs(widgetId).forEach(({ inputId, inputPortId, outputId, outputPortId }) => {
    // findPortDot only sees the page that is currently on screen, so a partner
    // sitting on the other page simply yields no hint.
    const inDot = findPortDot(inputId, inputPortId, "in");
    const outDot = findPortDot(outputId, outputPortId, "out");
    if (!inDot || !outDot) return;
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("class", "connection-path-link-hint");
    path.setAttribute(
      "d",
      bezierPath(
        portPosition(outDot),
        portPosition(inDot),
        outDot.closest(".widget-instance"),
        inDot.closest(".widget-instance")
      )
    );
    const feed = connections.find((conn) => conn.to.widgetId === inputId && conn.to.portId === inputPortId);
    if (feed?.color) path.style.stroke = feed.color;
    connectionsLayer.appendChild(path);
  });
}

document.addEventListener("mouseover", (e) => {
  const widget = e.target.closest?.(
    '.widget-instance[data-widget-type^="linked-input"], .widget-instance[data-widget-type^="linked-output"]'
  );
  if (widget) showLinkedIoHint(widget.id);
  else clearLinkedIoHint();
});

// Sliders inside input port rows belong to those ports. A cable owns its
// target parameter regardless of whether it carries CV or audio-rate signal.
// Settings and decorative sliders have no port row and stay editable.
function refreshWiredSliderLocks() {
  document.querySelectorAll(".widget-instance .port-row-in[data-port-id]").forEach((row) => {
    const slider = row.querySelector("input.port-slider");
    if (!slider) {
      if (row.closest('[data-widget-type="comb-filter"]') && row.dataset.portId === "polarity")
        updateCombFilterVisual(row.closest(".widget-instance").id);
      return;
    }
    const widget = row.closest(".widget-instance");
    if (!widget) return;
    const wired = connections.some((c) => c.to.widgetId === widget.id && c.to.portId === row.dataset.portId);
    // A row greyed out by its widget's mode (e.g. Duty Cycle outside Square, SuperSaw/Morph
    // outside their waveform) stays locked too.
    slider.disabled = wired || row.classList.contains("disabled");
    slider.title = wired ? "Von verbundenem Port gesteuert" : "";
    row.classList.toggle("port-row-modulated", wired);
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
    if (conn) {
      connections.forEach((candidate) => {
        if (connectionsShareOutput(candidate, conn)) candidate.color = color;
      });
    }
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
  const toRemove = connections.filter(
    (c) => c.from.widgetId === widgetId || c.to.widgetId === widgetId
  );
  toRemove.forEach(disconnectIfAudio);
  connections = connections.filter((c) => !toRemove.includes(c));
}

function connectionTouchesLinkedIO(conn) {
  const fromType = document.getElementById(conn.from.widgetId)?.dataset.widgetType;
  const toType = document.getElementById(conn.to.widgetId)?.dataset.widgetType;
  return isLinkedIoType(fromType) || isLinkedIoType(toType);
}

function shouldConnectAudio(conn) {
  if (getPortKind(conn.to.widgetId, conn.to.portId, "in") !== "audio") return false;

  // Preserve the existing relaxed rule for ordinary control widgets such as
  // Knob/Const, which own usable WebAudio ConstantSourceNodes. Linked I/O is
  // different: its GainNode is silent when its upstream source is MIDI or
  // another scalar control, so both ends must genuinely be audio here.
  return !connectionTouchesLinkedIO(conn) ||
    getPortKind(conn.from.widgetId, conn.from.portId, "out") === "audio";
}

function connectIfAudio(conn) {
  if (!shouldConnectAudio(conn)) return;
  const linkedRoute = connectionTouchesLinkedIO(conn);
  if (linkedRoute && linkedAudioRouteIds.has(conn.id)) return;
  audioEngine.connect(conn.from.widgetId, conn.from.portId, conn.to.widgetId, conn.to.portId);
  if (linkedRoute) linkedAudioRouteIds.add(conn.id);
}

function disconnectIfAudio(conn) {
  if (connectionTouchesLinkedIO(conn)) {
    if (!linkedAudioRouteIds.delete(conn.id)) return;
    audioEngine.disconnect(conn.from.widgetId, conn.from.portId, conn.to.widgetId, conn.to.portId);
    return;
  }

  // toKind alone - mirrors the relaxed connect() gate below (see its
  // comment): a control-only source can still be wired into an audio-shaped
  // destination, so its removal needs the matching real disconnect too.
  // audioEngine.disconnect() no-ops safely (try/catch) when nothing was
  // actually wired.
  const toKind = getPortKind(conn.to.widgetId, conn.to.portId, "in");
  if (toKind === "audio") {
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
  if (toRemove.some(connectionTouchesLinkedIO)) refreshLinkedIO();
  refreshControlGraph();
  refreshVoiceGroups();
  if (direction === "in" && portId === "layer" && knobStates.has(widgetId)) {
    renderKnobLayerSelector(widgetId);
  }
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
      const fromKind = getPortKind(fromWidgetId, fromPortId, "out");
      const targetType = document.getElementById(toWidgetId)?.dataset.widgetType;
      const toKind = isLinkedInputType(targetType)
        ? fromKind
        : getPortKind(toWidgetId, toPortId, "in");

      if (isLinkedInputType(targetType) && getPortKind(toWidgetId, toPortId, "in") !== toKind) {
        connections.filter(
          (c) => c.to.widgetId === toWidgetId && c.to.portId === toPortId
        ).forEach(disconnectIfAudio);
        connections = connections.filter(
          (c) => !(c.to.widgetId === toWidgetId && c.to.portId === toPortId)
        );
      }

      if (toKind !== "audio") {
        connections.filter(
          (c) => c.to.widgetId === toWidgetId && c.to.portId === toPortId
        ).forEach(disconnectIfAudio);
        connections = connections.filter(
          (c) => !(c.to.widgetId === toWidgetId && c.to.portId === toPortId)
        );
      }
      const connection = {
        id: `conn-${connectionCounter}`,
        from: { widgetId: fromWidgetId, portId: fromPortId },
        to: { widgetId: toWidgetId, portId: toPortId },
        color: getConnectionColorForOutput(fromWidgetId, fromPortId),
      };
      connections.push(connection);
      if (isLinkedInputType(targetType)) autoNameLinkedInput(toWidgetId, fromWidgetId, fromPortId, toPortId);

      // toKind alone, not fromKind too: a control-only source (Knob, Const,
      // ...) can still own a real WebAudio node (see audio-engine.js's
      // _resolveOutputFromEntry) and needs wiring into an audio-shaped
      // destination like a Multiplexer/Mixer "inN" - otherwise a Debug/
      // Voltmeter reading that destination's output stays stuck at 0
      // (tickDebugVoltmeterAudioSources samples the real WebAudio graph, not
      // the discrete propagate() path). Harmless when the source has no such
      // node: audioEngine.connect()'s _resolveScaledOutput resolves to null
      // and the connect is silently skipped, same as before.
      if (connectionTouchesLinkedIO(connection)) refreshLinkedIO();
      else connectIfAudio(connection);

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
        if (target && ["debug", "voltmeter", "rgb-led"].includes(target.dataset.widgetType)) {
          return;
        }
      }
      // Cable path: the device drives the target itself - see withDeviceSyncMuted().
      withDeviceSyncMuted(() => deliverToWidget(c.to.widgetId, c.to.portId, value, voiceIndex));
    });
}

// The device is the leading engine and runs standalone: it wires every patch cable and
// handles all MIDI itself. The browser therefore must never push values it merely derived -
// from a patch cable (propagate()) or from MIDI the device mirrored to it - back to the
// device. While such a derived update runs, sendLiveParamUpdate()/
// sendLiveParamUpdateThrottled()/savePresetNow() are muted; direct user interaction (a
// widget's own slider/toggle - which also goes through deliverToWidget() -, knob drag,
// settings dialog, MIDI learn) still reaches the device.
let deviceSyncMuteDepth = 0;

function withDeviceSyncMuted(fn) {
  deviceSyncMuteDepth++;
  try {
    return fn();
  } finally {
    deviceSyncMuteDepth--;
  }
}

function deliverToWidget(widgetId, portId, value, voiceIndex) {
  const el = document.getElementById(widgetId);
  if (!el) return;

  if (isLinkedInputType(el.dataset.widgetType)) {
    forwardLinkedInputValue(widgetId, portId, value, voiceIndex);
  }

  if (el.dataset.widgetType === "knob" && portId === "layer") {
    const state = knobStates.get(widgetId);
    if (state) {
      setKnobActiveLayer(widgetId, knobLayerIndexFromCv(value, state.layerCount), false);
      sendLiveParamUpdateThrottled(widgetId, { layer: clamp(Number(value) || 0, 0, CV_MAX_VOLTS) });
    }
  }

  if (el.dataset.widgetType === "button" && portId === "layer") {
    const state = buttonStates.get(widgetId);
    if (state) {
      setButtonActiveLayer(widgetId, knobLayerIndexFromCv(value, state.layerCount), false);
      sendLiveParamUpdateThrottled(widgetId, { layer: clamp(Number(value) || 0, 0, CV_MAX_VOLTS) });
    }
  }

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

  if (el.dataset.widgetType === "rgb-led") {
    const state = rgbLedStates.get(widgetId);
    const num = Number(value);
    if (state) {
      state.lastRgb = rgbLedColorAt(state.stops, Number.isNaN(num) ? 0 : num);
      updateRgbLedDisplay(widgetId);
    }
  }

  if (el.dataset.widgetType === "multiplier") {
    const state = multiplierStates.get(widgetId);
    if (state) {
      const idx = parseInt(String(portId).replace("in", ""), 10) - 1;
      if (idx >= 0 && idx < state.inputCount) {
        const num = Number(value);
        state.values[idx] = Number.isNaN(num) ? 0 : num;
        // If this widget is already in real-audio mode (some OTHER input is
        // a genuine audio-rate edge), a control-rate value like this one
        // never reaches the real WebAudio node through the graph edge
        // itself - push it in directly, or it silently reads as the
        // node's static default forever (see audio-engine.js's "multiplier"
        // setParam case).
        if (state.audioMode) audioEngine.setParam(widgetId, portId, state.values[idx]);
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
        // See "multiplier" above.
        if (state.audioMode) audioEngine.setParam(widgetId, portId, state.values[idx]);
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
        // See "multiplier" above.
        if (state.audioMode) audioEngine.setParam(widgetId, portId, state.values[idx]);
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

  if (["logic-and", "logic-or", "logic-xor", "logic-not"].includes(el.dataset.widgetType)) {
    const state = logicGateStates.get(widgetId);
    if (state) {
      const idx = portId === "in" ? 0 : parseInt(String(portId).replace("in", ""), 10) - 1;
      if (idx >= 0 && idx < logicGateInputCount(state)) {
        const num = Number(value);
        state.values[idx] = Number.isNaN(num) ? 0 : num;
        recomputeLogicGate(widgetId);
        markDirty();
      }
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

  // Mirror of the switch/multiplexer cases above, inverted: a single scalar
  // "in" value re-propagated to whichever OUTPUT is currently selected, but
  // ONLY on an actual change (num !== state.lastValue) - flipping the
  // selection itself must never re-emit the current "in" value onto the
  // newly active output (see outputMultiplexerSetActiveIndex/
  // setOutputSwitchSelectOut2), or switching away and back would leak the
  // OTHER output's most recent value onto this one instead of leaving it
  // showing whatever it last genuinely received.
  if (el.dataset.widgetType === "output-switch" && portId === "in") {
    const state = outputSwitchStates.get(widgetId);
    const num = Number(value);
    if (state && !Number.isNaN(num) && num !== state.lastValue) {
      state.lastValue = num;
      propagate(widgetId, state.selectOut2 ? "out2" : "out1", num);
    }
  }

  if (el.dataset.widgetType === "output-multiplexer" && portId === "in") {
    const state = outputMultiplexerStates.get(widgetId);
    const num = Number(value);
    if (state && !Number.isNaN(num) && num !== state.lastValue) {
      state.lastValue = num;
      propagate(widgetId, `out${state.activeIndex + 1}`, num);
    }
  }

  if (isLfoLikeType(el.dataset.widgetType) && portId === "rate") {
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

  if (isLfoLikeType(el.dataset.widgetType) && portId === "gain") {
    const state = lfoStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.gain = Number.isNaN(num) ? CV_MAX_VOLTS : clamp(num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "gain", state.gain);
      updateLfoVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { gain: state.gain });
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

  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  if (isLfoLikeType(el.dataset.widgetType) && portId === "reset") {
    const state = lfoStates.get(widgetId);
    if (state) {
      const above = Number(value) >= GATE_THRESHOLD_VOLTS;
      if (!state.resetPrevByVoice) state.resetPrevByVoice = new Map();
      const voiceKey = voiceIndex ?? "global";
      const wasAbove = state.resetPrevByVoice.get(voiceKey) === true;
      // Global: the shared LFO belongs to the whole chord - only the first
      // note (no other voice's reset already high) restarts it, later notes
      // just join; matches the firmware's OR-over-all-voices reset edge.
      const othersAbove = state.global === true &&
        [...state.resetPrevByVoice].some(([key, high]) => key !== voiceKey && high === true);
      if (above && !wasAbove && !othersAbove) {
        audioEngine.resetLfoPhase(widgetId, state.global === true ? 0 : voiceIndex);
        state.cyclePos = 0;
        state.linearPos = 0;
        state.lastTick = performance.now();
        if (state.oneShot) {


          state.oneShotDone = false;
          state.oneShotAccum = 0;
        }
        tickLfo(widgetId);
      }
      state.resetPrevByVoice.set(voiceKey, above);
    }
    const dot = el.querySelector('.port-row-in [data-port-id="reset"]');
    setLedActive(dot, Number(value) >= GATE_THRESHOLD_VOLTS);
  }

  
  
  
  
  if (isLfoLikeType(el.dataset.widgetType) && portId === "oneShot") {
    const state = lfoStates.get(widgetId);
    if (state) {
      state.oneShot = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "oneShot", state.oneShot);
      updateLfoVisual(widgetId);
      sendLiveParamUpdate(widgetId, { oneShot: state.oneShot });
    }
  }

  
  
  if (isLfoLikeType(el.dataset.widgetType) && portId === "global") {
    const state = lfoStates.get(widgetId);
    if (state) {
      state.global = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "global", state.global);
      updateLfoVisual(widgetId);
      sendLiveParamUpdate(widgetId, { global: state.global });
    }
  }

  
  
  
  
  
  if (el.dataset.widgetType === "wavetable" && portId === "morph") {
    const num = Number(value);
    if (!Number.isNaN(num)) setLfoMorph(widgetId, num);
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

  if (el.dataset.widgetType === "oscillator" && portId === "feedback") {
    const state = oscillatorStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.feedback = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "feedback", state.feedback);
      updateOscillatorFeedbackVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { feedback: state.feedback });
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "pitchCv") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = fmOscillatorStates.get(widgetId);
      if (state) {
        state.pitchCv = num;
        recomputeFmOscillatorFreq(widgetId, voiceIndex);
      }
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "tune") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      const state = fmOscillatorStates.get(widgetId);
      if (state) {
        state.tuneVolts = clamp(num, 0, CV_MAX_VOLTS);
        recomputeFmOscillatorFreq(widgetId, voiceIndex);
        updateFmOscillatorTuneVisual(widgetId);
        if (voiceIndex === undefined) {
          sendLiveParamUpdateThrottled(widgetId, { tune: state.tuneVolts });
        }
      }
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "gain") {
    const state = fmOscillatorStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.gain = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "gain", state.gain);
      updateFmOscillatorGainVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { gain: state.gain });
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "amplitude") {
    const num = Number(value);
    const safe = Number.isNaN(num) ? 1 : num;
    if (voiceIndex !== undefined) {
      audioEngine.setVoiceParam(widgetId, voiceIndex, "amplitude", safe);
    } else {
      const state = fmOscillatorStates.get(widgetId);
      if (state) {
        state.amplitude = safe;
        audioEngine.setParam(widgetId, "amplitude", state.amplitude);
        sendLiveParamUpdateThrottled(widgetId, { amplitude: state.amplitude });
      }
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "fmIndex") {
    const state = fmOscillatorStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.fmIndex = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "fmIndex", state.fmIndex);
      updateFmOscillatorIndexVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { fmIndex: state.fmIndex });
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "feedback") {
    const state = fmOscillatorStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.feedback = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "feedback", state.feedback);
      updateFmOscillatorFeedbackVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { feedback: state.feedback });
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "dutyCycle") {
    const state = fmOscillatorStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.dutyCycle = clamp(Number.isNaN(num) ? CV_MAX_VOLTS / 2 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "dutyCycle", state.dutyCycle);
      updateFmOscillatorDutyCycleVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { dutyCycle: state.dutyCycle });
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "morph") {
    setFmOscillatorMorph(widgetId, value);
  }

  if (el.dataset.widgetType === "fm-oscillator" && (portId === "superSawDetune" || portId === "superSawMix")) {
    const state = fmOscillatorStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state[portId] = clamp(Number.isNaN(num) ? CV_MAX_VOLTS / 2 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, portId, state[portId]);
      updateFmOscillatorSuperSawVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
    }
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "gate") {
    if (voiceIndex !== undefined) {
      audioEngine.setVoiceParam(widgetId, voiceIndex, "gate", Number(value));
    } else {
      audioEngine.setParam(widgetId, "gate", Number(value));
    }
    const dot = el.querySelector('.port-row-in [data-port-id="gate"]');
    setLedActive(dot, Number(value) > GATE_THRESHOLD_VOLTS);
  }

  if (el.dataset.widgetType === "fm-oscillator" && portId === "waveform") {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      setFmOscillatorWaveType(widgetId, mapFmWaveformValue(num));
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
      state.amount = Number.isNaN(num) ? CV_MAX_VOLTS / 2 : clamp(num, 0, CV_MAX_VOLTS);
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

  // Sixteen step CV ports - a live cable lands here exactly like a local
  // knob drag (see setStepArpValue/startStepArpKnobDrag above); the "wired
  // wins outright" rule itself is enforced by the disabled-knob styling in
  // updateStepArpKnobVisual and, authoritatively, by the firmware/audio
  // engine's own inPorts-over-local-knob precedence - this just keeps the
  // browser-side display in sync.
  if (el.dataset.widgetType === "step-arpeggiator" && /^step(1[0-6]|[1-9])$/.test(portId)) {
    const num = Number(value);
    if (!Number.isNaN(num)) {
      setStepArpValue(widgetId, Number(portId.slice(4)) - 1, num);
    }
  }

  // clock/gate/pitch/direction - client-side simulation of
  // nodes/StepArpeggiatorNode.h's render() (see stepArpNextIndex above),
  // driven by whatever already ticks/propagates these cables in the editor
  // (typically a Clock widget's own local pulse timer): flashes the
  // clock/gate input LEDs and the gateOut output LED, highlights the
  // currently held step, and propagates "cv"/"gateOut" downstream so a
  // chained widget previews correctly too - same purpose Clock's own
  // fireClockPulse serves for its "pulse" output.
  if (el.dataset.widgetType === "step-arpeggiator") {
    const state = stepArpStates.get(widgetId);
    if (state && portId === "gate") {
      const gateOn = Number(value) >= GATE_THRESHOLD_VOLTS;
      setLedActive(el.querySelector('[data-port-id="gate"][data-port-dir="in"]'), gateOn);
      if (gateOn && !state.gatePrev) {
        // Run/Reset rising edge: jump to step 1 and play it immediately
        // (CV + gateOut pulse + advance to the following step), instead of
        // waiting for the first clock edge afterward - see
        // stepArpTriggerStep/nodes/StepArpeggiatorNode.h's triggerStep().
        state.currentStep = 0;
        state.pingPongForward = true;
        // The internal fallback clock always resyncs to Gate too, whether
        // or not it is actually driving playback right now (see
        // nodes/StepArpeggiatorNode.h's render() - same rule): its next
        // tick becomes exactly one period from this exact moment.
        stepArpRestartInternalClock(widgetId);
        stepArpTriggerStep(widgetId, state, el);
      } else if (!gateOn && state.gatePrev) {
        // Gate falling edge - end any still-running gateOut pulse
        // immediately, not just on the next rising edge (see
        // nodes/StepArpeggiatorNode.h's render() - same rule, mirrored here
        // purely for the editor's own LED/propagate() simulation).
        clearTimeout(state.gateOutTimer);
        state.gateOutTimer = null;
        setLedActive(el.querySelector('[data-port-id="gateOut"][data-port-dir="out"]'), false);
        propagate(widgetId, "gateOut", GATE_OFF_VOLTS);
      }
      state.gateVolts = Number(value) || 0;
      state.gatePrev = gateOn;
    } else if (state && portId === "clock") {
      const clockOn = Number(value) >= GATE_THRESHOLD_VOLTS;
      setLedActive(el.querySelector('[data-port-id="clock"][data-port-dir="in"]'), clockOn);
      if (clockOn && !state.clockPrev && state.gateVolts >= GATE_THRESHOLD_VOLTS) {
        stepArpTriggerStep(widgetId, state, el);
      }
      state.clockPrev = clockOn;
    } else if (state && portId === "pitch") {
      state.pitchVolts = Number(value) || 0;
    } else if (state && portId === "direction") {
      state.directionVolts = Number(value) || 0;
      updateStepArpDirectionVisual(widgetId);
    } else if (state && portId === "pulseTime") {
      // Incoming cable carries raw 0-10V, same as the local slider - keeps
      // the simulated gateOut LED flash duration (see the "clock" branch
      // above) accurate even when this port is wired, and refreshes the
      // (now disabled - see updateStepArpPulseTimeVisual) slider display.
      state.pulseMs = cvToRange(Number(value) || 0, state.pulseTimeMinSec, state.pulseTimeMaxSec, "linear") * 1000;
      updateStepArpPulseTimeVisual(widgetId);
    } else if (state && portId === "rate") {
      // Incoming cable carries raw 0-10V, same as the local slider - only
      // actually affects playback while "clock" itself is unwired (see
      // stepArpRestartInternalClock), but keeps the (then-disabled) slider
      // display accurate either way.
      state.clockCvVolts = Number(value) || 0;
      updateStepArpRateVisual(widgetId);
      stepArpRestartInternalClock(widgetId);
    }
  }

  if (el.dataset.widgetType === "drum-sequencer") {
    const state = drumSequencerStates.get(widgetId);
    if (state && portId === "gate") {
      const gateOn = Number(value) >= GATE_THRESHOLD_VOLTS;
      setLedActive(el.querySelector('[data-port-id="gate"][data-port-dir="in"]'), gateOn);
      state.gateVolts = Number(value) || 0;
      if (gateOn && !state.gatePrev) {
        state.currentStep = 0;
        state.lastStep = -1;
        drumSequencerStopTriggers(widgetId);
        drumSequencerRestartInternalClock(widgetId);
        drumSequencerTriggerStep(widgetId);
      } else if (!gateOn && state.gatePrev) {
        drumSequencerStopTriggers(widgetId);
        // Gate dropped while a quantized switch was still armed - there is no
        // more running playback to quantize against, so resolve it now.
        if (state.pendingPatternIndex !== -1) {
          state.activePatternIndex = state.pendingPatternIndex;
          state.pendingPatternIndex = -1;
          updateDrumSequencerVisual(widgetId);
        }
      }
      state.gatePrev = gateOn;
    } else if (state && portId === "clock") {
      const clockOn = Number(value) >= GATE_THRESHOLD_VOLTS;
      setLedActive(el.querySelector('[data-port-id="clock"][data-port-dir="in"]'), clockOn);
      if (clockOn && !state.clockPrev && state.gateVolts >= GATE_THRESHOLD_VOLTS) drumSequencerTriggerStep(widgetId);
      state.clockPrev = clockOn;
    } else if (state && portId === "rate") {
      state.clockCvVolts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
      updateDrumSequencerVisual(widgetId);
      drumSequencerRestartInternalClock(widgetId);
    } else if (state && portId === "pattern") {
      // "pattern" is control-only (see PortTables::isControlOnlyInput), same
      // convention as Multiplexer's "select": wired CV picks the pattern
      // directly, same quantize/immediate handling as the on-widget slider.
      const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
      const idx = drumPatternIndexFromCv(volts, state.patterns.length);
      if (state.switchAtEnd && state.gatePrev) {
        state.pendingPatternIndex = idx;
      } else {
        state.activePatternIndex = idx;
        state.pendingPatternIndex = -1;
        updateDrumSequencerVisual(widgetId);
      }
    } else if (state && portId === "switchAtEnd") {
      // Control-only, same dual-source-fed convention as LFO's oneShot/global.
      state.switchAtEnd = Number(value) >= GATE_THRESHOLD_VOLTS;
      updateDrumSequencerVisual(widgetId);
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

  if (el.dataset.widgetType === "wave-player" && portId === "gain") {
    const state = wavePlayerStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.gain = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "gain", state.gain);
      updateWavePlayerGainVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { gain: state.gain });
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

  if (el.dataset.widgetType === "mda-piano") {
    const state = mdaPianoStates.get(widgetId);
    if (state && ["enableModWheel", "enablePitchWheel"].includes(portId)) {
      state[portId] = Number(value) >= 0.5;
      audioEngine.setParam(widgetId, portId, state[portId]);
      updateMdaPianoControls(widgetId);
      sendLiveParamUpdate(widgetId, { [portId]: state[portId] });
    } else if (state && MDA_PIANO_CV_PARAMS.includes(portId)) {
      const num = Number(value);
      if (!Number.isNaN(num)) {
        state[portId] = clamp(num, 0, CV_MAX_VOLTS);
        audioEngine.setParam(widgetId, portId, state[portId]);
        updateMdaPianoControls(widgetId);
        sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
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

  
  
  if (el.dataset.widgetType === "gain" && portId === "curve") {
    const state = gainStates.get(widgetId);
    if (state) {
      const num = Number(value);
      state.curve = clamp(Number.isNaN(num) ? 0 : num, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "curve", state.curve);
      updateGainVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { curve: state.curve });
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

  if (el.dataset.widgetType === "comb-filter" && COMB_FILTER_PORTS.includes(portId)) {
    const state = combFilterStates.get(widgetId);
    if (state) {
      const volts = combFilterCv(Number(value));
      state[portId] = volts;
      if (voiceIndex !== undefined) audioEngine.setVoiceParam(widgetId, voiceIndex, portId, volts);
      else audioEngine.setParam(widgetId, portId, volts);
      updateCombFilterVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { [portId]: volts });
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

  if ((el.dataset.widgetType === "filter" || el.dataset.widgetType === "curtis-filter") && portId === "filterType") {
    setFilterType(widgetId, filterTypeFromCv(value));
  }

  if ((el.dataset.widgetType === "filter" || el.dataset.widgetType === "curtis-filter") && portId === "slope") {
    setFilterSlope(widgetId, filterSlopeFromCv(value));
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

  if (el.dataset.widgetType === "bitcrusher" && portId === "crush") {
    const state = bitcrusherStates.get(widgetId);
    if (state) {
      state.crush = Math.max(0, Math.min(10, Number(value) || 0));
      audioEngine.setParam(widgetId, "crush", state.crush);
      updateBitcrusherVisual(widgetId);
      sendLiveParamUpdateThrottled(widgetId, { crush: state.crush });
    }
  }

  if (el.dataset.widgetType === "wave-folder" && WAVE_FOLDER_SLIDER_PORTS.includes(portId)) {
    setWaveFolderParam(widgetId, portId, Number(value));
  }

  if (el.dataset.widgetType === "envelope-follower" && ENVELOPE_FOLLOWER_SLIDER_PORTS.includes(portId)) {
    setEnvelopeFollowerParam(widgetId, portId, Number(value));
  }

  if (el.dataset.widgetType === "burst-generator") {
    if (portId === "mode") {
      setBurstGeneratorMode(widgetId, Number(value) >= GATE_THRESHOLD_VOLTS ? "delay" : "burst");
    } else if (BURST_GENERATOR_SLIDER_PORTS.includes(portId)) {
      setBurstGeneratorParam(widgetId, portId, Number(value));
    }
  }

  if (el.dataset.widgetType === "pan" && portId === "pan") {
    setPanParam(widgetId, Number(value));
  }

  if (el.dataset.widgetType === "graphic-eq" && portId === "gain") {
    const state = graphicEqStates.get(widgetId);
    if (state) {
      state.outputGain = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
      audioEngine.setParam(widgetId, "gain", state.outputGain);
      const valueSpan = el.querySelector('[data-role="graphic-eq-gain-value"]');
      if (valueSpan) valueSpan.textContent = formatNumber(state.outputGain);
      sendLiveParamUpdateThrottled(widgetId, { gain: state.outputGain });
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

  if (el.dataset.widgetType === "chorus" && portId === "bypass") {
    const state = chorusStates.get(widgetId);
    if (state) {
      state.bypass = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "bypass", state.bypass);
      updateChorusVisual(widgetId);
      sendLiveParamUpdate(widgetId, { bypass: state.bypass });
    }
  }

  if (el.dataset.widgetType === "bitcrusher" && portId === "bypass") {
    const state = bitcrusherStates.get(widgetId);
    if (state) {
      state.bypass = Number(value) >= GATE_THRESHOLD_VOLTS;
      audioEngine.setParam(widgetId, "bypass", state.bypass);
      updateBitcrusherVisual(widgetId);
      sendLiveParamUpdate(widgetId, { bypass: state.bypass });
    }
  }

  if (hasBypass(el.dataset.widgetType) &&
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
        const gateOn = Number(value) > GATE_THRESHOLD_VOLTS;
        if (voiceIndex !== undefined) {
          audioEngine.setVoiceParam(widgetId, voiceIndex, "gate", Number(value));
        } else {
          audioEngine.setParam(widgetId, "gate", Number(value));
        }
        const dot = el.querySelector('.port-row-in [data-port-id="gate"]');
        setLedActive(dot, gateOn);
        if (voiceIndex !== undefined) {
          if (gateOn) {
            state.liveVoiceIndex = voiceIndex;
            setAdsrGate(state, true);
          } else if (state.liveVoiceIndex === voiceIndex) {
            setAdsrGate(state, false);
          }
        } else if (gateOn !== state.liveGate) {
          state.liveGate = gateOn;
          setAdsrGate(state, gateOn);
        }
        updateAdsrGraph(widgetId);
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

  if (el.dataset.widgetType === "dx-envelope") {
    const state = dxEnvelopeStates.get(widgetId);
    if (state) {
      if (portId === "gate") {
        const gateOn = Number(value) > GATE_THRESHOLD_VOLTS;
        if (voiceIndex !== undefined) audioEngine.setVoiceParam(widgetId, voiceIndex, "gate", Number(value));
        else audioEngine.setParam(widgetId, "gate", Number(value));
        const dot = el.querySelector('.port-row-in [data-port-id="gate"]');
        setLedActive(dot, gateOn);
        if (voiceIndex !== undefined) {
          if (gateOn) {
            state.liveVoiceIndex = voiceIndex;
            setDxEnvelopeGate(state, true);
          } else if (state.liveVoiceIndex === voiceIndex) {
            setDxEnvelopeGate(state, false);
          }
        } else if (gateOn !== state.liveGate) {
          state.liveGate = gateOn;
          setDxEnvelopeGate(state, gateOn);
        }
        updateDxEnvelopeVisuals(widgetId);
      } else if (portId === "velocity") {
        const velocity = Number.isFinite(Number(value)) ? Number(value) : CV_MAX_VOLTS;
        if (voiceIndex !== undefined) audioEngine.setVoiceParam(widgetId, voiceIndex, "velocity", velocity);
        else audioEngine.setParam(widgetId, "velocity", velocity);
      } else if (/^level[1-4]$/.test(portId)) {
        state[portId] = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
        audioEngine.setParam(widgetId, portId, state[portId]);
        updateDxEnvelopeVisuals(widgetId);
        sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
      } else if (/^time[1-4]$/.test(portId)) {
        const volts = clamp(Number(value) || 0, 0, CV_MAX_VOLTS);
        state[portId] = Math.max(
          0.001,
          cvToSeconds(volts, state[`${portId}MaxSeconds`], state[`${portId}Curve`])
        );
        audioEngine.setParam(widgetId, portId, state[portId]);
        updateDxEnvelopeVisuals(widgetId);
        sendLiveParamUpdateThrottled(widgetId, { [portId]: state[portId] });
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
  const state = midiInStates.get(widgetId) || midiFilterStates.get(widgetId);
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

function routeMidiWidget(data, id, state) {
    const slotState = midiVoiceSlotStates.get(id);
    if (!state || !slotState) return;

    if (data.midiType === "note_on") {
      if (state.voices <= 1) {
        if (midiInStates.has(id) && isFxMidiIn(id) && slotState.monoCurrentNote !== null) return;
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
      if (midiFilterStates.has(id) && (data.control === 120 || data.control === 123)) {
        if (state.voices <= 1) {
          propagate(id, "gate", GATE_OFF_VOLTS);
          if (slotState.monoCurrentNote !== null) deliverMidiSplitZones(id, slotState.monoCurrentNote, 0, false, undefined);
        } else {
          slotState.slots.forEach((slot, voiceIndex) => {
            if (slot.note !== null) {
              propagate(id, "gate", GATE_OFF_VOLTS, voiceIndex);
              deliverMidiSplitZones(id, slot.note, 0, false, voiceIndex);
            }
            slot.note = null;
            slot.held = false;
          });
        }
        slotState.monoHeldNotes.clear();
        slotState.monoCurrentNote = null;
        slotState.pendingRelease = false;
        slotState.pedalDown = false;
        return;
      }
      if (data.control !== 64) return; 

      const wasDown = slotState.pedalDown;
      const nowDown = data.value >= 64; 
      slotState.pedalDown = nowDown;
      if (!wasDown || nowDown) return; 

      if (state.voices <= 1) {
        if (slotState.pendingRelease) {
          propagate(id, "gate", GATE_OFF_VOLTS);
          if (slotState.monoCurrentNote !== null) deliverMidiSplitZones(id, slotState.monoCurrentNote, 0, false, undefined);
          slotState.monoCurrentNote = null;
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
    } else if (data.midiType === "aftertouch") {
      propagate(id, "aftertouch", (data.value / 127) * CV_MAX_VOLTS);
    } else if (data.midiType === "pitchwheel") {
      const cents = state.pitchBendRangeCents ?? 200;
      let cv = clamp(CV_MAX_VOLTS / 2 + (data.value / 8192) * (cents / 1200), 0, CV_MAX_VOLTS);
      if (state.pitchBendQuantizeCents) cv = Math.round(cv * 1200) / 1200;
      propagate(id, "pitchBend", cv);
    }
}

function routeRawMidi(sourceId, portId, data, path = new Set(), delivered = new Set()) {
  const key = `${sourceId}:${portId}`;
  if (path.has(key)) return;
  path.add(key);
  for (const cable of connections) {
    if (cable.from.widgetId !== sourceId || cable.from.portId !== portId || cable.to.portId !== "midi") continue;
    const id = cable.to.widgetId;
    const type = document.getElementById(id)?.dataset.widgetType;
    if (type === "midi-filter") {
      if (delivered.has(id) || path.has(`${id}:midi`)) continue;
      const state = midiFilterStates.get(id);
      if (!state || !acceptsMidiFilter(state, data)) continue;
      delivered.add(id);
      routeMidiWidget(data, id, state);
      routeRawMidi(id, "midi", data, path, delivered);
    } else if (type === "midi-split") {
      const state = midiSplitStates.get(id);
      for (let z = 0; z < (state?.zoneCount || 0); z += 1) {
        const zone = state.zones[z];
        const noteEvent = ["note_on", "note_off", "polytouch"].includes(data.midiType);
        if (noteEvent && (data.note < zone.from || data.note > zone.to)) continue;
        const forwarded = noteEvent ? { ...data, note: clamp(data.note + zone.octave * 12, 0, 127) } : data;
        routeRawMidi(id, `zone${z + 1}Midi`, forwarded, path, delivered);
      }
    } else if (type === "soundfont-player") {
      audioEngine.handleSoundFontMidi(id, data);
    } else if (type === "mda-piano") {
      audioEngine.handleMdaPianoMidi(id, data);
    }
  }
  path.delete(key);
}

function routeMidiToWidgets(data) {
  if (data.type !== "midi") return;
  if (data.midiType === "note_on" && data.velocity === 0) data = { ...data, midiType: "note_off" };
  const delivered = new Set();
  midiInStates.forEach((state, id) => {
    if (state.channel !== null && data.channel !== state.channel) return;
    routeMidiWidget(data, id, state);
    routeRawMidi(id, "midi", data, new Set(), delivered);
  });
}


function applyFilterWidgetParams(id, params, curtis = false) {
  const state = filterStates.get(id);
  if (!state || !params) return;
  const allowedTypes = FILTER_TYPE_BUTTONS.map((entry) => entry.id);
  const legacy = CURTIS_LEGACY_FILTER_TYPES[params.filterType];
  const requestedType = params.bypass === true ? "none" : (legacy ? legacy.filterType : params.filterType);
  state.filterType = allowedTypes.includes(requestedType) ? requestedType : "lowpass";
  state.cutoff = params.cutoff ?? 5;
  state.cutoffMinHz = params.cutoffMinHz ?? 20;
  state.cutoffMaxHz = params.cutoffMaxHz ?? 20000;
  state.cutoffCurve = params.cutoffCurve === "linear" ? "linear" : "log";
  state.resonance = params.resonance ?? 5;
  state.resonanceMinQ = params.resonanceMinQ ?? 0.1;
  state.resonanceMaxQ = params.resonanceMaxQ ?? 20;
  state.resonanceCurve = params.resonanceCurve === "linear" ? "linear" : "log";
  state.coeffStride = clamp(Math.round(params.coeffStride ?? 4), 1, 16);
  const requestedSlope = legacy ? legacy.slope : Number(params.slope);
  state.slope = FILTER_SLOPE_BUTTONS.some((entry) => entry.id === requestedSlope)
    ? requestedSlope
    : (curtis ? 24 : 12);
  state.global = params.global === true;
  audioEngine.setParam(id, "filterType", state.filterType);
  audioEngine.setParam(id, "slope", state.slope);
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
  updateFilterVisual(id);
}

const graphicEqCvEngine = makeEffectCvEngine("graphic-eq");

const WIDGET_ENGINES = {
  annotation: {
    getParams: (id) => getAnnotationParams(id),
    applyParams: (id, params) => applyAnnotationParams(id, params),
  },
  debug: {
    getParams: () => ({}),
    applyParams: () => {},
  },
  oscilloscope: {
    getParams: (id) => {
      const state = oscilloscopeStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = oscilloscopeStates.get(id);
      if (!state || !params) return;
      state.width = params.width;
      state.height = params.height;
      state.triggerChannel = params.triggerChannel;
      state.triggerLevel = params.triggerLevel;
      state.timebase = params.timebase;
      applyOscilloscopeState(id);
    },
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
      state.pitchBendRangeCents = clamp(Number(state.pitchBendRangeCents) || 200, 1, 6000);
      state.pitchBendQuantizeCents = state.pitchBendQuantizeCents === true;
      resizeVoiceSlots(id, state.voices);
      updateMidiInControls(id);
    },
  },
  "midi-filter": {
    getParams: (id) => {
      const state = midiFilterStates.get(id);
      return state ? { channel: state.channel, note: state.note } : {};
    },
    applyParams: (id, params) => {
      const state = midiFilterStates.get(id);
      if (!state || !params) return;
      state.channel = midiFilterCriterion(params.channel, 15);
      state.note = midiFilterCriterion(params.note, 127);
      for (const parameter of ["channel", "note"]) {
        const select = document.getElementById(id)?.querySelector(`[data-role="midi-filter-${parameter}"]`);
        if (select) select.value = String(state[parameter]);
      }
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
      const serializeControl = (control, includeBinding = false) => ({
        ...(includeBinding ? { cc: control.cc, channel: control.channel } : {}),
        raw: control.raw,
        point1In: control.point1In,
        point1Out: control.point1Out,
        point2In: control.point2In,
        point2Out: control.point2Out,
        constraintMin: control.constraintMin,
        constraintMax: control.constraintMax,
        valueList: parseValueList(control.listText) || [],
      });
      const firstLayer = state.layers[0];
      return {
        knobCount: state.count,
        layerCount: state.layerCount,
        activeLayer: state.activeLayer,
        outputSide: state.outputSide,
        bindings: firstLayer.controls.slice(0, state.count).map(({ cc, channel }) => ({ cc, channel })),
        layers: state.layers.slice(0, state.layerCount).map((layer, layerIndex) => ({
          name: layer.name,
          labels: layer.labels.slice(0, state.count),
          knobs: layer.controls.slice(0, state.count).map((control) => serializeControl(control, layerIndex === 0)),
        })),
        // Layer 1 remains available for old firmware/browser builds.
        labels: firstLayer.labels.slice(0, state.count),
        knobs: firstLayer.controls.slice(0, state.count).map((control) => serializeControl(control, true)),
      };
    },
    applyParams: (id, params) => {
      const state = knobStates.get(id);
      if (!state || !params) return;
      const layerEntries = Array.isArray(params.layers) && params.layers.length ? params.layers : null;
      const newFormat = Array.isArray(params.knobs) ? params.knobs : null;
      const oldFormat = !newFormat && Array.isArray(params.controls) ? params.controls : null;
      const sourceList = layerEntries?.[0]?.knobs || newFormat || oldFormat;
      const count = clamp(
        Math.round(Number(params.knobCount ?? params.count ?? (sourceList ? sourceList.length : 1)) || 1),
        1,
        KNOB_MAX_COUNT
      );
      state.count = count;
      state.layerCount = clamp(layerEntries?.length ?? params.layerCount ?? 1, 1, KNOB_MAX_LAYERS);
      state.activeLayer = clamp(Math.round(Number(params.activeLayer) || 0), 0, state.layerCount - 1);
      state.outputSide = ["top", "bottom"].includes(params.outputSide) ? params.outputSide : "bottom";
      state.layers = Array.from({ length: state.layerCount }, () => createKnobLayer());
      const applyControls = (layer, controls, labels) => {
        if (Array.isArray(labels)) {
          labels.forEach((label, index) => {
            if (index < KNOB_MAX_COUNT) layer.labels[index] = typeof label === "string" ? label : "";
          });
        }
        if (!Array.isArray(controls)) return;
        controls.forEach((raw, index) => {
          if (index >= KNOB_MAX_COUNT || !raw) return;
          const control = layer.controls[index];
          control.cc = typeof raw.cc === "number" ? raw.cc : null;
          control.channel = typeof raw.channel === "number" ? raw.channel : null;
          control.raw = typeof raw.raw === "number" ? raw.raw : 0;
          control.point1In = 0; // A In/B In/Min/Max were removed - fixed full knob travel
          control.point1Out = typeof raw.point1Out === "number" ? raw.point1Out : 0;
          control.point2In = 127;
          control.point2Out = typeof raw.point2Out === "number" ? raw.point2Out : CV_MAX_VOLTS;
          control.constraintMin = null;
          control.constraintMax = null;
          control.listText = Array.isArray(raw.valueList) && raw.valueList.length
            ? raw.valueList.join(", ")
            : (typeof raw.listText === "string" ? raw.listText : "");
        });
      };
      if (layerEntries) {
        state.layers.forEach((layer, layerIndex) => {
          const entry = layerEntries[layerIndex] || {};
          layer.name = typeof entry.name === "string" ? entry.name.slice(0, 32) : "";
          applyControls(layer, entry.knobs, entry.labels);
        });
      } else if (sourceList) {
        applyControls(state.layers[0], sourceList, params.labels);
      } else {
        const control = state.layers[0].controls[0];
        if (typeof params.cc === "number") control.cc = params.cc;
        if (typeof params.channel === "number") control.channel = params.channel;
        if (typeof params.raw === "number") control.raw = params.raw;

        if (typeof params.point1Out === "number") control.point1Out = params.point1Out;

        if (typeof params.point2Out === "number") control.point2Out = params.point2Out;

        if (Array.isArray(params.valueList)) control.listText = params.valueList.join(", ");
      }
      const bindings = Array.isArray(params.bindings) ? params.bindings : state.layers[0].controls;
      state.layers.forEach((layer) => {
        for (let knobIndex = 0; knobIndex < state.count; knobIndex += 1) {
          const binding = bindings[knobIndex] || state.layers[0].controls[knobIndex];
          layer.controls[knobIndex].cc = typeof binding?.cc === "number" ? binding.cc : null;
          layer.controls[knobIndex].channel = typeof binding?.channel === "number" ? binding.channel : null;
        }
      });
      renderKnobControls(id);
      renderKnobLayerSelector(id);
      const portsEl = document.getElementById(id)?.querySelector(".widget-ports");
      if (portsEl) renderKnobPorts(id, portsEl);
      state.layers.forEach((layer, layerIndex) => {
        for (let knobIndex = 0; knobIndex < count; knobIndex += 1) {
          const mapped = roundFloat(mapKnobValue(layer.controls[knobIndex]));
          audioEngine.setParam(id, knobValueParamName(knobIndex, layerIndex), mapped);
        }
      });
      for (let knobIndex = 0; knobIndex < count; knobIndex += 1) updateKnobVisual(id, knobIndex);
    },
  },
  button: {
    getParams: (id) => {
      const state = buttonStates.get(id);
      if (!state) return {};
      const firstLayer = state.layers[0];
      return {
        buttonCount: state.count,
        layerCount: state.layerCount,
        activeLayer: state.activeLayer,
        outputSide: state.outputSide,
        bindings: firstLayer.controls.slice(0, state.count).map(({ cc, channel }) => ({ cc, channel })),
        layers: state.layers.slice(0, state.layerCount).map((layer) => ({
          name: layer.name,
          labels: layer.labels.slice(0, state.count),
          buttons: layer.controls.slice(0, state.count).map((control) => ({
            value0: control.value0,
            value127: control.value127,
            valueList: parseValueList(control.listText) || [],
            listIndex: control.listIndex,
            wasAbove: control.wasAbove,
          })),
        })),
      };
    },
    applyParams: (id, params) => {
      const state = buttonStates.get(id);
      if (!state || !params) return;
      const entries = Array.isArray(params.layers) ? params.layers.slice(0, BUTTON_MAX_LAYERS) : [];
      state.layerCount = clamp(entries.length || Number(params.layerCount) || 1, 1, BUTTON_MAX_LAYERS);
      const firstButtons = Array.isArray(entries[0]?.buttons) ? entries[0].buttons : [];
      state.count = clamp(Number(params.buttonCount) || firstButtons.length || 1, 1, BUTTON_MAX_COUNT);
      state.activeLayer = clamp(Math.round(Number(params.activeLayer) || 0), 0, state.layerCount - 1);
      state.outputSide = ["top", "bottom"].includes(params.outputSide) ? params.outputSide : "bottom";
      state.layers = Array.from({ length: state.layerCount }, () => createButtonLayer());
      state.layers.forEach((layer, layerIndex) => {
        const entry = entries[layerIndex] || {};
        layer.name = typeof entry.name === "string" ? entry.name.slice(0, 32) : "";
        if (Array.isArray(entry.labels)) entry.labels.slice(0, BUTTON_MAX_COUNT).forEach((label, index) => { layer.labels[index] = typeof label === "string" ? label : ""; });
        if (Array.isArray(entry.buttons)) entry.buttons.slice(0, BUTTON_MAX_COUNT).forEach((raw, index) => {
          const control = layer.controls[index];
          control.value0 = typeof raw?.value0 === "number" ? raw.value0 : 0;
          control.value127 = typeof raw?.value127 === "number" ? raw.value127 : CV_MAX_VOLTS;
          control.listText = Array.isArray(raw?.valueList) ? raw.valueList.join(", ") : "";
          control.listIndex = clamp(Math.round(Number(raw?.listIndex) || 0), 0, Math.max(0, (raw?.valueList?.length || 1) - 1));
          control.wasAbove = raw?.wasAbove === true;
        });
      });
      const bindings = Array.isArray(params.bindings) ? params.bindings : [];
      state.layers.forEach((layer) => {
        for (let index = 0; index < state.count; index += 1) {
          layer.controls[index].cc = typeof bindings[index]?.cc === "number" ? bindings[index].cc : null;
          layer.controls[index].channel = typeof bindings[index]?.channel === "number" ? bindings[index].channel : null;
        }
      });
      renderButtonControls(id);
      renderButtonLayerSelector(id);
      const portsEl = document.getElementById(id)?.querySelector(".widget-ports");
      if (portsEl) renderButtonPorts(id, portsEl);
      state.layers.forEach((layer, layerIndex) => {
        for (let buttonIndex = 0; buttonIndex < state.count; buttonIndex += 1) {
          audioEngine.setParam(id, buttonValueParamName(buttonIndex, layerIndex), buttonCurrentOutput(layer.controls[buttonIndex]));
        }
      });
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
  "output-switch": {
    getParams: (id) => {
      const state = outputSwitchStates.get(id);
      return state ? { cc: state.cc, channel: state.channel, selectOut2: state.selectOut2 } : {};
    },
    applyParams: (id, params) => {
      const state = outputSwitchStates.get(id);
      if (state && params) {
        state.cc = params.cc ?? null;
        state.channel = params.channel ?? null;
        state.selectOut2 = params.selectOut2 === true;
        audioEngine.setParam(id, "selectOut2", state.selectOut2);
        updateOutputSwitchVisual(id);
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
  "logic-and": {
    getParams: (id) => {
      const state = logicGateStates.get(id);
      return state ? { inputCount: state.inputCount, values: [...state.values] } : {};
    },
    applyParams: (id, params) => {
      const state = logicGateStates.get(id);
      if (!state || !params) return;
      state.inputCount = clamp(params.inputCount || 2, LOGIC_GATE_MIN_INPUTS, LOGIC_GATE_MAX_INPUTS);
      if (Array.isArray(params.values)) state.values = params.values.slice();
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderLogicGatePorts(id, portsEl);
    },
  },
  "logic-or": {
    getParams: (id) => {
      const state = logicGateStates.get(id);
      return state ? { inputCount: state.inputCount, values: [...state.values] } : {};
    },
    applyParams: (id, params) => {
      const state = logicGateStates.get(id);
      if (!state || !params) return;
      state.inputCount = clamp(params.inputCount || 2, LOGIC_GATE_MIN_INPUTS, LOGIC_GATE_MAX_INPUTS);
      if (Array.isArray(params.values)) state.values = params.values.slice();
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderLogicGatePorts(id, portsEl);
    },
  },
  "logic-xor": {
    getParams: (id) => {
      const state = logicGateStates.get(id);
      return state ? { values: [...state.values] } : {};
    },
    applyParams: (id, params) => {
      const state = logicGateStates.get(id);
      if (!state || !params) return;
      if (Array.isArray(params.values)) state.values = params.values.slice();
      recomputeLogicGate(id);
    },
  },
  "logic-not": {
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
      return state ? { pulseMs: state.pulseMs } : {};
    },
    applyParams: (id, params) => {
      const state = clockStates.get(id);
      if (!state || !params) return;
      state.pulseMs = params.pulseMs ?? 100;
      restartClockTimer(id);
    },
  },
  "cv-in": {
    getParams: (id) => {
      const state = cvInStates.get(id);
      return state ? { port: state.port, address: state.address, channel: state.channel } : {};
    },
    applyParams: (id, params) => {
      const state = cvInStates.get(id);
      if (!state || !params) return;
      state.port = typeof params.port === "number" ? params.port : -1;
      state.address = typeof params.address === "number" ? params.address : 0;
      state.channel = params.channel === 1 ? 1 : 0;
      updateCvIoLabel(id);
    },
  },
  "cv-out": {
    getParams: (id) => {
      const state = cvOutStates.get(id);
      return state ? { port: state.port, address: state.address, channel: state.channel } : {};
    },
    applyParams: (id, params) => {
      const state = cvOutStates.get(id);
      if (!state || !params) return;
      state.port = typeof params.port === "number" ? params.port : -1;
      state.address = typeof params.address === "number" ? params.address : 0;
      state.channel = params.channel === 1 ? 1 : 0;
      updateCvIoLabel(id);
    },
  },
  "digital-in": {
    getParams: (id) => {
      const state = digitalInStates.get(id);
      return state ? { pin: state.pin } : {};
    },
    applyParams: (id, params) => {
      const state = digitalInStates.get(id);
      if (!state || !params) return;
      state.pin = typeof params.pin === "number" ? params.pin : -1;
      updateGpioIoLabel(id);
    },
  },
  "rgb-led": {
    getParams: (id) => {
      const state = rgbLedStates.get(id);
      return state ? { ledIndex: state.ledIndex, stops: state.stops.map((stop) => ({ ...stop })) } : {};
    },
    applyParams: (id, params) => {
      const state = rgbLedStates.get(id);
      if (!state || !params) return;
      state.ledIndex = Number.isInteger(params.ledIndex) && params.ledIndex >= 0 ? params.ledIndex : 0;
      state.stops = Array.isArray(params.stops) && params.stops.length > 0
        ? normalizeRgbLedStops(params.stops)
        : defaultRgbLedStops();
      updateRgbLedDisplay(id);
    },
  },
  "digital-out": {
    getParams: (id) => {
      const state = digitalOutStates.get(id);
      return state ? { pin: state.pin } : {};
    },
    applyParams: (id, params) => {
      const state = digitalOutStates.get(id);
      if (!state || !params) return;
      state.pin = typeof params.pin === "number" ? params.pin : -1;
      updateGpioIoLabel(id);
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
            gain: state.gain,
            phase: state.phase,
            zeroPointFixed: state.zeroPointFixed,
            zeroPoint: state.zeroPoint,
            oneShot: state.oneShot,
            global: state.global,
          }
        : {};
    },
    applyParams: (id, params) => {
      const state = lfoStates.get(id);
      if (!state || !params) return;
      state.waveType = params.waveType === "supersaw" ? "sine" : (params.waveType || "sine");
      state.maxRate = clamp(params.maxRate ?? LFO_MAX_RATE_HZ, 0, LFO_MAX_RATE_HZ);
      state.rate = clamp(params.rate ?? 2, 0, state.maxRate);
      state.gain = clamp(Number(params.gain ?? CV_MAX_VOLTS), 0, CV_MAX_VOLTS);
      state.phase = clamp(params.phase ?? 0, -180, 180);
      state.zeroPointFixed = params.zeroPointFixed === true;
      state.zeroPoint = params.zeroPoint ?? 5;
      state.oneShot = params.oneShot === true;
      state.global = params.global === true;
      
      
      state.oneShotDone = true;
      state.oneShotAccum = 0;
      audioEngine.setParam(id, "waveType", state.waveType);
      audioEngine.setParam(id, "maxRate", state.maxRate);
      audioEngine.setParam(id, "rate", state.rate);
      audioEngine.setParam(id, "gain", state.gain);
      audioEngine.setParam(id, "phase", state.phase);
      audioEngine.setParam(id, "zeroPointFixed", state.zeroPointFixed);
      audioEngine.setParam(id, "zeroPoint", state.zeroPoint);
      audioEngine.setParam(id, "oneShot", state.oneShot);
      audioEngine.setParam(id, "global", state.global);
      updateLfoWaveButtons(id);
      updateLfoVisual(id);
    },
  },
  wavetable: {
    getParams: (id) => {
      const state = lfoStates.get(id);
      return state
        ? {
            rate: state.rate,
            maxRate: state.maxRate,
            gain: state.gain,
            morph: state.morph,
            oneShot: state.oneShot,
            global: state.global,
            wavetables: encodeBank(lfoWavetableBank(state)),
          }
        : {};
    },
    applyParams: (id, params) => {
      const state = lfoStates.get(id);
      if (!state || !params) return;
      state.maxRate = clamp(params.maxRate ?? LFO_MAX_RATE_HZ, 0, LFO_MAX_RATE_HZ);
      state.rate = clamp(params.rate ?? 2, 0, state.maxRate);
      state.gain = clamp(Number(params.gain ?? CV_MAX_VOLTS), 0, CV_MAX_VOLTS);
      state.morph = clamp(Number(params.morph ?? 0) || 0, 0, CV_MAX_VOLTS);
      state.oneShot = params.oneShot === true;
      state.global = params.global === true;
      state.wavetables = decodeBank(params.wavetables) || createDefaultBank();
      state.oneShotDone = true;
      state.oneShotAccum = 0;
      audioEngine.setParam(id, "maxRate", state.maxRate);
      audioEngine.setParam(id, "rate", state.rate);
      audioEngine.setParam(id, "gain", state.gain);
      audioEngine.setParam(id, "wavetables", state.wavetables);
      audioEngine.setParam(id, "morph", state.morph);
      audioEngine.setParam(id, "oneShot", state.oneShot);
      audioEngine.setParam(id, "global", state.global);
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
        feedback: state.feedback,
      };
    },
    applyParams: (id, params) => {
      const state = oscillatorStates.get(id);
      if (!state || !params) return;
      const center = CV_MAX_VOLTS / 2;
      state.waveType = params.waveType === "supersaw" ? "sine" : (params.waveType || "sine");
      state.amplitude = params.amplitude ?? 1;
      state.tuneVolts = params.tune ?? center;
      state.gain = params.gain ?? CV_MAX_VOLTS;
      state.feedback = params.feedback ?? 0;
      state.pitchCv = center;
      audioEngine.setParam(id, "waveType", state.waveType);
      audioEngine.setParam(id, "amplitude", 0, 0);
      audioEngine.setParam(id, "gain", state.gain);
      audioEngine.setParam(id, "feedback", state.feedback);
      recomputeOscillatorFreq(id, undefined);
      updateOscillatorTuneVisual(id);
      updateOscillatorGainVisual(id);
      updateOscillatorFeedbackVisual(id);
      updateOscillatorWaveButtons(id);
    },
  },
  "fm-oscillator": {
    getParams: (id) => {
      const state = fmOscillatorStates.get(id);
      if (!state) return {};
      return {
        waveType: state.waveType,
        amplitude: state.amplitude,
        tune: state.tuneVolts,
        gain: state.gain,
        fmIndex: state.fmIndex,
        feedback: state.feedback,
        dutyCycle: state.dutyCycle,
        superSawDetune: state.superSawDetune,
        superSawMix: state.superSawMix,
        morph: state.morph,
        // The bank only travels with the preset while the wavetable waveform is selected.
        ...(state.waveType === "wavetable" ? { wavetables: encodeBank(fmOscillatorWavetableBank(state)) } : {}),
      };
    },
    applyParams: (id, params) => {
      const state = fmOscillatorStates.get(id);
      if (!state || !params) return;
      const center = CV_MAX_VOLTS / 2;
      state.waveType = FM_WAVE_TYPE_BUTTONS.some((wave) => wave.id === params.waveType) ? params.waveType : "sine";
      state.amplitude = params.amplitude ?? 1;
      state.tuneVolts = params.tune ?? center;
      state.gain = params.gain ?? CV_MAX_VOLTS;
      state.fmIndex = params.fmIndex ?? 0;
      state.feedback = params.feedback ?? 0;
      state.dutyCycle = params.dutyCycle ?? center;
      state.superSawDetune = params.superSawDetune ?? center;
      state.superSawMix = params.superSawMix ?? center;
      state.morph = clamp(Number(params.morph) || 0, 0, CV_MAX_VOLTS);
      state.wavetables = decodeBank(params.wavetables) || state.wavetables;
      state.pitchCv = center;
      audioEngine.setParam(id, "waveType", state.waveType);
      audioEngine.setParam(id, "amplitude", 0, 0);
      audioEngine.setParam(id, "gain", state.gain);
      audioEngine.setParam(id, "fmIndex", state.fmIndex);
      audioEngine.setParam(id, "feedback", state.feedback);
      audioEngine.setParam(id, "dutyCycle", state.dutyCycle);
      audioEngine.setParam(id, "superSawDetune", state.superSawDetune);
      audioEngine.setParam(id, "superSawMix", state.superSawMix);
      audioEngine.setParam(id, "morph", state.morph);
      if (state.wavetables) audioEngine.setParam(id, "wavetables", state.wavetables);
      recomputeFmOscillatorFreq(id, undefined);
      updateFmOscillatorTuneVisual(id);
      updateFmOscillatorGainVisual(id);
      updateFmOscillatorIndexVisual(id);
      updateFmOscillatorFeedbackVisual(id);
      updateFmOscillatorDutyCycleVisual(id);
      updateFmOscillatorSuperSawVisual(id);
      updateFmOscillatorWavetableVisual(id);
      updateFmOscillatorWaveButtons(id);
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
      return state ? { sampleFile: state.sampleFile, rootNoteMidi: state.rootNoteMidi, gain: state.gain } : {};
    },
    applyParams: (id, params) => {
      const state = wavePlayerStates.get(id);
      if (!state || !params) return;
      state.sampleFile = typeof params.sampleFile === "string" ? params.sampleFile : "";
      state.rootNoteMidi = params.rootNoteMidi ?? 60;
      state.gain = clamp(Number(params.gain ?? CV_MAX_VOLTS), 0, CV_MAX_VOLTS);
      audioEngine.setParam(id, "sampleFile", state.sampleFile);
      audioEngine.setParam(id, "rootNoteMidi", state.rootNoteMidi);
      audioEngine.setParam(id, "gain", state.gain);
      updateWavePlayerLabel(id);
      updateWavePlayerGainVisual(id);
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
  "mda-piano": {
    getParams: (id) => {
      const state = mdaPianoStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = mdaPianoStates.get(id);
      if (!state || !params) return;
      state.maxVoices = clamp(Math.round(Number(params.maxVoices) || 32), 4, 128);
      state.enableModWheel = params.enableModWheel === true;
      state.enablePitchWheel = params.enablePitchWheel === true;
      audioEngine.setParam(id, "enableModWheel", state.enableModWheel);
      audioEngine.setParam(id, "enablePitchWheel", state.enablePitchWheel);
      MDA_PIANO_CV_PARAMS.forEach((portId) => {
        const fallback = state[portId];
        const value = Number(params[portId]);
        state[portId] = clamp(Number.isNaN(value) ? fallback : value, 0, CV_MAX_VOLTS);
        audioEngine.setParam(id, portId, state[portId]);
      });
      updateMdaPianoControls(id);
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
    getParams: () => ({ labels: [...getAudioLinkLabels()] }),
    applyParams: (id, params) => {
      if (Array.isArray(params?.labels)) setAudioLinkLabels(params.labels);
      updateAudioOutputLinkVisual(id);
    },
  },
  "audio-input-link": {
    getParams: (id) => {
      const state = audioInputLinkStates.get(id);
      return state ? { gains: [...state.gains], labels: [...getAudioLinkLabels(id)] } : {};
    },
    applyParams: (id, params) => {
      const state = audioInputLinkStates.get(id);
      if (!state) return;
      if (Array.isArray(params?.labels)) setAudioLinkLabels(params.labels);
      if (Array.isArray(params?.gains)) {
        for (let channel = 0; channel < LINK_BUS_COUNT; channel += 1) {
          state.gains[channel] = clamp(Number(params.gains[channel] ?? CV_MAX_VOLTS), 0, CV_MAX_VOLTS);
          audioEngine.setParam(id, `gain${channel + 1}`, state.gains[channel]);
        }
      }
      updateAudioInputLinkVisual(id);
    },
  },
  "linked-input": {
    getParams: (id) => ({ ...linkedInputStates.get(id) }),
    applyParams: (id, params) => {
      const state = linkedInputStates.get(id);
      if (!state) return;
      if (typeof params?.linkId === "string" && params.linkId) state.linkId = params.linkId;
      if (typeof params?.linkName === "string") state.linkName = params.linkName;
      if (Array.isArray(params?.labels)) state.labels = Array.from({ length: state.linkCount }, (_, index) => typeof params.labels[index] === "string" ? params.labels[index] : "");
      if (["top", "bottom", "left", "right"].includes(params?.portSide)) state.portSide = params.portSide;
      audioEngine.setParam(id, "linkId", state.linkId);
      audioEngine.setParam(id, "linkName", state.linkName);
      updateLinkedInputVisual(id);
    },
  },
  "linked-output": {
    getParams: (id) => ({ ...linkedOutputStates.get(id) }),
    applyParams: (id, params) => {
      const state = linkedOutputStates.get(id);
      if (!state) return;
      state.sourceLinkId = typeof params?.sourceLinkId === "string" ? params.sourceLinkId : "";
      if (["top", "bottom", "left", "right"].includes(params?.portSide)) state.portSide = params.portSide;
      audioEngine.setParam(id, "sourceLinkId", state.sourceLinkId);
      updateLinkedOutputVisual(id);
    },
  },
  detune: {
    getParams: (id) => {
      const state = detuneStates.get(id);
      return state
        ? { amount: state.amount, maxOffsetCents: state.maxOffsetCents, stepCents: detuneStepCents(state) }
        : {};
    },
    applyParams: (id, params) => {
      const state = detuneStates.get(id);
      if (!state || !params) return;
      const amount = Number(params.amount);
      state.amount = clamp(Number.isFinite(amount) ? amount : CV_MAX_VOLTS / 2, 0, CV_MAX_VOLTS);
      const maxOffsetCents = Number(params.maxOffsetCents);
      state.maxOffsetCents = Number.isFinite(maxOffsetCents) && maxOffsetCents >= 0 ? maxOffsetCents : 1200;
      state.unit = Number(params.stepCents) <= 1 ? "cent" : "semitone";
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
      // Legacy presets stored "curve" as "linear"/"exponential" (shaping the
      // single amount stage). The new "curve" is a separate 2nd VCA stage
      // (5V = unity, see cv::curveGain) with no equivalent old value, so any
      // non-numeric/missing curve falls back to unity (no change in level).
      state.curve = typeof params.curve === "number" ? clamp(params.curve, 0, CV_MAX_VOLTS) : CV_MAX_VOLTS / 2;
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
  "comb-filter": {
    getParams: (id) => ({ ...combFilterStates.get(id) }),
    applyParams: (id, params) => {
      const state = combFilterStates.get(id);
      if (!state || !params) return;
      COMB_FILTER_PORTS.forEach((port) => {
        state[port] = combFilterCv(params[port], COMB_FILTER_DEFAULTS[port]);
        audioEngine.setParam(id, port, state[port]);
      });
      updateCombFilterVisual(id);
    },
  },
  "graphic-eq": {
    getParams: (id) => ({
      ...graphicEqCvEngine.getParams(id),
      frequencies: [...(graphicEqStates.get(id)?.frequencies || GRAPHIC_EQ_DEFAULT_FREQS)],
    }),
    applyParams: (id, params) => {
      graphicEqCvEngine.applyParams(id, params);
      const state = graphicEqStates.get(id);
      if (!state || !params) return;
      const freqs = validGraphicEqFrequencies(params.frequencies)
        ? params.frequencies.map(Number)
        : [...state.frequencies];
      state.frequencies = freqs;
      state.outputGain = typeof params.gain === "number" ? clamp(params.gain, 0, CV_MAX_VOLTS) : CV_MAX_VOLTS;
      freqs.forEach((hz, i) => audioEngine.setParam(id, `freq${i + 1}`, hz));
      audioEngine.setParam(id, "gain", state.outputGain);
      updateGraphicEqVisual(id);
    },
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
  "output-multiplexer": {
    getParams: (id) => {
      const state = outputMultiplexerStates.get(id);
      return state ? { outputCount: state.outputCount, select: state.manualSelect } : {};
    },
    applyParams: (id, params) => {
      const state = outputMultiplexerStates.get(id);
      if (!state || !params) return;
      state.outputCount = clamp(params.outputCount || 2, OUTPUT_MULTIPLEXER_MIN_OUTPUTS, OUTPUT_MULTIPLEXER_MAX_OUTPUTS);
      state.manualSelect = clamp(parseInt(params.select, 10) || 0, 0, state.outputCount - 1);
      audioEngine.setParam(id, "select", state.manualSelect);
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderOutputMultiplexerPorts(id, portsEl);
      outputMultiplexerSetActiveIndex(id, state.manualSelect);
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
      state.graphCollapsed = params.graphCollapsed === true;
      updateAdsrSliders(id);
      applyAdsrCollapsed(id);
    },
  },
  "dx-envelope": {
    getParams: (id) => {
      const state = dxEnvelopeStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = dxEnvelopeStates.get(id);
      if (!state || !params) return;
      for (let i = 1; i <= 4; i += 1) {
        const level = Number(params[`level${i}`] ?? state[`level${i}`]);
        const maxSeconds = Number(params[`time${i}MaxSeconds`] ?? MAX_ENVELOPE_SECONDS);
        state[`level${i}`] = clamp(Number.isFinite(level) ? level : 0, 0, CV_MAX_VOLTS);
        state[`time${i}MaxSeconds`] = Number.isFinite(maxSeconds) && maxSeconds > 0
          ? maxSeconds
          : MAX_ENVELOPE_SECONDS;
        state[`time${i}Curve`] = params[`time${i}Curve`] === "log" ? "log" : "linear";
        const time = Number(params[`time${i}`] ?? state[`time${i}`]);
        state[`time${i}`] = clamp(
          Number.isFinite(time) ? time : 0.001,
          0.001,
          state[`time${i}MaxSeconds`]
        );
        audioEngine.setParam(id, `level${i}`, state[`level${i}`]);
        audioEngine.setParam(id, `time${i}`, state[`time${i}`]);
      }
      state.graphCollapsed = params.graphCollapsed === true;
      updateDxEnvelopeVisuals(id);
      applyDxEnvelopeCollapsed(id);
    },
  },
  "step-arpeggiator": {
    getParams: (id) => {
      const state = stepArpStates.get(id);
      return state
        ? {
            steps: [...state.steps],
            stepCount: state.stepCount,
            pulseMs: state.pulseMs,
            pulseTimeMinSec: state.pulseTimeMinSec,
            pulseTimeMaxSec: state.pulseTimeMaxSec,
            maxSemitoneOffset: state.maxSemitoneOffset,
            direction: state.directionVolts,
            retriggerGateOut: state.retriggerGateOut === true,
            clockCvVolts: state.clockCvVolts,
            clockMaxBpm: state.clockMaxBpm,
            clockRoundBpm: state.clockRoundBpm === true,
          }
        : {};
    },
    applyParams: (id, params) => {
      const state = stepArpStates.get(id);
      if (!state || !params) return;
      state.steps =
        Array.isArray(params.steps) && params.steps.length === STEP_ARP_MAX_STEPS
          ? params.steps.map((v) => clamp(Number(v) || 0, 0, CV_MAX_VOLTS))
          : new Array(STEP_ARP_MAX_STEPS).fill(STEP_ARP_DEFAULT_VOLTS);
      state.stepCount = Number(params.stepCount) <= 8 ? 8 : STEP_ARP_MAX_STEPS;
      const pulseMs = Number(params.pulseMs);
      state.pulseMs = Number.isFinite(pulseMs) && pulseMs > 0 ? pulseMs : 10;
      const minSec = Number(params.pulseTimeMinSec);
      const maxSec = Number(params.pulseTimeMaxSec);
      state.pulseTimeMinSec = Number.isFinite(minSec) && minSec >= 0 ? minSec : 0.001;
      state.pulseTimeMaxSec = Number.isFinite(maxSec) && maxSec > state.pulseTimeMinSec ? maxSec : Math.max(2, state.pulseTimeMinSec + 1);
      const maxSemitoneOffset = Number(params.maxSemitoneOffset);
      state.maxSemitoneOffset = Number.isFinite(maxSemitoneOffset) ? clamp(maxSemitoneOffset, 0, 60) : 60;
      state.directionVolts = clamp(Number(params.direction) || 0, 0, CV_MAX_VOLTS);
      state.retriggerGateOut = params.retriggerGateOut === true;
      const cvVolts = Number(params.clockCvVolts);
      state.clockCvVolts = clamp(Number.isFinite(cvVolts) ? cvVolts : 0, 0, CV_MAX_VOLTS);
      const maxBpm = Number(params.clockMaxBpm);
      state.clockMaxBpm = Number.isFinite(maxBpm) && maxBpm > 0 ? maxBpm : 120;
      state.clockRoundBpm = params.clockRoundBpm === true;
      const el = document.getElementById(id);
      const portsEl = el ? el.querySelector(".widget-ports") : null;
      if (portsEl) renderStepArpPorts(id, portsEl);
      updateStepArpVisual(id);
      stepArpRestartInternalClock(id);
    },
  },
  "drum-sequencer": {
    getParams: (id) => {
      const state = drumSequencerStates.get(id);
      return state
        ? {
            stepCount: state.stepCount,
            trackCount: state.trackCount,
            patterns: state.patterns.map((p) => ({
              cells: p.active.map((active, index) => (active ? p.velocities[index] : -(p.velocities[index] + 1))),
            })),
            activePatternIndex: state.activePatternIndex,
            switchAtEnd: state.switchAtEnd === true,
            trackPulseMs: [...state.trackPulseMs],
            clockCvVolts: state.clockCvVolts,
            clockMaxBpm: state.clockMaxBpm,
            clockRoundBpm: state.clockRoundBpm === true,
          }
        : {};
    },
    applyParams: (id, params) => {
      const state = drumSequencerStates.get(id);
      if (!state || !params) return;
      const requestedSteps = Number(params.stepCount ?? params.barCount);
      const stepCount = DRUM_STEP_COUNTS.includes(requestedSteps) ? requestedSteps : DRUM_DEFAULT_STEPS;
      const trackCount = clamp(parseInt(params.trackCount, 10) || 1, 1, DRUM_MAX_TRACKS);
      const cellCount = stepCount * trackCount;

      // Backward compatibility: a preset saved before multi-pattern support
      // only has a top-level "cells" array - load it as the single pattern.
      let patternsIn = Array.isArray(params.patterns) ? params.patterns : null;
      if (!patternsIn && Array.isArray(params.cells)) patternsIn = [{ cells: params.cells }];
      if (!patternsIn || patternsIn.length === 0) patternsIn = [{ cells: [] }];

      state.patterns = patternsIn.slice(0, DRUM_MAX_PATTERNS).map((entry) => {
        const active = new Array(cellCount).fill(false);
        const velocities = new Array(cellCount).fill(CV_MAX_VOLTS);
        const cells = Array.isArray(entry?.cells) ? entry.cells : [];
        const count = Math.min(cells.length, cellCount);
        for (let index = 0; index < count; index += 1) {
          const encoded = Number(cells[index]);
          if (!Number.isFinite(encoded)) continue;
          active[index] = encoded >= 0;
          velocities[index] = clamp(encoded >= 0 ? encoded : -encoded - 1, 0, CV_MAX_VOLTS);
        }
        return { active, velocities };
      });
      state.stepCount = stepCount;
      state.trackCount = trackCount;
      state.activePatternIndex = clamp(parseInt(params.activePatternIndex, 10) || 0, 0, state.patterns.length - 1);
      state.pendingPatternIndex = -1;
      state.switchAtEnd = params.switchAtEnd === true;

      const legacyPulseMs = drumSequencerPulseMs(params.pulseMs);
      const trackPulseMs = Array.isArray(params.trackPulseMs) ? params.trackPulseMs : [];
      state.trackPulseMs = Array.from({ length: DRUM_MAX_TRACKS }, (_, track) => drumSequencerPulseMs(trackPulseMs[track], legacyPulseMs));
      state.clockCvVolts = clamp(Number(params.clockCvVolts) || 0, 0, CV_MAX_VOLTS);
      const maxBpm = Number(params.clockMaxBpm);
      state.clockMaxBpm = Number.isFinite(maxBpm) && maxBpm > 0 ? clamp(maxBpm, 0.1, 10000) : 120;
      state.clockRoundBpm = params.clockRoundBpm === true;
      state.currentStep = 0;
      state.lastStep = -1;
      const portsEl = document.getElementById(id)?.querySelector(".widget-ports");
      if (portsEl) renderDrumSequencerPorts(id, portsEl);
      updateDrumSequencerVisual(id);
      drumSequencerRestartInternalClock(id);
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
  bitcrusher: {
    getParams: (id) => {
      const state = bitcrusherStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = bitcrusherStates.get(id);
      if (!state || !params) return;
      state.crush = Math.max(0, Math.min(10, Number(params.crush ?? 5)));
      state.bypass = params.bypass === true;
      audioEngine.setParam(id, "crush", state.crush);
      audioEngine.setParam(id, "bypass", state.bypass);
      updateBitcrusherVisual(id);
    },
  },
  "wave-folder": {
    getParams: (id) => {
      const state = waveFolderStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = waveFolderStates.get(id);
      if (!state || !params) return;
      state.drive = clamp(Number(params.drive ?? 0) || 0, 0, CV_MAX_VOLTS);
      state.symmetry = clamp(Number(params.symmetry ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      state.gain = clamp(Number(params.gain ?? CV_MAX_VOLTS) || 0, 0, CV_MAX_VOLTS);
      state.mix = clamp(Number(params.mix ?? CV_MAX_VOLTS) || 0, 0, CV_MAX_VOLTS);
      WAVE_FOLDER_SLIDER_PORTS.forEach((portId) => audioEngine.setParam(id, portId, state[portId]));
      updateWaveFolderVisual(id);
    },
  },
  "envelope-follower": {
    getParams: (id) => {
      const state = envelopeFollowerStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = envelopeFollowerStates.get(id);
      if (!state || !params) return;
      state.attack = clamp(Number(params.attack ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      state.release = clamp(Number(params.release ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      state.preGain = clamp(Number(params.preGain ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      const coeffStride = parseInt(params.coeffStride, 10);
      state.coeffStride = Number.isNaN(coeffStride) ? 4 : clamp(coeffStride, 1, 16);
      ENVELOPE_FOLLOWER_SLIDER_PORTS.forEach((portId) => audioEngine.setParam(id, portId, state[portId]));
      audioEngine.setParam(id, "coeffStride", state.coeffStride);
      updateEnvelopeFollowerVisual(id);
    },
  },
  pan: {
    getParams: (id) => {
      const state = panStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = panStates.get(id);
      if (!state || !params) return;
      state.pan = clamp(Number(params.pan ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      audioEngine.setParam(id, "pan", state.pan);
      updatePanVisual(id);
    },
  },
  "burst-generator": {
    getParams: (id) => {
      const state = burstGeneratorStates.get(id);
      return state ? { ...state } : {};
    },
    applyParams: (id, params) => {
      const state = burstGeneratorStates.get(id);
      if (!state || !params) return;
      state.mode = clamp(Number(params.mode ?? 0) || 0, 0, CV_MAX_VOLTS);
      state.count = clamp(Number(params.count ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      state.time = clamp(Number(params.time ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      state.decay = clamp(Number(params.decay ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      audioEngine.setParam(id, "mode", state.mode);
      BURST_GENERATOR_SLIDER_PORTS.forEach((portId) => audioEngine.setParam(id, portId, state[portId]));
      updateBurstGeneratorVisual(id);
    },
  },
  "sample-hold": {
    getParams: (id) => {
      const state = sampleHoldStates.get(id);
      return state ? { threshold: state.threshold } : {};
    },
    applyParams: (id, params) => {
      const state = sampleHoldStates.get(id);
      if (!state || !params) return;
      state.threshold = clamp(Number(params.threshold ?? CV_MAX_VOLTS / 2) || 0, 0, CV_MAX_VOLTS);
      audioEngine.setParam(id, "threshold", state.threshold);
      updateSampleHoldVisual(id);
    },
  },
  delay: makeEffectCvEngine("delay"),
  "reverb-light": makeEffectCvEngine("reverb-light"),
};

["linked-input-4", "linked-input-8"].forEach((type) => { WIDGET_ENGINES[type] = WIDGET_ENGINES["linked-input"]; });
["linked-output-4", "linked-output-8"].forEach((type) => { WIDGET_ENGINES[type] = WIDGET_ENGINES["linked-output"]; });

function getWidgetParams(type, id) {
  const engine = WIDGET_ENGINES[type];
  return engine ? engine.getParams(id) : {};
}

function applyWidgetParams(type, id, params) {
  const engine = WIDGET_ENGINES[type];
  if (engine) engine.applyParams(id, params);
}



const SCHEMA_VERSION = 4;
let restoringPreset = false;

function serializePreset() {
  // Firmware still receives one flat generator graph. uiPages/pageId only
  // restore the Web UI grouping and are ignored by the engine parser.
  captureToMasterPosition(currentPageName);

  const serializeWidget = (el, pageName) => {
    const labelSpan = el.querySelector(".widget-instance-label");
    const type = el.dataset.widgetType;
    const isToMaster = type === "audio-output-link";
    const position = isToMaster ? pages.generator.toMasterUi : {
      x: parseInt(el.style.left, 10) || 0,
      y: parseInt(el.style.top, 10) || 0,
    };
    return {
      id: el.id,
      type,
      label: labelSpan ? labelSpan.textContent : type,
      ui: { ...position, ...(pageName && !isToMaster ? { pageId: pageName } : {}) },
      params: getWidgetParams(type, el.id),
    };
  };

  const generatorWidgets = [];
  const generatorWidgetIds = new Set();
  generatorPageNames().forEach((name) => {
    pages[name].canvasContentEl.querySelectorAll(".widget-instance").forEach((el) => {
      if (generatorWidgetIds.has(el.id)) return;
      generatorWidgetIds.add(el.id);
      generatorWidgets.push(serializeWidget(el, name));
    });
  });

  const fxWidgets = [];
  const fxWidgetIds = new Set();
  pages.fx.canvasContentEl.querySelectorAll(".widget-instance").forEach((el) => {
    fxWidgetIds.add(el.id);
    fxWidgets.push(serializeWidget(el, "fx"));
  });

  return {
    schemaVersion: SCHEMA_VERSION,
    title: presetDescription.title,
    description: presetDescription.description,
    longDescription: presetDescription.longDescription,
    author: presetDescription.author,
    uiPages: generatorPageNames().map((name) => ({
      id: name,
      title: pages[name].title,
      toMasterUi: { ...pages[name].toMasterUi },
    })),
    pages: {
      generator: {
        widgets: generatorWidgets,
        connections: connections.filter((c) => generatorWidgetIds.has(c.from.widgetId)),
      },
      fx: {
        widgets: fxWidgets,
        connections: connections.filter((c) => fxWidgetIds.has(c.from.widgetId)),
      },
    },
  };
}


let currentPresetName = null;
let presetDescription = { title: "", description: "", longDescription: "", author: "" };

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
  // A cable-driven change (e.g. a CV into MIDI In's "voices") must not rewrite the preset on
  // the device over and over - see deliverToWidget().
  if (deviceSyncMuteDepth > 0) return;
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
  const generatorWidgetCount = generatorPageNames().reduce(
    (count, name) => count + pages[name].canvasContentEl.querySelectorAll('.widget-instance:not([data-widget-type="audio-output-link"])').length,
    0
  );
  const hasWidgets = generatorWidgetCount > 0 ||
    pages.fx.canvasContentEl.querySelectorAll(".widget-instance").length > 1;
  if (currentPresetName === null && hasWidgets) {
    if (!confirm("Discard unsaved preset?")) return;
  }
  
  
  
  currentPresetName = null;
  presetDescription = { title: "", description: "", longDescription: "", author: "" };
  loadInitTemplate();
  refreshPresetMenu();
}










// Generic "a parameter changed" signal used by every widget's own state-
// mutation code (knob/button/switch drags, port-value edits, annotation
// edits, canvas moves, ...) - deliberately a no-op, NOT an autosave trigger.
// savePresetNow() sends the whole preset to the firmware, which reparses it
// and rebuilds the audio graph from scratch (see GraphBuilder) - audible as
// a brief dropout. A parameter tweak must never cause that: it already
// reaches the RUNNING engine live via sendLiveParamUpdateThrottled()
// (WS "paramUpdate", patches the field on the live NodeInstance in place,
// no rebuild) wherever a call site needs that. Only two things persist to
// the saved preset file: an explicit Save Preset action, and a structural
// graph edit (add/remove widget, add/remove connection - see e.g.
// removeWidget's caller, the connection-drag completion handler, and
// connection deletion, all of which call savePresetNow() directly, not
// through here). Whenever one of those DOES fire, it serializes each
// widget's current live state anyway (see getWidgetParams/serializePreset),
// so no parameter change is ever actually lost - it just rides along with
// the next real save instead of forcing one of its own. A brief stub
// history: this used to be a genuine no-op (`{}`) by omission, not design;
// a later attempt turned it into a debounced savePresetNow() (matching
// scheduleAnnotationSave's pattern) to fix call sites that had NO other
// persistence path - that reintroduced exactly the audio-dropout-on-tweak
// problem this comment now documents, so it was reverted back to a no-op.
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
  if (deviceSyncMuteDepth > 0) return; // see deliverToWidget()
  wsSend({ type: "paramUpdate", widgetId, params });
}





const pendingLiveParams = new Map(); 
let liveParamFlushScheduled = false;
function sendLiveParamUpdateThrottled(widgetId, params) {
  if (deviceSyncMuteDepth > 0) return; // see deliverToWidget()
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

function restoreGeneratorPageStructure(uiPages) {
  resetAdditionalGeneratorPages();
  pages.generator.title = "Sound Generation";
  pages.generator.toMasterUi = { x: 40, y: 40 };
  const pageIdMap = new Map([["generator", "generator"]]);
  const entries = Array.isArray(uiPages) ? uiPages.slice(0, 64) : [];
  const base = entries.find((entry) => entry?.id === "generator");
  if (base) {
    pages.generator.title = String(base.title || "Sound Generation").trim().slice(0, 64) || "Sound Generation";
    if (Number.isFinite(base.toMasterUi?.x) && Number.isFinite(base.toMasterUi?.y)) {
      pages.generator.toMasterUi = { x: base.toMasterUi.x, y: base.toMasterUi.y };
    }
  }
  pageTab("generator").textContent = pages.generator.title;

  const restoredIds = new Set(["generator", "fx"]);
  entries.forEach((entry) => {
    if (!entry || typeof entry.id !== "string" || restoredIds.has(entry.id)) return;
    restoredIds.add(entry.id);
    const restoredId = createGeneratorPage({
      id: entry.id,
      title: entry.title,
      toMasterUi: entry.toMasterUi,
    });
    pageIdMap.set(entry.id, restoredId);
  });
  return pageIdMap;
}

function ensureSingletonLinkWidget(pageName) {
  const type = pageName === "generator" ? "audio-output-link" : "audio-input-link";
  const exists = type === "audio-output-link"
    ? sharedToMasterElement()
    : pages[pageName].canvasContentEl.querySelector(`[data-widget-type="${type}"]`);
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

  // Orthogonal cables may intentionally travel outside a widget's rectangle.
  // Include their actual SVG bounds, so Fit-to-View never clips a cable loop.
  connectionsLayer.querySelectorAll(".connection-path").forEach((path) => {
    try {
      const bounds = path.getBBox();
      if (bounds.width === 0 && bounds.height === 0) return;
      minX = Math.min(minX, bounds.x);
      minY = Math.min(minY, bounds.y);
      maxX = Math.max(maxX, bounds.x + bounds.width);
      maxY = Math.max(maxY, bounds.y + bounds.height);
    } catch {
      // A detached SVG path has no box. Widget bounds still produce a fit.
    }
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
  presetDescription = {
    title: typeof data.title === "string" ? data.title : "",
    description: typeof data.description === "string" ? data.description : "",
    longDescription: typeof data.longDescription === "string" ? data.longDescription : "",
    author: typeof data.author === "string" ? data.author : "",
  };
  const pageData = normalizePresetData(data);
  const hasUiPages = Array.isArray(data.uiPages);
  const pageIdMap = restoreGeneratorPageStructure(data.uiPages);
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

  
  const loadedGeneratorLinkTypes = new Set();
  pageData.generator.widgets.forEach((w) => {
    if (LINK_WIDGET_TYPES.includes(w.type)) {
      if (loadedGeneratorLinkTypes.has(w.type)) return;
      loadedGeneratorLinkTypes.add(w.type);
    }
    const requestedPage = typeof w.ui?.pageId === "string" ? w.ui.pageId : "generator";
    const targetPage = w.type === "audio-output-link"
      ? "generator"
      : (pageIdMap.get(requestedPage) || "generator");
    switchPage(targetPage);
    const label = w.label || (WIDGET_TYPES[w.type] || {}).label || w.type;
    const x = w.ui ? w.ui.x : w.x;
    const y = w.ui ? w.ui.y : w.y;
    createWidget(w.type, label, x, y, w.id);
    applyWidgetParams(w.type, w.id, widgetParamsFromLegacy(w));
    if (w.type === "audio-output-link" && !hasUiPages) {
      pages.generator.toMasterUi = { x: Number(x) || 0, y: Number(y) || 0 };
    }
  });
  switchPage("generator");
  ensureSingletonLinkWidget("generator");
  moveToMasterToPage("generator");

  switchPage("fx");
  const loadedFxLinkTypes = new Set();
  pageData.fx.widgets.forEach((w) => {
    if (LINK_WIDGET_TYPES.includes(w.type)) {
      if (loadedFxLinkTypes.has(w.type)) return;
      loadedFxLinkTypes.add(w.type);
    }
    const label = w.label || (WIDGET_TYPES[w.type] || {}).label || w.type;
    const x = w.ui ? w.ui.x : w.x;
    const y = w.ui ? w.ui.y : w.y;
    createWidget(w.type, label, x, y, w.id);
    applyWidgetParams(w.type, w.id, widgetParamsFromLegacy(w));
  });
  ensureSingletonLinkWidget("fx");

  
  
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

  linkedAudioRouteIds.clear();
  connections.forEach(connectIfAudio);
  refreshControlGraph();
  refreshVoiceGroups();
  refreshLinkedIO();

  // Same as a middle-button click on every page: fit all widgets and cables into view.
  // Done after the cables exist so their bounds count too.
  if (fitToView) {
    [...generatorPageNames(), "fx"].forEach((name) => {
      switchPage(name);
      fitPageToView();
    });
  }

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
    await applyPresetData(data, { fitToView: true });
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

const OSCILLOSCOPES_VISIBLE_KEY = "espressynth.oscilloscopesVisible";
let oscilloscopesVisible = localStorage.getItem(OSCILLOSCOPES_VISIBLE_KEY) === "1";

function setOscilloscopesVisible(visible) {
  oscilloscopesVisible = visible;
  document.body.classList.toggle("show-oscilloscopes", visible);
  localStorage.setItem(OSCILLOSCOPES_VISIBLE_KEY, visible ? "1" : "0");
  const checkbox = document.getElementById("menuShowOscilloscopes");
  if (checkbox) checkbox.checked = visible;
}

function setNodeCpuVisible(visible) {
  nodeCpuVisible = visible;
  document.body.classList.toggle("show-node-cpu", visible);
  localStorage.setItem(NODE_CPU_VISIBLE_KEY, visible ? "1" : "0");
  const checkbox = document.getElementById("menuToggleNodeCpu");
  if (checkbox) checkbox.checked = visible;
}

function updateNodeCpuDisplay(pairs) {
  if (!Array.isArray(pairs)) return;
  pairs.forEach((pair) => {
    const widgetId = pair && pair[0];
    const renderPercent = Number(pair && pair[1]) || 0;
    const edgePercent = Number(pair && pair[2]) || 0;
    const percent = renderPercent + edgePercent;
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
    badge.title = `render ${renderPercent.toFixed(1)}% + edges ${edgePercent.toFixed(1)}%`;
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
const WS_STALE_TIMEOUT_MS = 5000;
// The "connection lost" overlay only appears once the device has been unreachable for at
// least this long; a short drop that reconnects in time goes unnoticed (no overlay, no
// page reload).
const CONNECTION_LOST_UI_DELAY_MS = 5000;
let wsLastGoodAt = 0;
let connectionLostUiTimer = null;
const WS_WATCHDOG_INTERVAL_MS = 500;
let lastWsMessageAt = 0;
let wsWatchdogTimer = null;

// Heavy HTTP transfers to the device (e.g. the wavetable editor's live
// uploads, see postLfoWavetableLive()) occupy its web server task, so WS
// broadcasts pile up behind them and arrive late although the device is
// fine. Such callers suspend the silence check until `ms` from now; the
// stale timeout then counts from the end of the suspension, not from the
// last (delayed) message. A real socket "close" is still handled at once.
const WS_WATCHDOG_BUSY_GRACE_MS = 5000;
let wsWatchdogSuspendedUntil = 0;

function suspendWsWatchdog(ms) {
  wsWatchdogSuspendedUntil = Math.max(wsWatchdogSuspendedUntil, Date.now() + ms);
}

// Shared by the "close" handler and the watchdog below: flips the UI to
// "lost" exactly once per socket and hands the socket off. Returns false if
// this socket has already been superseded (e.g. the watchdog already
// declared it dead and moved on), so the caller can skip scheduling a
// redundant reconnect.
function markWsLost(socket) {
  if (currentSocket !== socket) return false;
  clearInterval(wsWatchdogTimer);
  currentSocket = null;
  if (!connectionLostUiTimer && !wsHasBeenDisconnected) {
    const lostFor = Date.now() - (wsLastGoodAt || Date.now());
    connectionLostUiTimer = setTimeout(() => {
      connectionLostUiTimer = null;
      if (currentSocket && currentSocket.readyState === WebSocket.OPEN) return; // back already
      wsHasBeenDisconnected = true;
      connectionLostOverlay?.classList.remove("hidden");
    }, Math.max(0, CONNECTION_LOST_UI_DELAY_MS - lostFor));
  }
  return true;
}

function startWsWatchdog(socket) {
  clearInterval(wsWatchdogTimer);
  wsWatchdogTimer = setInterval(() => {
    if (currentSocket !== socket || socket.readyState !== WebSocket.OPEN) return;
    if (lastWsMessageAt === 0) return;
    const now = Date.now();
    if (now < wsWatchdogSuspendedUntil) return;
    if (now - Math.max(lastWsMessageAt, wsWatchdogSuspendedUntil) > WS_STALE_TIMEOUT_MS) {
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
    wsLastGoodAt = lastWsMessageAt;
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
    } else if (data.type === "masterClock") {
      applyMasterClockBroadcast(data);
    } else if (data.type === "midi") {
      appendLog(`[${data.time}] ${data.port}: ${data.message}`);
      routeMidiToWidgets(data);
      routeMidiToKnobs(data);
      routeMidiToButtons(data);
      routeMidiToUpDownButtons(data);
      routeMidiToSwitches(data);
      routeMidiToOutputSwitches(data);


      routeMidiToMasterClock(data);
      routeMidiToPresetNav(data);
      if (["note_on", "note_off"].includes(data.midiType)) {
        previewMidiSplitNote(data.note, data.midiType === "note_on" && data.velocity > 0);
      }
      flashMidiActivity();
    } else if (data.type === "midi-error") {
      appendLog(`MIDI error: ${data.error}`);
    } else if (data.type === "cpuLoad") {
      updateCpuLoadDisplay(data.core0, data.core1, data.underruns);
      updateMemStatsDisplay(data.psFreeKb, data.psTotalKb, data.sysFreeKb, data.sysTotalKb);
    } else if (data.type === "nodeCpu") {
      updateNodeCpuDisplay(data.w);
    } else if (data.type === "cvStatus") {
      updateCvStatusDisplay(data.w);
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
    wsLastGoodAt = Date.now();
    clearTimeout(connectionLostUiTimer);
    connectionLostUiTimer = null;
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

// One-shot hydration on page load - the WS "masterClock" broadcast only
// fires on a subsequent change (see broadcast_status_tick()'s comment), so a
// freshly-opened tab needs this to learn the device's current bpm/mapping
// instead of showing the JS defaults until something happens to change.
fetch("/api/masterclock").then((r) => r.json()).then(applyMasterClockBroadcast).catch(() => {});






// Firmware exposes BLE pairing through its native configuration surface, not this legacy web
// API - hide the control whose /api/ble/* routes do not exist. Settings stays: it also holds
// Device Name + preset-nav Learn (backed by the real /api/settings/device route), even though
// its Ethernet/Wi-Fi section is the same kind of dead /api/settings/network call and will just
// silently fail to hydrate (openSettings() already logs-and-continues on that fetch).
document.getElementById("menuBleConnect")?.remove();



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

const menuToggleNodeCpu = document.getElementById("menuToggleNodeCpu");
menuToggleNodeCpu.checked = nodeCpuVisible;
menuToggleNodeCpu.closest(".dropdown-check").addEventListener("click", (e) => e.stopPropagation());
menuToggleNodeCpu.addEventListener("change", () => setNodeCpuVisible(menuToggleNodeCpu.checked));
setNodeCpuVisible(nodeCpuVisible);

const menuShowOscilloscopes = document.getElementById("menuShowOscilloscopes");
menuShowOscilloscopes.checked = oscilloscopesVisible;
menuShowOscilloscopes.closest(".dropdown-check").addEventListener("click", (e) => e.stopPropagation());
menuShowOscilloscopes.addEventListener("change", () => setOscilloscopesVisible(menuShowOscilloscopes.checked));
setOscilloscopesVisible(oscilloscopesVisible);

const menuCableStyle = document.getElementById("menuCableStyle");
menuCableStyle.value = cableStyleFromFlags();
menuCableStyle.closest(".dropdown-select").addEventListener("click", (e) => e.stopPropagation());
menuCableStyle.addEventListener("change", () => setCableStyle(menuCableStyle.value));

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

const CONNECTION_WIDTH_KEY = "espressynth.connectionWidth";
const menuConnectionWidth = document.getElementById("menuConnectionWidth");
const menuConnectionWidthValue = document.getElementById("menuConnectionWidthValue");

function setConnectionWidth(width, persist = true) {
  const value = Math.round(Math.min(10, Math.max(2, Number(width) || 5)));
  document.documentElement.style.setProperty("--connection-width", `${value}px`);
  menuConnectionWidth.value = String(value);
  menuConnectionWidthValue.textContent = `${value} px`;
  if (persist) localStorage.setItem(CONNECTION_WIDTH_KEY, String(value));
}

const savedConnectionWidth = Number(localStorage.getItem(CONNECTION_WIDTH_KEY));
setConnectionWidth(Number.isFinite(savedConnectionWidth) ? savedConnectionWidth : 5, false);
menuConnectionWidth.closest(".dropdown-range").addEventListener("click", (e) => e.stopPropagation());
menuConnectionWidth.addEventListener("input", () => setConnectionWidth(menuConnectionWidth.value));

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
