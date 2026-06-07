const controlButtons = Array.from(document.querySelectorAll('[data-control]'));

const nodes = {
  connection: document.getElementById('connection'),
  status: document.getElementById('status'),
  network: document.getElementById('network-status'),
  controller: document.getElementById('controller-status'),
  transferPak: document.getElementById('transfer-pak-status'),
  cartridge: document.getElementById('cartridge-status'),
  compat: document.getElementById('compat-status'),
  debug: document.getElementById('debug-status'),
  stickReadout: document.getElementById('stick-readout'),
  analogDot: document.getElementById('analog-dot')
};

const WS_INPUT_BACKPRESSURE_BYTES = 8 * 1024;
const WS_CONNECT_TIMEOUT_MS = 4000;
const WS_RECONNECT_BASE_MS = 500;
const WS_RECONNECT_MAX_MS = 5000;

const buttonBits = {
  A: 1 << 15,
  B: 1 << 14,
  Z: 1 << 13,
  START: 1 << 12,
  UP: 1 << 11,
  DOWN: 1 << 10,
  LEFT: 1 << 9,
  RIGHT: 1 << 8,
  L: 1 << 5,
  R: 1 << 4,
  C_UP: 1 << 3,
  C_DOWN: 1 << 2,
  C_LEFT: 1 << 1,
  C_RIGHT: 1 << 0
};

const keyMap = {
  ArrowUp: 'UP',
  ArrowDown: 'DOWN',
  ArrowLeft: 'LEFT',
  ArrowRight: 'RIGHT',
  KeyZ: 'A',
  KeyX: 'B',
  KeyC: 'Z',
  Enter: 'START',
  ShiftLeft: 'L',
  ShiftRight: 'R',
  KeyI: 'C_UP',
  KeyK: 'C_DOWN',
  KeyJ: 'C_LEFT',
  KeyL: 'C_RIGHT'
};

let socket = null;
let latestState = null;
let reconnectTimer = 0;
let reconnectAttempts = 0;
let shuttingDown = false;

const heldIntervals = new Map();
const releaseTimers = new Map();
const activeKeys = new Set();

function valueOrDash(value, formatter = String) {
  if (value === undefined || value === null || value === '') return 'unavailable';
  return formatter(value);
}

function boolLabel(value) {
  if (value === undefined || value === null) return 'unavailable';
  return value ? 'yes' : 'no';
}

function hex(value, width = 2) {
  if (typeof value !== 'number' || Number.isNaN(value)) return 'unavailable';
  return `0x${value.toString(16).toUpperCase().padStart(width, '0')}`;
}

function count(value) {
  if (typeof value !== 'number' || Number.isNaN(value)) return 'unavailable';
  return value.toLocaleString();
}

function setStatus(text) {
  if (nodes.status) nodes.status.textContent = text;
}

function setSocketConnected(connected) {
  document.body.dataset.socket = connected ? 'connected' : 'disconnected';
  if (nodes.connection) nodes.connection.textContent = connected ? 'connected' : 'offline';
}

function setHeld(control, isHeld) {
  document.querySelectorAll(`[data-control="${control}"]`).forEach((button) => {
    button.dataset.held = isHeld ? 'true' : 'false';
  });
}

