const MIDI_MIN = 36;
const MIDI_MAX = 84;
const KEYBOARD_START = 36;
const KEYBOARD_END = MIDI_MAX;
const NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const WHITE_NAMES = new Set(["C", "D", "E", "F", "G", "A", "B"]);
const DEGREE_NAMES = { C: "Do", D: "Re", E: "Mi", F: "Fa", G: "Sol", A: "La", B: "Si" };
const DEFAULT_NOTES = [60, 62, 64, 65, 67, 69, 71];
const DEFAULT_COLORS = ["#ff0000", "#ff8800", "#ffff00", "#00ff00", "#00aaff", "#4444ff", "#aa00ff"];
const keyboardMap = { z: 0, s: 1, x: 2, d: 3, c: 4, v: 5, g: 6, b: 7, h: 8, n: 9, j: 10, m: 11, q: 12, 2: 13, w: 14, 3: 15, e: 16, r: 17, 5: 18, t: 19, 6: 20, y: 21, 7: 22, u: 23 };

const state = {
  port: null, reader: null, writer: null, writeChain: Promise.resolve(), keepReading: false,
  activeNotes: new Set(), octave: 4, transpose: 0, bpm: 120, volume: 80, playing: false, click: true,
  recording: false, pattern: Array.from({ length: 3 }, () => Array(16).fill(false)), heldKeys: new Set(),
  pitchMap: [...DEFAULT_NOTES], pitchMapEnabled: Array(7).fill(true), colors: [...DEFAULT_COLORS], brightness: Array(7).fill(100),
  localAudio: false, audioContext: null, localVoices: new Map(), previewTimers: new Map(), mappingSelection: null,
  configRevision: 0, dirty: false, awaitingState: false,
};

const $ = (id) => document.getElementById(id);
const keyboard = $("keyboard");
const activeNoteList = $("active-note-list");
const log = $("serial-log");
const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
function noteName(midi) { return `${NOTE_NAMES[midi % 12]}${Math.floor(midi / 12) - 1}`; }
function noteLabel(midi) { const name = NOTE_NAMES[midi % 12]; return name.length === 1 ? DEGREE_NAMES[name] : name; }
function describeNote(midi) { return `${noteLabel(midi)} ${noteName(midi)}`; }
function hardwareKeysForMidi(midi) { return state.pitchMap.reduce((labels, mapped, index) => { if (state.pitchMapEnabled[index] && mapped === midi) labels.push(`S${index + 1}`); return labels; }, []); }

