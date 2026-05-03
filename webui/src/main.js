const palette = [
  [255, 255, 255],
  [132, 130, 132],
  [66, 65, 66],
  [0, 0, 0]
];

const keyMap = {
  ArrowUp: 'up',
  ArrowDown: 'down',
  ArrowLeft: 'left',
  ArrowRight: 'right',
  KeyZ: 'a',
  KeyX: 'b',
  Enter: 'start',
  ShiftLeft: 'select',
  ShiftRight: 'select'
};

const canvas = document.getElementById('screen');
const statusNode = document.getElementById('status');
const metaNode = document.getElementById('meta');
const controlButtons = Array.from(document.querySelectorAll('[data-control]'));
const WS_INPUT_BACKPRESSURE_BYTES = 8 * 1024;
const WS_CONNECT_TIMEOUT_MS = 4000;
const WS_RECONNECT_BASE_MS = 500;
const WS_RECONNECT_MAX_MS = 5000;
const AUDIO_PROCESSOR_SIZE = 1024;
const AUDIO_START_BUFFER_SECONDS = 0.09;
const AUDIO_RESUME_BUFFER_SECONDS = 0.025;
const AUDIO_TARGET_BUFFER_SECONDS = 0.14;
const AUDIO_MAX_BUFFER_SECONDS = 0.32;

let imageData = null;
let socket = null;
let latestState = null;
let reconnectTimer = 0;
let reconnectAttempts = 0;
let shuttingDown = false;
let audioContext = null;
let audioProcessor = null;
let audioRequested = false;
let audioQueue = [];
let audioQueueOffset = 0;
let audioQueuedSamples = 0;
let audioPlaybackStarted = false;
let audioEverStarted = false;
let audioUnderruns = 0;
let frames = 0;
let lastFpsAt = performance.now();
let fps = 0;

const heldIntervals = new Map();
const releaseTimers = new Map();
const activeKeys = new Set();

function setStatus(text) {
  statusNode.textContent = text;
}

function setSocketConnected(connected) {
  document.body.dataset.socket = connected ? 'connected' : 'disconnected';
}

function setHeld(control, isHeld) {
  const button = document.querySelector(`[data-control="${control}"]`);
  if (!button) return;
  button.dataset.held = isHeld ? 'true' : 'false';
}

function socketWritable(limit = WS_INPUT_BACKPRESSURE_BYTES) {
  return (
    socket &&
    socket.readyState === WebSocket.OPEN &&
    socket.bufferedAmount <= limit
  );
}

function sendSocketJson(payload, limit = WS_INPUT_BACKPRESSURE_BYTES) {
  if (!socketWritable(limit)) return false;
  try {
    socket.send(JSON.stringify(payload));
    return true;
  } catch {
    return false;
  }
}

function scheduleReconnect() {
  if (shuttingDown) return;
  window.clearTimeout(reconnectTimer);
  const capped = Math.min(
    WS_RECONNECT_MAX_MS,
    WS_RECONNECT_BASE_MS * 2 ** reconnectAttempts
  );
  reconnectAttempts += 1;
  const jitter = Math.floor(Math.random() * 250);
  reconnectTimer = window.setTimeout(() => {
    connectSocket().catch(() => scheduleReconnect());
  }, capped + jitter);
}

function updateMeta(state = latestState) {
  const parts = [`stream ${fps} fps`];
  if (state) {
    parts.push(`AP ${state.network.ssid}`);
    parts.push(`${state.network.ip}`);
    parts.push(`clients ${state.network.socketClients}`);
    parts.push(`heap ${state.memory.freeHeap}`);
    if (state.audio) {
      if (state.audio.available) {
        const audioParts = [
          `audio ${state.audio.enabled ? 'on' : 'off'} ${state.audio.sampleRate}hz`
        ];
        if (audioRequested && audioContext) {
          const queuedMs = Math.round(
            (audioQueuedSamples / audioContext.sampleRate) * 1000
          );
          audioParts.push(`buf ${queuedMs}ms`);
          if (audioUnderruns) audioParts.push(`xruns ${audioUnderruns}`);
        }
        parts.push(audioParts.join(' '));
      } else {
        parts.push('audio disabled');
      }
    }
    if (state.input) {
      parts.push(`web buttons ${state.input.webButtons}`);
      parts.push(`web directions ${state.input.webDirections}`);
      parts.push(`ff00 ${state.input.ff00}`);
    }
  }
  metaNode.textContent = parts.join('\n');
}

