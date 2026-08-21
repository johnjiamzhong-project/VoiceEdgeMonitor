const MAX_EVENTS = 100;
const MAX_TRANSCRIPTS = 50;
const MAX_WAVE_SAMPLES = 8192;

const $ = (id) => document.getElementById(id);
const params = new URLSearchParams(window.location.search);
const defaultWsUrl = params.get("ws") || localStorage.getItem("voiceedge.ws") ||
  `ws://${window.location.hostname || "127.0.0.1"}:8765/voiceedge`;

const state = {
  socket: null,
  manualDisconnect: false,
  reconnectAttempt: 0,
  reconnectTimer: null,
  wave: [],
  lastPacket: 0,
  audioContext: null,
  listening: false,
  nextAudioTime: 0,
};

$("ws-url").value = defaultWsUrl;

function setText(id, value) { $(id).textContent = value; }

function addEvent(message, kind = "info") {
  const list = $("event-list");
  const empty = list.querySelector(".empty-state");
  if (empty) empty.remove();
  const item = document.createElement("li");
  item.dataset.kind = kind;
  const meta = document.createElement("span");
  meta.className = "event-meta";
  meta.textContent = new Date().toLocaleTimeString();
  item.append(meta, document.createTextNode(message));
  list.prepend(item);
  while (list.children.length > MAX_EVENTS) list.lastElementChild.remove();
}

function addTranscript(text, processingMs, segmentId) {
  const list = $("transcript-list");
  const empty = list.querySelector(".empty-state");
  if (empty) empty.remove();
  const item = document.createElement("li");
  const meta = document.createElement("span");
  meta.className = "transcript-meta";
  meta.textContent = `segment ${segmentId} · ${processingMs} ms`;
  item.append(meta, document.createTextNode(text || "（空文本）"));
  list.prepend(item);
  while (list.children.length > MAX_TRANSCRIPTS) list.lastElementChild.remove();
}

function setConnection(online, label = online ? "已连接" : "未连接") {
  const badge = $("connection-badge");
  badge.textContent = label;
  badge.classList.toggle("online", online);
  badge.classList.toggle("offline", !online);
  $("listen-button").disabled = !online;
}

function updateState(message) {
  setText("board-status", "在线");
  setText("device-name", message.device || "—");
  setText("audio-format", `${message.sample_rate || "—"} Hz · ${message.channels || "—"} ch`);
  setText("audio-device", message.format || "—");
  setText("vad-status", message.vad || "idle");
  setText("vad-counts", `started ${message.vad_started || 0} / ended ${message.vad_ended || 0}`);
  setText("asr-status", message.asr || "idle");
  setText("asr-metrics", `segments ${message.asr_segments || 0} / ${message.asr_processing_ms || 0} ms`);
  setText("captured-frames", String(message.captured_frames || 0));
  setText("client-count", `clients ${message.clients || 0}`);
  setText("error-count", `${message.read_errors || 0} / ${message.xruns || 0}`);
  setText("recovery-count", `recoveries ${message.recoveries || 0}`);
}

function handleText(raw) {
  let message;
  try { message = JSON.parse(raw); } catch { addEvent(`无效 JSON：${raw}`, "error"); return; }
  if (message.type === "hello") {
    addEvent("Server hello 已收到");
  } else if (message.type === "state") {
    updateState(message);
  } else if (message.type === "vad_event") {
    setText("vad-status", message.event?.replace("vad_", "") || "unknown");
    addEvent(`${message.event || "vad_event"} · ${message.rms ?? ""}`);
  } else if (message.type === "asr_started") {
    setText("asr-status", "processing");
    addEvent(`ASR 开始 · segment ${message.segment_id}`);
  } else if (message.type === "asr_final") {
    setText("asr-status", "idle");
    addTranscript(message.text, message.processing_ms, message.segment_id);
    addEvent(`ASR 完成 · segment ${message.segment_id} · ${message.processing_ms} ms`);
  } else if (message.type === "asr_failed") {
    setText("asr-status", "error");
    addEvent(`ASR 失败 · ${message.reason || "unknown"}`, "error");
  }
}

function decodeAudio(arrayBuffer) {
  if (arrayBuffer.byteLength < 32) throw new Error("audio packet too short");
  const view = new DataView(arrayBuffer);
  if (String.fromCharCode(view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3)) !== "VEA1") {
    throw new Error("invalid VEA1 magic");
  }
  const version = view.getUint8(4);
  const codec = view.getUint8(5);
  const channels = view.getUint8(6);
  const sampleRate = view.getUint32(8, true);
  const sequence = Number(view.getBigUint64(12, true));
  const sampleCount = view.getUint32(28, true);
  if (version !== 1 || codec !== 0 || channels === 0 || arrayBuffer.byteLength !== 32 + sampleCount * 2) {
    throw new Error("invalid VEA1 audio header");
  }
  const samples = new Int16Array(sampleCount);
  for (let i = 0; i < sampleCount; i += 1) samples[i] = view.getInt16(32 + i * 2, true);
  return { channels, sampleRate, sequence, samples };
}