function describeMessage(message, direction) {
  if (typeof message === "string") return message.startsWith("Web Serial") ? "网页已连接设备" : message;
  const notes = Array.isArray(message.notes) ? message.notes.map((n, i) => `S${i + 1}=${describeNote(Number(n))}`).join("，") : "";
  if (message.t === "state_get") return "请求读取硬件真实配置";
  if (message.t === "state") return `已同步硬件配置 rev ${message.rev ?? 0}`;
  if (["mapping_apply", "mapping_save", "pitch_map"].includes(message.t)) return `${message.t === "mapping_save" || message.t === "pitch_map" ? "保存" : "应用"}音高：${notes}`;
  if (message.t === "mapping_ack") return message.ok ? `${message.saved ? "音高已保存" : "音高已应用"}，rev ${message.rev ?? 0}` : "音高映射失败";
  if (["led_apply", "led_save"].includes(message.t)) return message.t === "led_save" ? "保存按键颜色" : "应用按键颜色预览";
  if (message.t === "config_ack") return message.saved ? `硬件已保存配置，rev ${message.rev}` : "硬件已应用运行配置";
  if (message.t === "config_error") return `硬件保存失败：${message.reason || "未知原因"}`;
  if (message.t === "hw_note_on") return `硬件按下 S${message.k}：${describeNote(Number(message.n))}`;
  if (message.t === "hw_note_off") return `硬件松开 S${message.k}：${describeNote(Number(message.n))}`;
  if (message.t === "note_on") return `网页弹奏：${describeNote(Number(message.n))}`;
  if (message.t === "note_off") return `网页松开：${describeNote(Number(message.n))}`;
  if (message.t === "transport") return message.cmd === "start" ? "开始播放" : "停止播放";
  if (message.t === "click") return message.on ? "打开 Click" : "关闭 Click";
  if (message.t === "tempo") return `调整 BPM：${message.bpm}`;
  if (["volume", "volume_set", "volume_ack"].includes(message.t)) return `硬件音量：${message.value}%`;
  if (message.t === "pattern_step") return `修改节奏：第 ${Number(message.s) + 1} 步`;
  if (message.t === "pattern_clear") return "清空节奏";
  if (message.t === "rec") return message.on ? "开始录制" : "停止录制";
  return direction === "out" ? "发送控制指令" : "收到设备消息";
}
function addLog(direction, value) {
  const line = document.createElement("div"); line.className = `log-entry ${direction}`;
  line.textContent = `${direction === "out" ? "发送" : "收到"}：${describeMessage(value, direction)}`;
  log.append(line); while (log.children.length > 100) log.firstElementChild.remove(); log.scrollTop = log.scrollHeight;
}
function setDirty(dirty) { state.dirty = dirty; $("save-state").textContent = dirty ? "有未保存修改" : `已同步 rev ${state.configRevision}`; }
function setStatus(online, text) {
  $("serial-status").dataset.state = online ? "online" : "offline"; $("serial-status-text").textContent = text;
  $("hardware-state").textContent = online ? "在线" : "离线"; $("port-state").textContent = online ? "已连接" : "未连接";
  $("connect-button").textContent = online ? "断开设备" : "连接设备";
}
async function sendMessage(message) {
  addLog("out", message);
  if (!state.writer) { $("last-event").textContent = "网页未连接硬件"; return false; }
  try {
    const writer = state.writer; const write = state.writeChain.then(() => writer.write(new TextEncoder().encode(`${JSON.stringify(message)}\n`)));
    state.writeChain = write.catch(() => {}); await write; return true;
  } catch (error) { addLog("in", `写入失败：${error.message}`); await disconnectSerial(); return false; }
}
function updateVolume(value) {
  state.volume = clamp(Number(value), 0, 100); $("volume-value").textContent = `${state.volume}%`; $("top-volume").textContent = `${state.volume}%`;
  $("volume-slider").value = String(state.volume); $("volume-slider-value").textContent = `${state.volume}%`;
}
function updateActiveNotes() {
  const notes = [...state.activeNotes].sort((a, b) => a - b); $("active-count").textContent = String(notes.length); activeNoteList.replaceChildren();
  if (!notes.length) { const empty = document.createElement("li"); empty.className = "muted"; empty.textContent = "没有活动音符"; activeNoteList.append(empty); }
  else notes.forEach((midi) => { const item = document.createElement("li"); item.innerHTML = `<strong>${noteName(midi)}</strong><span>MIDI ${midi}</span>`; activeNoteList.append(item); });
  $("active-note-name").textContent = notes.length ? noteName(notes.at(-1)) : "未按键";
  $("active-note-detail").textContent = notes.length ? `MIDI ${notes.at(-1)} · ${Math.floor(notes.at(-1) / 12) - 1} 八度` : "MIDI -- · --";
}
function markKey(midi, active) { document.querySelectorAll(`[data-midi="${midi}"]`).forEach((element) => element.classList.toggle("active", active)); }
function updateHardwareKeyLabels() {
  const strip = $("hardware-map-strip"); strip.replaceChildren();
  state.pitchMap.forEach((midi, index) => { const item = document.createElement("span"); item.className = "hardware-map-item"; item.textContent = `S${index + 1} ${state.pitchMapEnabled[index] ? describeNote(midi) : "停用"}`; strip.append(item); });
  document.querySelectorAll(".keyboard [data-midi]").forEach((key) => { const badge = key.querySelector(".key-hardware"); const labels = hardwareKeysForMidi(Number(key.dataset.midi)); badge.textContent = labels.join(" / "); key.classList.toggle("mapped-hardware", labels.length > 0); });
}
function sendNoteOn(midi, velocity = 100) {
  const safe = clamp(midi, MIDI_MIN, MIDI_MAX); if (state.activeNotes.has(safe)) return;
  state.activeNotes.add(safe); markKey(safe, true); updateActiveNotes(); $("last-event").textContent = `${noteName(safe)} 发声`;
  if (state.localAudio) playLocalNote(safe, velocity); sendMessage({ t: "note_on", n: safe, v: clamp(velocity, 1, 127) });
}
function sendNoteOff(midi) {
  if (!state.activeNotes.has(midi)) return; state.activeNotes.delete(midi); markKey(midi, false); updateActiveNotes(); if (state.localAudio) stopLocalNote(midi); sendMessage({ t: "note_off", n: midi });
}
function ensureAudioContext() { if (!state.audioContext) state.audioContext = new AudioContext(); if (state.audioContext.state === "suspended") state.audioContext.resume(); return state.audioContext; }
function playLocalNote(midi, velocity = 100) { if (!state.localAudio || state.localVoices.has(midi)) return; const context = ensureAudioContext(); const oscillator = context.createOscillator(); const gain = context.createGain(); const now = context.currentTime; oscillator.type = "sine"; oscillator.frequency.value = 440 * 2 ** ((midi - 69) / 12); gain.gain.setValueAtTime(0.0001, now); gain.gain.exponentialRampToValueAtTime(0.12 * velocity / 127, now + 0.008); oscillator.connect(gain).connect(context.destination); oscillator.start(now); state.localVoices.set(midi, { oscillator, gain }); }
function stopLocalNote(midi) { const voice = state.localVoices.get(midi); if (!voice || !state.audioContext) return; const now = state.audioContext.currentTime; voice.gain.gain.cancelScheduledValues(now); voice.gain.gain.setValueAtTime(Math.max(voice.gain.gain.value, 0.0001), now); voice.gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.035); voice.oscillator.stop(now + 0.04); state.localVoices.delete(midi); }
function stopAllLocalNotes() { for (const midi of [...state.localVoices.keys()]) stopLocalNote(midi); }
function releaseAllNotes() { for (const midi of [...state.activeNotes]) sendNoteOff(midi); state.heldKeys.clear(); }
function previewMappedNote(midi) { const safe = clamp(midi, MIDI_MIN, MIDI_MAX); if (state.activeNotes.has(safe)) return; sendMessage({ t: "note_on", n: safe, v: 105 }); const timer = setTimeout(() => { sendMessage({ t: "note_off", n: safe }); state.previewTimers.delete(safe); }, 320); state.previewTimers.set(safe, timer); }