function drawPackedFrame(buffer) {
  if (!canvas) return;

  const context = canvas.getContext('2d');
  if (!context) return;

  if (!imageData) {
    imageData = context.createImageData(160, 144);
  }

  const bytes = new Uint8Array(buffer);
  if (bytes.length < 6 || bytes[0] !== 71 || bytes[1] !== 66 || bytes[2] !== 70) {
    return;
  }

  let src = 6;
  let pixel = 0;
  while (pixel < 160 * 144 && src < bytes.length) {
    const packed = bytes[src++];
    for (let shift = 0; shift < 8 && pixel < 160 * 144; shift += 2) {
      const rgb = palette[(packed >> shift) & 0x03];
      const dst = pixel++ * 4;
      imageData.data[dst] = rgb[0];
      imageData.data[dst + 1] = rgb[1];
      imageData.data[dst + 2] = rgb[2];
      imageData.data[dst + 3] = 255;
    }
  }

  context.putImageData(imageData, 0, 0);
  frames += 1;
  const now = performance.now();
  if (now - lastFpsAt >= 1000) {
    fps = frames;
    frames = 0;
    lastFpsAt = now;
    updateMeta();
  }
}

function requestAudioStream(enabled) {
  sendSocketJson({ type: 'audio', enabled });
}

function resetAudioQueue() {
  audioQueue = [];
  audioQueueOffset = 0;
  audioQueuedSamples = 0;
  audioPlaybackStarted = false;
  audioEverStarted = false;
  audioUnderruns = 0;
}

function dropAudioSamples(samplesToDrop) {
  let remaining = samplesToDrop;
  while (remaining > 0 && audioQueue.length) {
    const current = audioQueue[0];
    const available = current.length - audioQueueOffset;
    if (remaining >= available) {
      remaining -= available;
      audioQueuedSamples -= available;
      audioQueue.shift();
      audioQueueOffset = 0;
    } else {
      audioQueueOffset += remaining;
      audioQueuedSamples -= remaining;
      remaining = 0;
    }
  }
  if (!audioQueue.length) {
    audioQueueOffset = 0;
    audioQueuedSamples = 0;
  }
}

function trimAudioQueue() {
  if (!audioContext) return;
  const maxSamples = Math.floor(
    audioContext.sampleRate * AUDIO_MAX_BUFFER_SECONDS
  );
  if (audioQueuedSamples <= maxSamples) return;

  const targetSamples = Math.floor(
    audioContext.sampleRate * AUDIO_TARGET_BUFFER_SECONDS
  );
  dropAudioSamples(audioQueuedSamples - targetSamples);
}

function renderQueuedAudio(event) {
  const output = event.outputBuffer.getChannelData(0);
  output.fill(0);

  if (!audioContext || !audioQueue.length) {
    audioPlaybackStarted = false;
    return;
  }

  if (!audioPlaybackStarted) {
    const bufferSeconds = audioEverStarted
      ? AUDIO_RESUME_BUFFER_SECONDS
      : AUDIO_START_BUFFER_SECONDS;
    const startSamples = Math.floor(audioContext.sampleRate * bufferSeconds);
    if (audioQueuedSamples < startSamples) return;
    audioPlaybackStarted = true;
    audioEverStarted = true;
  }

  let written = 0;
  while (written < output.length && audioQueue.length) {
    const current = audioQueue[0];
    const available = current.length - audioQueueOffset;
    const count = Math.min(output.length - written, available);
    output.set(
      current.subarray(audioQueueOffset, audioQueueOffset + count),
      written
    );
    written += count;
    audioQueueOffset += count;
    audioQueuedSamples -= count;

    if (audioQueueOffset >= current.length) {
      audioQueue.shift();
      audioQueueOffset = 0;
    }
  }

  if (written < output.length) {
    audioUnderruns += 1;
    audioPlaybackStarted = false;
    updateMeta();
  }
}