function socketWritable(limit = WS_INPUT_BACKPRESSURE_BYTES) {
  return socket && socket.readyState === WebSocket.OPEN && socket.bufferedAmount <= limit;
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

function rows(target, entries) {
  if (!target) return;
  target.replaceChildren();
  entries.forEach(([label, value]) => {
    const dt = document.createElement('dt');
    const dd = document.createElement('dd');
    dt.textContent = label;
    dd.textContent = value;
    target.append(dt, dd);
  });
}

function updateButtonState(buttons = 0) {
  Object.entries(buttonBits).forEach(([control, bit]) => {
    const pressed = (buttons & bit) !== 0;
    document.querySelectorAll(`[data-control="${control}"]`).forEach((button) => {
      button.dataset.active = pressed ? 'true' : 'false';
    });
  });
}

function updateAnalog(stickX = 0, stickY = 0) {
  const x = Number.isFinite(stickX) ? stickX : 0;
  const y = Number.isFinite(stickY) ? stickY : 0;
  if (nodes.stickReadout) nodes.stickReadout.textContent = `${x} / ${y}`;
  if (nodes.analogDot) {
    const clamp = (value) => Math.max(-80, Math.min(80, value));
    nodes.analogDot.style.transform =
      `translate(${clamp(x) / 1.6}%, ${-clamp(y) / 1.6}%)`;
  }
}

function renderState(state = latestState) {
  const network = state?.network || {};
  const memory = state?.memory || {};
  const controller = state?.controller || {};
  const pak = state?.transferPak || {};
  const cartridge = state?.cartridge || {};
  const compat = state?.compat || {};
  const debug = state?.debug || {};

  updateButtonState(controller.buttons || 0);
  updateAnalog(controller.stickX, controller.stickY);

  rows(nodes.network, [
    ['SSID', valueOrDash(network.ssid)],
    ['IP', valueOrDash(network.ip)],
    ['WebSocket port', valueOrDash(network.websocketPort)],
    ['Clients', valueOrDash(network.socketClients, count)],
    ['Free heap', valueOrDash(memory.freeHeap, count)],
    ['Min free heap', valueOrDash(memory.minFreeHeap, count)]
  ]);

  rows(nodes.controller, [
    ['Buttons', hex(controller.buttons, 4)],
    ['Stick X', valueOrDash(controller.stickX)],
    ['Stick Y', valueOrDash(controller.stickY)]
  ]);

  rows(nodes.transferPak, [
    ['Powered', boolLabel(pak.powered)],
    ['Access enabled', boolLabel(pak.accessEnabled)],
    ['Bank', valueOrDash(pak.bank)],
    ['Status', hex(pak.status)],
    ['Reads', valueOrDash(pak.reads, count)],
    ['Writes', valueOrDash(pak.writes, count)],
    ['Invalid accesses', valueOrDash(pak.invalidAccesses, count)]
  ]);

  rows(nodes.cartridge, [
    ['Title', valueOrDash(cartridge.title)],
    ['ROM loaded', boolLabel(cartridge.romLoaded)],
    ['Save loaded', boolLabel(cartridge.saveLoaded)],
    ['Save stubbed', boolLabel(cartridge.saveStubbed)],
    ['Save dirty', boolLabel(cartridge.saveDirty)],
    ['Save persisted', boolLabel(cartridge.savePersisted)],
    ['Loaded persisted', boolLabel(cartridge.saveLoadedPersisted)],
    ['Save pending', boolLabel(cartridge.savePending)],
    ['Load result', valueOrDash(cartridge.saveLoadResult)],
    ['Flush result', valueOrDash(cartridge.saveFlushResult)],
    ['Last flush OK', boolLabel(cartridge.saveLastFlushOk)],
    ['Flush count', valueOrDash(cartridge.saveFlushCount, count)],
    ['Failed flushes', valueOrDash(cartridge.saveFailedFlushCount, count)],
    ['Write sequence', valueOrDash(cartridge.saveWriteSeq, count)],
    ['Changed bytes', valueOrDash(cartridge.saveChangedBytes, count)],
    ['Last save offset', hex(cartridge.saveLastOffset, 4)],
    ['Bounds fault', boolLabel(cartridge.boundsFault)],
    ['Type', hex(cartridge.type)],
    ['ROM banks', valueOrDash(cartridge.romBanks, count)],
    ['RAM banks', valueOrDash(cartridge.ramBanks, count)],
    ['Header checksum', hex(cartridge.headerChecksum)]
  ]);

  rows(nodes.compat, [
    ['Accessory present', boolLabel(compat.accessoryPresent)],
    ['ROM header OK', boolLabel(compat.romHeaderOk)],
    ['Save read-only/stubbed', boolLabel(compat.saveReadOnlyOrStubbed)]
  ]);

  rows(nodes.debug, [
    ['Joy-Bus status', valueOrDash(debug.joybusStatus, count)],
    ['Joy-Bus poll', valueOrDash(debug.joybusPoll, count)],
    ['Accessory reads', valueOrDash(debug.joybusAccessoryReads, count)],
    ['Accessory writes', valueOrDash(debug.joybusAccessoryWrites, count)],
    ['Malformed frames', valueOrDash(debug.joybusMalformed, count)],
    ['Timing errors', valueOrDash(debug.joybusTimingErrors, count)],
    ['Response failures', valueOrDash(debug.joybusResponseFailures, count)],
    ['Dispatch reads', valueOrDash(debug.accessoryReads, count)],
    ['Dispatch writes', valueOrDash(debug.accessoryWrites, count)],
    ['Dispatch malformed', valueOrDash(debug.accessoryMalformed, count)]
  ]);
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

async function loadState() {
  const response = await fetch(new URL('api/state', window.location.href), { cache: 'no-store' });
  const state = await response.json();
  latestState = state;
  setStatus(`${state?.network?.ssid || 'espN64'} ${state?.network?.ip || ''}`.trim());
  renderState(state);
  return state;
}

async function websocketPort() {
  if (latestState?.network?.websocketPort) return latestState.network.websocketPort;
  try {
    const state = await loadState();
    return state.network?.websocketPort || Number(window.location.port || 80);
  } catch {
    return Number(window.location.port || 80);
  }
}

async function sendInput(control, pressed) {
  if (sendSocketJson({ type: 'input', control, pressed })) return;
  await fetch(new URL('api/input', window.location.href), {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: `control=${encodeURIComponent(control)}&pressed=${pressed ? '1' : '0'}`
  }).catch(() => {});
}

function pressControl(control) {
  if (releaseTimers.has(control)) {
    clearTimeout(releaseTimers.get(control));
    releaseTimers.delete(control);
  }
  setHeld(control, true);
  if (heldIntervals.has(control)) clearInterval(heldIntervals.get(control));
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
  if (releaseTimers.has(control)) clearTimeout(releaseTimers.get(control));
  const timer = setTimeout(() => {
    sendInput(control, false);
    releaseTimers.delete(control);
  }, 80);
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
  if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
    socket.close();
  }

  const port = await websocketPort();
  const protocol = window.location.protocol === 'https:' ? 'wss' : 'ws';
  const nextSocket = new WebSocket(`${protocol}://${window.location.hostname}:${port}/ws`);
  nextSocket.binaryType = 'arraybuffer';
  socket = nextSocket;

  const openTimeout = window.setTimeout(() => {
    if (nextSocket.readyState === WebSocket.CONNECTING) nextSocket.close();
  }, WS_CONNECT_TIMEOUT_MS);

  nextSocket.addEventListener('open', () => {
    window.clearTimeout(openTimeout);
    reconnectAttempts = 0;
    setSocketConnected(true);
    setStatus('connected');
    sendSocketJson({ type: 'state' });
    loadState().catch(() => {});
  });

  nextSocket.addEventListener('message', (event) => {
    if (typeof event.data !== 'string') return;
    try {
      latestState = JSON.parse(event.data);
      renderState(latestState);
    } catch {
      // Ignore malformed messages.
    }
  });

  nextSocket.addEventListener('close', () => {
    window.clearTimeout(openTimeout);
    setSocketConnected(false);
    setStatus('reconnecting');
    if (socket === nextSocket) socket = null;
    scheduleReconnect();
  });

  nextSocket.addEventListener('error', () => {
    setSocketConnected(false);
    try {
      nextSocket.close();
    } catch {
      // Ignore close failures; reconnect handles recovery.
    }
  });
}

function onKeyDown(event) {
  const control = keyMap[event.code];
  if (!control || activeKeys.has(event.code)) return;
  activeKeys.add(event.code);
  pressControl(control);
}

function onKeyUp(event) {
  const control = keyMap[event.code];
  if (!control) return;
  activeKeys.delete(event.code);
  releaseControl(control);
}

controlButtons.forEach((button) => bindPointerHandlers(button));
window.addEventListener('keydown', onKeyDown);
window.addEventListener('keyup', onKeyUp);

renderState({});
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
    navigator.serviceWorker.register(new URL('sw.js', window.location.href).href).catch(() => {});
  });
}