function choosePitch(index, midi) { state.pitchMap[index] = clamp(midi, MIDI_MIN, MIDI_MAX); state.mappingSelection = null; setDirty(true); renderPitchMap(); updateHardwareKeyLabels(); $("last-event").textContent = `S${index + 1} 已选择 ${describeNote(midi)}`; }
function renderKeyboard() {
  keyboard.replaceChildren(); const whiteNotes = [];
  for (let midi = KEYBOARD_START; midi <= KEYBOARD_END; midi += 1) if (WHITE_NAMES.has(NOTE_NAMES[midi % 12])) whiteNotes.push(midi);
  const pianoDown = (midi, event, element) => { event.preventDefault(); if (state.mappingSelection !== null) { choosePitch(state.mappingSelection, midi + state.transpose); return; } element.setPointerCapture?.(event.pointerId); sendNoteOn(midi + state.transpose); };
  const pianoUp = (midi) => { if (state.mappingSelection === null) sendNoteOff(clamp(midi + state.transpose, MIDI_MIN, MIDI_MAX)); };
  whiteNotes.forEach((midi, index) => { const key = document.createElement("button"); key.type = "button"; key.className = "white-key"; key.dataset.midi = String(midi); key.style.zIndex = "1"; key.innerHTML = `<span class="key-hardware" aria-hidden="true"></span><span class="key-label">${noteLabel(midi)}</span><span class="key-octave">${noteName(midi)}</span>`; key.addEventListener("pointerdown", (event) => pianoDown(midi, event, key)); key.addEventListener("pointerup", () => pianoUp(midi)); key.addEventListener("pointercancel", () => pianoUp(midi)); keyboard.append(key); key.dataset.whiteIndex = String(index); });
  const whiteIndexByMidi = new Map(whiteNotes.map((midi, index) => [midi, index]));
  for (let midi = KEYBOARD_START; midi <= KEYBOARD_END; midi += 1) { const name = NOTE_NAMES[midi % 12]; if (!name.includes("#")) continue; const leftWhite = whiteIndexByMidi.get(midi - 1); if (leftWhite === undefined) continue; const key = document.createElement("button"); key.type = "button"; key.className = "black-key"; key.dataset.midi = String(midi); key.style.left = `${((leftWhite + 1) / whiteNotes.length) * 100}%`; key.innerHTML = `<span class="key-hardware" aria-hidden="true"></span><span class="key-label">${name}</span>`; key.addEventListener("pointerdown", (event) => pianoDown(midi, event, key)); key.addEventListener("pointerup", () => pianoUp(midi)); key.addEventListener("pointercancel", () => pianoUp(midi)); keyboard.append(key); }
  updateHardwareKeyLabels();
}
function updateOctave() { $("octave-value").textContent = `C${state.octave}`; }
function updateTranspose() { $("transpose-value").textContent = state.transpose > 0 ? `+${state.transpose}` : String(state.transpose); }
function renderPitchMap() {
  const grid = $("mapping-grid"); grid.replaceChildren();
  for (let index = 0; index < 7; index += 1) {
    const row = document.createElement("div"); row.className = "mapping-row"; row.dataset.index = String(index);
    const label = document.createElement("label"); label.textContent = `S${index + 1}`;
    const enabled = document.createElement("input"); enabled.type = "checkbox"; enabled.checked = state.pitchMapEnabled[index]; enabled.title = `启用 S${index + 1}`; enabled.addEventListener("change", () => { state.pitchMapEnabled[index] = enabled.checked; setDirty(true); updateHardwareKeyLabels(); });
    const select = document.createElement("select"); select.setAttribute("aria-label", `S${index + 1} 音高`); for (let midi = MIDI_MIN; midi <= MIDI_MAX; midi += 1) { const option = document.createElement("option"); option.value = String(midi); option.textContent = `${describeNote(midi)} · MIDI ${midi}`; option.selected = midi === state.pitchMap[index]; select.append(option); } select.addEventListener("change", () => { state.pitchMap[index] = Number(select.value); setDirty(true); updateHardwareKeyLabels(); });
    const choose = document.createElement("button"); choose.type = "button"; choose.className = "mapping-audition"; choose.textContent = state.mappingSelection === index ? "请点琴键" : "选琴键"; choose.addEventListener("click", () => { state.mappingSelection = state.mappingSelection === index ? null : index; renderPitchMap(); });
    const audition = document.createElement("button"); audition.type = "button"; audition.className = "mapping-audition"; audition.textContent = "试听"; audition.addEventListener("click", () => previewMappedNote(state.pitchMap[index]));
    const color = document.createElement("input"); color.type = "color"; color.value = state.colors[index]; color.title = `S${index + 1} RGB 颜色`; color.addEventListener("input", () => { state.colors[index] = color.value; setDirty(true); });
    const hex = document.createElement("input"); hex.type = "text"; hex.className = "color-hex"; hex.value = state.colors[index]; hex.maxLength = 7; hex.addEventListener("change", () => { if (/^#[0-9a-f]{6}$/i.test(hex.value)) { state.colors[index] = hex.value.toLowerCase(); color.value = state.colors[index]; setDirty(true); } else hex.value = state.colors[index]; });
    const bright = document.createElement("input"); bright.type = "range"; bright.min = "0"; bright.max = "100"; bright.value = String(state.brightness[index]); bright.title = "亮度"; bright.addEventListener("input", () => { state.brightness[index] = Number(bright.value); setDirty(true); });
    row.append(label, enabled, select, choose, audition, color, hex, bright); grid.append(row);
  }
}
function configPayload(kind) { const message = { t: kind, notes: state.pitchMap, enabled: state.pitchMapEnabled.map((value) => value ? 1 : 0) }; if (kind.startsWith("led")) { message.colors = state.colors; message.brightness = state.brightness; } return message; }
function updateTransport() { $("transport-state").textContent = state.playing ? "播放" : "停止"; $("play-button").setAttribute("aria-pressed", String(state.playing)); $("play-button").textContent = state.playing ? "▶ 播放中" : "▶ 播放"; $("click-state").textContent = state.click ? "开启" : "关闭"; $("click-button").textContent = state.click ? "Click 开启" : "Click 关闭"; $("rec-state").textContent = state.recording ? "录制中" : "待机"; $("rec-button").setAttribute("aria-pressed", String(state.recording)); }

function applyHardwareConfiguration(save) {
  const mappingKind = save ? "mapping_save" : "mapping_apply"; const ledKind = save ? "led_save" : "led_apply";
  setDirty(true); $("save-state").textContent = save ? "正在保存" : "正在应用";
  sendMessage(configPayload(mappingKind)); sendMessage(configPayload(ledKind));
  $("last-event").textContent = save ? "已发送保存请求，等待硬件确认" : "已应用到硬件运行态";
}
function applyState(message) {
  if (Array.isArray(message.notes) && message.notes.length === 7) state.pitchMap = message.notes.map((n) => clamp(Number(n), MIDI_MIN, MIDI_MAX));
  if (Array.isArray(message.enabled) && message.enabled.length === 7) state.pitchMapEnabled = message.enabled.map(Boolean);
  if (Array.isArray(message.colors) && message.colors.length === 7) state.colors = message.colors.map((color, index) => /^#[0-9a-f]{6}$/i.test(color) ? color.toLowerCase() : state.colors[index]);
  if (Array.isArray(message.brightness) && message.brightness.length === 7) state.brightness = message.brightness.map((value) => clamp(Number(value), 0, 100));
  if (Number.isInteger(message.bpm)) state.bpm = clamp(message.bpm, 60, 240);
  if (Number.isInteger(message.volume)) updateVolume(message.volume);
  if (typeof message.playing === "boolean") state.playing = message.playing; else if (Number.isInteger(message.run)) state.playing = Boolean(message.run);
  if (typeof message.click === "boolean") state.click = message.click;
  state.configRevision = Number.isInteger(message.rev) ? message.rev : state.configRevision; state.awaitingState = false; setDirty(false);
  $("bpm-value").textContent = String(state.bpm); $("top-bpm").textContent = String(state.bpm); $("config-revision").textContent = `rev ${state.configRevision}`; renderPitchMap(); updateHardwareKeyLabels(); updateTransport(); $("last-event").textContent = "已同步硬件真实配置";
}
function handleSerialLine(line) {
  try {
    const message = JSON.parse(line); addLog("in", message);
    if (message.t === "hw_note_on" && Number.isInteger(message.n)) { if (state.localAudio) playLocalNote(clamp(message.n, MIDI_MIN, MIDI_MAX), clamp(message.v || 100, 1, 127)); $("last-event").textContent = `硬件按键 S${message.k}：${noteName(message.n)}`; }
    else if (message.t === "hw_note_off" && Number.isInteger(message.n)) { if (state.localAudio) stopLocalNote(clamp(message.n, MIDI_MIN, MIDI_MAX)); $("last-event").textContent = `硬件释放 S${message.k}`; }
    else if (message.t === "state") applyState(message);
    else if (["volume", "volume_ack"].includes(message.t)) { updateVolume(message.value); $("last-event").textContent = `硬件音量 ${state.volume}%`; }
    else if (message.t === "tempo_ack") { state.bpm = clamp(Number(message.bpm), 60, 240); $("bpm-value").textContent = String(state.bpm); $("top-bpm").textContent = String(state.bpm); }
    else if (message.t === "mapping_ack") { if (Array.isArray(message.notes)) state.pitchMap = message.notes.map((n) => clamp(Number(n), MIDI_MIN, MIDI_MAX)); if (Number.isInteger(message.rev)) state.configRevision = message.rev; $("config-revision").textContent = `rev ${state.configRevision}`; renderPitchMap(); updateHardwareKeyLabels(); $("last-event").textContent = message.ok ? (message.saved ? "音高已保存到硬件，等待其他配置回执" : "音高已应用到硬件") : "音高映射失败"; }
    else if (message.t === "config_ack") { if (Number.isInteger(message.rev)) state.configRevision = message.rev; $("config-revision").textContent = `rev ${state.configRevision}`; if (message.saved) setDirty(false); $("last-event").textContent = message.saved ? `配置已保存，rev ${state.configRevision}` : "配置已应用"; }
    else if (message.t === "config_error") { $("save-state").textContent = "保存失败"; $("last-event").textContent = `保存失败：${message.reason || "未知原因"}`; }
  } catch { addLog("in", line); }
}
async function connectSerial() {
  if (!("serial" in navigator)) { setStatus(false, "浏览器不支持 Web Serial"); return; }
  try { state.port = await navigator.serial.requestPort(); await state.port.open({ baudRate: 115200 }); state.writer = state.port.writable.getWriter(); state.localAudio = false; $("local-audio-button").setAttribute("aria-pressed", "false"); $("local-audio-button").textContent = "电脑端音高：关闭"; state.awaitingState = true; setStatus(true, "正在读取硬件"); setDirty(false); addLog("in", "Web Serial connected"); readSerial(); await sendMessage({ t: "state_get" }); } catch (error) { addLog("in", `连接取消或失败：${error.message}`); setStatus(false, "未连接设备"); }
}
async function readSerial() {
  if (!state.port?.readable) return; state.keepReading = true; state.reader = state.port.readable.getReader(); const decoder = new TextDecoder(); let buffer = "";
  try { while (state.keepReading) { const { value, done } = await state.reader.read(); if (done) break; buffer += decoder.decode(value, { stream: true }); const lines = buffer.split("\n"); buffer = lines.pop() || ""; lines.filter(Boolean).forEach((line) => handleSerialLine(line.trim())); } }
  catch (error) { if (state.keepReading) addLog("in", `读取失败：${error.message}`); }
  finally { state.reader?.releaseLock(); state.reader = null; if (state.keepReading) { state.keepReading = false; state.writer?.releaseLock(); state.writer = null; state.port = null; setStatus(false, "设备连接已断开"); releaseAllNotes(); } }
}
async function disconnectSerial() { releaseAllNotes(); stopAllLocalNotes(); state.keepReading = false; try { await state.reader?.cancel(); } catch {} try { state.writer?.releaseLock(); } catch {} try { await state.port?.close(); } catch {} state.reader = null; state.writer = null; state.writeChain = Promise.resolve(); state.port = null; setStatus(false, "未连接设备"); }
function renderPattern() { const grid = $("pattern-grid"); grid.replaceChildren(); ["Kick", "Snare", "Hat"].forEach((label, voice) => { const row = document.createElement("div"); row.className = "pattern-row"; row.innerHTML = `<span class="pattern-label">${label}</span>`; for (let step = 0; step < 16; step += 1) { const button = document.createElement("button"); button.type = "button"; button.className = `pattern-step ${step % 4 === 0 ? "beat" : ""}`; button.setAttribute("aria-label", `${label} 第 ${step + 1} 步`); button.setAttribute("aria-pressed", String(state.pattern[voice][step])); button.classList.toggle("active", state.pattern[voice][step]); button.addEventListener("click", () => { state.pattern[voice][step] = !state.pattern[voice][step]; renderPattern(); sendMessage({ t: "pattern_step", v: voice, s: step, on: state.pattern[voice][step] }); }); row.append(button); } grid.append(row); }); }
function setTransport(playing) { state.playing = playing; updateTransport(); sendMessage({ t: "transport", cmd: playing ? "start" : "stop" }); }
function bindEvents() {
  $("connect-button").addEventListener("click", () => state.port ? disconnectSerial() : connectSerial()); $("play-button").addEventListener("click", () => setTransport(true)); $("stop-button").addEventListener("click", () => setTransport(false));
  $("click-button").addEventListener("click", () => { state.click = !state.click; updateTransport(); sendMessage({ t: "click", on: state.click }); }); $("rec-button").addEventListener("click", () => { state.recording = !state.recording; updateTransport(); sendMessage({ t: "rec", on: state.recording }); });
  $("bpm-down").addEventListener("click", () => { state.bpm = clamp(state.bpm - 1, 60, 240); $("bpm-value").textContent = String(state.bpm); $("top-bpm").textContent = String(state.bpm); sendMessage({ t: "tempo", bpm: state.bpm }); }); $("bpm-up").addEventListener("click", () => { state.bpm = clamp(state.bpm + 1, 60, 240); $("bpm-value").textContent = String(state.bpm); $("top-bpm").textContent = String(state.bpm); sendMessage({ t: "tempo", bpm: state.bpm }); });
  $("volume-slider").addEventListener("input", () => { updateVolume($("volume-slider").value); sendMessage({ t: "volume_set", value: state.volume }); }); $("octave-down").addEventListener("click", () => { state.octave = clamp(state.octave - 1, 2, 5); updateOctave(); }); $("octave-up").addEventListener("click", () => { state.octave = clamp(state.octave + 1, 2, 5); updateOctave(); }); $("transpose-down").addEventListener("click", () => { state.transpose = clamp(state.transpose - 1, -12, 12); updateTranspose(); }); $("transpose-up").addEventListener("click", () => { state.transpose = clamp(state.transpose + 1, -12, 12); updateTranspose(); });
  $("clear-pattern").addEventListener("click", () => { state.pattern.forEach((row) => row.fill(false)); renderPattern(); sendMessage({ t: "pattern_clear" }); }); $("apply-config").addEventListener("click", () => applyHardwareConfiguration(false)); $("save-config").addEventListener("click", () => applyHardwareConfiguration(true)); $("reload-config").addEventListener("click", () => { state.awaitingState = true; setStatus(Boolean(state.port), "正在读取硬件"); sendMessage({ t: "state_get" }); }); $("reset-config").addEventListener("click", () => { state.pitchMap = [...DEFAULT_NOTES]; state.pitchMapEnabled = Array(7).fill(true); state.colors = [...DEFAULT_COLORS]; state.brightness = Array(7).fill(100); setDirty(true); renderPitchMap(); updateHardwareKeyLabels(); $("last-event").textContent = "已恢复网页默认值，点击应用或保存后才发送"; });
  $("local-audio-button").addEventListener("click", () => { state.localAudio = !state.localAudio; $("local-audio-button").setAttribute("aria-pressed", String(state.localAudio)); $("local-audio-button").textContent = `电脑端试听：${state.localAudio ? "开启" : "关闭"}`; if (!state.localAudio) stopAllLocalNotes(); });
  window.addEventListener("keydown", (event) => { if (event.repeat || event.ctrlKey || event.metaKey || event.altKey) return; const key = event.key.toLowerCase(); const offset = keyboardMap[key]; if (offset === undefined) return; event.preventDefault(); const midi = (state.octave + 1) * 12 + offset + state.transpose; if (midi < MIDI_MIN || midi > MIDI_MAX || state.heldKeys.has(key)) return; state.heldKeys.add(key); sendNoteOn(midi); }); window.addEventListener("keyup", (event) => { const key = event.key.toLowerCase(); if (!state.heldKeys.has(key)) return; state.heldKeys.delete(key); sendNoteOff(clamp((state.octave + 1) * 12 + keyboardMap[key] + state.transpose, MIDI_MIN, MIDI_MAX)); });
  window.addEventListener("blur", () => { releaseAllNotes(); stopAllLocalNotes(); }); document.addEventListener("visibilitychange", () => { if (document.hidden) { releaseAllNotes(); stopAllLocalNotes(); } }); window.addEventListener("beforeunload", () => { releaseAllNotes(); stopAllLocalNotes(); });
}
renderKeyboard(); renderPattern(); renderPitchMap(); updateOctave(); updateTranspose(); updateVolume(state.volume); updateTransport(); updateActiveNotes(); setStatus(false, "未连接设备"); setDirty(false); bindEvents();