function startAudioOutput() {
  if (!audioContext || audioProcessor) return true;
  if (!audioContext.createScriptProcessor) {
    setStatus('audio unsupported');
    return false;
  }

  audioProcessor = audioContext.createScriptProcessor(
    AUDIO_PROCESSOR_SIZE,
    0,
    1
  );
  audioProcessor.onaudioprocess = renderQueuedAudio;
  audioProcessor.connect(audioContext.destination);
  return true;
}

function ensureAudio() {
  if (latestState?.audio && !latestState.audio.available) {
    return;
  }
  if (!window.AudioContext && !window.webkitAudioContext) {
    setStatus('audio unsupported');
    return;
  }
  if (!audioContext) {
    const Context = window.AudioContext || window.webkitAudioContext;
    audioContext = new Context();
  }
  if (!startAudioOutput()) return;
  if (audioContext.state === 'suspended') {
    audioContext.resume().catch(() => {});
  }
  if (!audioRequested) resetAudioQueue();
  audioRequested = true;
  requestAudioStream(true);
}

function playAudioChunk(buffer) {
  const bytes = new Uint8Array(buffer);
  if (bytes.length < 9 || bytes[0] !== 71 || bytes[1] !== 66 || bytes[2] !== 65) {
    return;
  }

  if (!audioContext) return;

  const sampleRate = bytes[4] | (bytes[5] << 8);
  const sampleCount = bytes[6] | (bytes[7] << 8);
  if (!sampleRate || !sampleCount || bytes.length < 8 + sampleCount) return;

  if (!startAudioOutput()) return;

  const ratio = audioContext.sampleRate / sampleRate;
  const outputCount = Math.max(1, Math.round(sampleCount * ratio));
  const output = new Float32Array(outputCount);
  for (let index = 0; index < outputCount; index += 1) {
    const sourcePosition = index / ratio;
    const left = Math.min(sampleCount - 1, Math.floor(sourcePosition));
    const right = Math.min(sampleCount - 1, left + 1);
    const blend = sourcePosition - left;
    const leftSample = (bytes[8 + left] - 128) / 128;
    const rightSample = (bytes[8 + right] - 128) / 128;
    output[index] = leftSample + (rightSample - leftSample) * blend;
  }

  audioQueue.push(output);
  audioQueuedSamples += output.length;
  trimAudioQueue();
}

async function loadState() {
  const response = await fetch('/api/state');
  const state = await response.json();
  latestState = state;
  setStatus(`${state.network.ssid} ${state.network.ip}`);
  updateMeta(state);
  return state;
}

async function websocketPort() {
  if (latestState?.network?.websocketPort) {
    return latestState.network.websocketPort;
  }
  try {
    const state = await loadState();
    return state.network.websocketPort || Number(window.location.port || 80);
  } catch {
    return Number(window.location.port || 80);
  }
}

async function sendInput(control, pressed) {
  if (sendSocketJson({ type: 'input', control, pressed })) {
    return;
  }
  await fetch('/api/input', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: `control=${encodeURIComponent(control)}&pressed=${pressed ? '1' : '0'}`
  }).catch(() => {});
}

function pressControl(control) {
  ensureAudio();

  if (releaseTimers.has(control)) {
    clearTimeout(releaseTimers.get(control));
    releaseTimers.delete(control);
  }

  setHeld(control, true);
  if (heldIntervals.has(control)) {
    clearInterval(heldIntervals.get(control));
  }

  heldIntervals.set(control, setInterval(() => sendInput(control, true), 250));
  sendInput(control, true);
  setStatus(`pressed ${control}`);
}