function handleAudio(arrayBuffer) {
  try {
    const packet = decodeAudio(arrayBuffer);
    state.lastPacket = packet.sequence;
    setText("packet-status", `packet ${packet.sequence} · ${packet.samples.length} samples`);
    const mono = [];
    for (let i = 0; i < packet.samples.length; i += packet.channels) {
      let sum = 0;
      for (let channel = 0; channel < packet.channels; channel += 1) sum += packet.samples[i + channel] || 0;
      mono.push(sum / packet.channels / 32768);
    }
    state.wave.push(...mono);
    if (state.wave.length > MAX_WAVE_SAMPLES) state.wave.splice(0, state.wave.length - MAX_WAVE_SAMPLES);
    if (state.listening) scheduleAudio(packet);
  } catch (error) {
    addEvent(`音频包错误：${error.message}`, "error");
  }
}

function scheduleAudio(packet) {
  if (!state.audioContext) return;
  const context = state.audioContext;
  if (context.state === "suspended") context.resume();
  const frames = Math.floor(packet.samples.length / packet.channels);
  const now = context.currentTime;
  if (state.nextAudioTime < now) state.nextAudioTime = now + 0.03;
  if (state.nextAudioTime - now > 0.6) return;
  const buffer = context.createBuffer(packet.channels, frames, packet.sampleRate);
  for (let channel = 0; channel < packet.channels; channel += 1) {
    const data = buffer.getChannelData(channel);
    for (let frame = 0; frame < frames; frame += 1) {
      data[frame] = packet.samples[frame * packet.channels + channel] / 32768;
    }
  }
  const source = context.createBufferSource();
  source.buffer = buffer;
  source.connect(context.destination);
  source.start(state.nextAudioTime);
  state.nextAudioTime += buffer.duration;
}

function drawWaveform() {
  const canvas = $("waveform");
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (canvas.width !== Math.floor(width * ratio) || canvas.height !== Math.floor(height * ratio)) {
    canvas.width = Math.floor(width * ratio);
    canvas.height = Math.floor(height * ratio);
  }
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  context.strokeStyle = "rgba(143, 163, 189, 0.15)";
  context.beginPath();
  context.moveTo(0, height / 2);
  context.lineTo(width, height / 2);
  context.stroke();
  if (state.wave.length > 1) {
    context.strokeStyle = "#4da3ff";
    context.lineWidth = 1.5;
    context.beginPath();
    state.wave.forEach((sample, index) => {
      const x = index / (state.wave.length - 1) * width;
      const y = height / 2 - Math.max(-1, Math.min(1, sample)) * height * 0.42;
      if (index === 0) context.moveTo(x, y); else context.lineTo(x, y);
    });
    context.stroke();
  }
  requestAnimationFrame(drawWaveform);
}

function connect() {
  const url = $("ws-url").value.trim();
  if (!url) return;
  localStorage.setItem("voiceedge.ws", url);
  state.manualDisconnect = false;
  if (state.socket) state.socket.close();
  setConnection(false, "连接中");
  const socket = new WebSocket(url);
  state.socket = socket;
  socket.binaryType = "arraybuffer";
  socket.onopen = () => {
    state.reconnectAttempt = 0;
    setConnection(true);
    addEvent(`已连接 ${url}`);
    socket.send(JSON.stringify({ type: "hello", version: 1, client: "browser" }));
  };
  socket.onmessage = (event) => typeof event.data === "string" ? handleText(event.data) : handleAudio(event.data);
  socket.onerror = () => addEvent("WebSocket 错误", "error");
  socket.onclose = () => {
    setConnection(false);
    if (!state.manualDisconnect) {
      const delay = Math.min(10000, 500 * (2 ** state.reconnectAttempt));
      state.reconnectAttempt += 1;
      addEvent(`连接断开，${delay} ms 后重连`);
      clearTimeout(state.reconnectTimer);
      state.reconnectTimer = setTimeout(connect, delay);
    }
  };
}

function disconnect() {
  state.manualDisconnect = true;
  clearTimeout(state.reconnectTimer);
  if (state.socket) state.socket.close();
  state.socket = null;
  setConnection(false);
}

async function toggleListening() {
  if (!state.listening) {
    state.audioContext = new AudioContext();
    await state.audioContext.resume();
    state.nextAudioTime = state.audioContext.currentTime + 0.05;
    state.listening = true;
    $("listen-button").textContent = "停止监听";
    setText("audio-status", "监听中");
  } else {
    state.listening = false;
    if (state.audioContext) await state.audioContext.close();
    state.audioContext = null;
    $("listen-button").textContent = "开始监听";
    setText("audio-status", "已停止");
  }
}

$("connect-button").addEventListener("click", connect);
$("disconnect-button").addEventListener("click", disconnect);
$("listen-button").addEventListener("click", toggleListening);
$("clear-events").addEventListener("click", () => {
  $("event-list").innerHTML = '<li class="empty-state">暂无事件</li>';
});
window.addEventListener("beforeunload", disconnect);
drawWaveform();
connect();