function releaseControl(control) {
  setHeld(control, false);

  if (heldIntervals.has(control)) {
    clearInterval(heldIntervals.get(control));
    heldIntervals.delete(control);
  }
  if (releaseTimers.has(control)) {
    clearTimeout(releaseTimers.get(control));
  }

  const timer = setTimeout(() => {
    sendInput(control, false);
    releaseTimers.delete(control);
  }, 120);
  releaseTimers.set(control, timer);
}

function bindPointerHandlers(button) {
  const control = button.dataset.control;
  button.addEventListener('pointerdown', (event) => {
    event.preventDefault();
    try {
      button.setPointerCapture(event.pointerId);
    } catch {
      // Ignore capture failures.
    }
    pressControl(control);
  });

  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((name) => {
    button.addEventListener(name, () => releaseControl(control));
  });
}

async function connectSocket() {
  if (shuttingDown) return;
  setSocketConnected(false);
  window.clearTimeout(reconnectTimer);
  if (
    socket &&
    (socket.readyState === WebSocket.OPEN ||
      socket.readyState === WebSocket.CONNECTING)
  ) {
    socket.close();
  }

  const port = await websocketPort();
  const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
  const nextSocket = new WebSocket(
    `${protocol}://${window.location.hostname}:${port}/ws`
  );
  nextSocket.binaryType = 'arraybuffer';
  socket = nextSocket;
  const openTimeout = window.setTimeout(() => {
    if (nextSocket.readyState === WebSocket.CONNECTING) {
      nextSocket.close();
    }
  }, WS_CONNECT_TIMEOUT_MS);

  nextSocket.addEventListener('open', () => {
    window.clearTimeout(openTimeout);
    reconnectAttempts = 0;
    setSocketConnected(true);
    setStatus('connected', true);
    if (audioRequested) requestAudioStream(true);
    loadState().catch(() => {});
  });

  nextSocket.addEventListener('message', (event) => {
    if (typeof event.data === 'string') {
      try {
        const state = JSON.parse(event.data);
        latestState = state;
        updateMeta(state);
      } catch {
        // Ignore malformed messages.
      }
      return;
    }

    const bytes = new Uint8Array(event.data);
    if (bytes[0] === 71 && bytes[1] === 66 && bytes[2] === 65) {
      playAudioChunk(event.data);
    } else {
      drawPackedFrame(event.data);
    }
  });

  nextSocket.addEventListener('close', () => {
    window.clearTimeout(openTimeout);
    setSocketConnected(false);
    setStatus('reconnecting');
    resetAudioQueue();
    if (socket === nextSocket) {
      socket = null;
    }
    scheduleReconnect();
  });

  nextSocket.addEventListener('error', () => {
    setSocketConnected(false);
    try {
      nextSocket.close();
    } catch {
      // Ignore close failures; the reconnect timer handles recovery.
    }
  });
}

function onKeyDown(event) {
  const control = keyMap[event.code];
  if (!control || activeKeys.has(event.code)) return;
  ensureAudio();
  activeKeys.add(event.code);
  sendInput(control, true);
}

function onKeyUp(event) {
  const control = keyMap[event.code];
  if (!control) return;
  activeKeys.delete(event.code);
  sendInput(control, false);
}

controlButtons.forEach((button) => bindPointerHandlers(button));

window.addEventListener('keydown', onKeyDown);
window.addEventListener('keyup', onKeyUp);

connectSocket().catch(() => {
  setSocketConnected(false);
  setStatus('reconnecting');
  scheduleReconnect();
});

loadState().catch(() => {});

window.addEventListener('pagehide', () => {
  shuttingDown = true;
  window.clearTimeout(reconnectTimer);
  if (socket) socket.close();
  heldIntervals.forEach((timer) => clearInterval(timer));
  heldIntervals.clear();
  releaseTimers.forEach((timer) => clearTimeout(timer));
  releaseTimers.clear();
});

if ('serviceWorker' in navigator) {
  window.addEventListener('load', () => {
    navigator.serviceWorker.register('/sw.js').catch(() => {});
  });
}
