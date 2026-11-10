// The demo page: hosts the editor (fmvibe-ui, built from web/ui.cpp) on the canvas, passes it the mouse and the
// keyboard as src/gui/editor.cpp does in a plugin window, plays notes from the computer's keyboard, and runs the
// synth in an AudioWorklet (worklet.js) once the browser lets the page make sound.

import createFMVibeUI from './fmvibe-ui.js';
import {createCanvas2d} from './canvas2d.js';

const canvas = document.getElementById('editor');
const stage = document.getElementById('stage');
const editorFrame = document.getElementById('frame');
const soundBanner = document.getElementById('sound');
const failedBanner = document.getElementById('failed');
const menu = document.getElementById('menu');
const toast = document.getElementById('toast');
const octaveLabel = document.getElementById('octave');
const velocityLabel = document.getElementById('velocity');
const ctx = canvas.getContext('2d');
const draw = createCanvas2d(ctx);

const CURSORS = ['default', 'pointer', 'ns-resize', 'ew-resize', 'move', 'text']; // gui::CursorKind
const TYPING_KEYS = { // View::Key
  Enter: 0, NumpadEnter: 0, Escape: 1, Backspace: 2, Delete: 3, ArrowLeft: 4, ArrowRight: 5, Home: 6, End: 7,
  ArrowUp: 9, ArrowDown: 10,
};
const SELECT_ALL = 8;

// --- Audio --------------------------------------------------------------------------------------------------

const dspBytes = fetch('fmvibe-dsp.wasm').then((r) => {
  if (!r.ok)
    throw new Error(`fmvibe-dsp.wasm: ${r.status}`);
  return r.arrayBuffer();
});
let audio = null;      // {context, node} once the worklet runs
let starting = false;  // the first click or key starts it
let pending = [];      // edits (index, value, ...) not sent yet
let flushQueued = false;
let scopeAsked = null; // what the scope was last asked for: the editor asks once, maybe before the audio starts

function send(message, transfer) {
  if (audio)
    audio.node.port.postMessage(message, transfer ?? []);
}

// Edits go over once per task, so a drag's or a preset's many changes are one message.
function flush() {
  flushQueued = false;
  if (pending.length > 0) {
    const changes = new Float64Array(pending);
    pending = [];
    send({type: 'params', changes}, [changes.buffer]);
  }
}

async function startAudio() {
  if (audio) {
    if (audio.context.state === 'suspended')
      audio.context.resume();
    return;
  }
  if (starting)
    return;
  starting = true;
  try {
    // Made during the click or key press, so the browser lets it play.
    const context = new AudioContext({latencyHint: 'interactive'});
    const [wasm] = await Promise.all([dspBytes, context.audioWorklet.addModule('worklet.js')]);
    const count = ui._ui_param_count();
    const values = new Float64Array(ui.HEAPF64.buffer, ui._ui_values(), count).slice();
    const node = new AudioWorkletNode(context, 'fmvibe', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: {wasm, values, oversampling: ui._ui_oversampling(), tempo},
    });
    node.port.onmessage = (e) => fromAudio(e.data);
    node.onprocessorerror = () => fail('The synth stopped with an error; reload the page to start it again.');
    node.connect(context.destination);
    pending = []; // the values sent hold them already
    audio = {context, node};
    if (scopeAsked)
      send(scopeAsked);
    window.fmvibe = {context, node, ui}; // for poking around in the console
    if (context.state === 'suspended')
      await context.resume();
    soundBanner.hidden = true;
  } catch (error) {
    console.error(error);
    fail('The sound could not start in this browser.');
  }
}

function fromAudio(message) {
  if (message.type === 'telemetry') {
    // The struct's bytes, as the audio side's module laid them out; both modules come from the same header. A
    // stale cached module from another build may lay out more: never more than the editor's struct holds.
    const size = Math.min(message.buffer.byteLength, ui._ui_telemetry_size());
    ui.HEAPU8.set(new Uint8Array(message.buffer, 0, size), ui._ui_telemetry_buffer());
    ui._ui_telemetry_stamp(performance.now());
    send({type: 'buffer', buffer: message.buffer}, [message.buffer]);
  } else if (message.type === 'ready') {
    if (message.telemetrySize !== ui._ui_telemetry_size() || message.paramCount !== ui._ui_param_count())
      console.error('The audio and editor modules disagree; were they built together?', message);
  }
}

function fail(text) {
  soundBanner.hidden = true;
  failedBanner.textContent = text;
  failedBanner.hidden = false;
}

// --- The editor ---------------------------------------------------------------------------------------------

let toastTimer = 0;
function showToast(text) {
  toast.textContent = text;
  toast.classList.add('shown');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('shown'), 4500);
}

// What the editor asks of the page (ui.cpp's js_* functions).
const host = {
  param(index, value) {
    if (!audio)
      return; // the values sent at the start will hold it
    pending.push(index, value);
    if (!flushQueued) {
      flushQueued = true;
      queueMicrotask(flush);
    }
  },
  allParams(values) {
    pending = []; // all of them, as they are now
    send({type: 'all', values}, [values.buffer]);
  },
  note(key, on, velocity = 100) { // the editor's keyboard plays at 100, as the plugin's does
    flush(); // after the edits made before it
    send({type: 'note', key, on, velocity});
  },
  bend(amount) {
    send({type: 'bend', amount});
  },
  scope(op, cycles) {
    scopeAsked = {type: 'scope', op, cycles};
    send(scopeAsked);
  },
  oversampling(factor) {
    flush();
    send({type: 'oversampling', factor});
  },
  tempo(bpm) {
    setTempo(bpm);
  },
  layoutChanged() {
    resize();
  },
  toast: showToast,
  setting(key) {
    try {
      return localStorage.getItem('fmvibe.' + key);
    } catch {
      return null;
    }
  },
  setSetting(key, value) {
    try {
      localStorage.setItem('fmvibe.' + key, value);
    } catch {
      // private windows and blocked storage: the setting lasts until the page is closed
    }
  },
};

let ui;
let dirty = true;
let viewWidth = 1;  // the editor's size in its own units; resize() asks it
let viewHeight = 1;
let paintQueued = false; // resize() has a paint coming
try {
  ui = await createFMVibeUI({fmv2d: draw, fmvHost: host});
  ui._ui_init();
} catch (error) {
  console.error(error);
  fail('The editor could not start in this browser.');
  throw error;
}

// As wide as the page allows, keeping the editor's shape and fitting the window's height (the plugin's
// window goes from 600 to 3600 pixels wide); sharp on high-density screens.
function resize() {
  viewWidth = ui._ui_width();
  viewHeight = ui._ui_height();
  const room = stage.clientWidth - 32;
  const below = document.querySelector('footer').offsetHeight + 34;
  const roomHeight = window.innerHeight - editorFrame.getBoundingClientRect().top - window.scrollY - below;
  const width = Math.max(300, Math.min(room, Math.max(roomHeight, 400) * viewWidth / viewHeight, 1800));
  const height = width * viewHeight / viewWidth;
  canvas.style.width = `${width}px`;
  canvas.style.height = `${height}px`;
  // Setting the canvas's size clears it, even to the size it has: only when it changes (every layout change comes
  // here, most keeping the editor's height), and then painted before the display shows it blank, which frame() may
  // not do on a fast display. Once the editor is done with what it was doing: it may have asked for this mid-edit.
  const ratio = window.devicePixelRatio || 1;
  const pixelsWide = Math.round(width * ratio);
  const pixelsHigh = Math.round(height * ratio);
  if (canvas.width !== pixelsWide || canvas.height !== pixelsHigh) {
    canvas.width = pixelsWide;
    canvas.height = pixelsHigh;
    draw.invalidate();
    if (!paintQueued) {
      paintQueued = true;
      queueMicrotask(() => {
        paintQueued = false;
        paint();
      });
    }
  }
  dirty = true;
}

function paint() {
  dirty = false;
  const scale = canvas.width / viewWidth;
  ctx.setTransform(scale, 0, 0, scale, 0, 0);
  ui._ui_paint();
}

// About sixty polls a second, as the plugin window's timer, whatever the display's rate: a frame of the display
// polls once about a sixtieth of a second has gone by since the last poll, the time short of it or past it carried
// over, so a 120 Hz display polls every other frame and a 144 Hz one 60 times a second all the same. Frames come
// when something moved.
const POLL_MS = 1000 / 60;
let lastFrame = 0; // when the display's last frame came
let owed = 0;      // ms since the last poll, with what was carried over
function frame(now) {
  requestAnimationFrame(frame);
  owed += now - lastFrame;
  lastFrame = now;
  if (owed < POLL_MS - 4) // a 60 Hz display's frames come a little early at times
    return;
  owed = Math.min(owed - POLL_MS, POLL_MS / 2); // after a long wait (a hidden page), no burst of polls
  if (ui._ui_poll())
    dirty = true;
  if (dirty)
    paint();
}

function viewPoint(e) {
  const r = canvas.getBoundingClientRect();
  return [(e.clientX - r.left) * viewWidth / r.width, (e.clientY - r.top) * viewHeight / r.height];
}

// Ctrl on a Mac is the right button, so Command stands in for it there.
function ctrlOf(e) {
  return e.ctrlKey || e.metaKey;
}

let cursor = null; // as last set on the canvas
// `changed`: whether what the editor shows changed, so it wants a paint (a mouse move may change nothing).
function afterInput(changed = true) {
  const now = CURSORS[ui._ui_cursor()] ?? 'default';
  if (now !== cursor) {
    canvas.style.cursor = now;
    cursor = now;
  }
  if (changed)
    dirty = true;
}

// --- Mouse --------------------------------------------------------------------------------------------------

let dragging = false; // the left button went down on the editor: it gets the moves and the release, wherever
let panning = false;  // and the same for the middle button, which drags a panel that scrolls
let lastPoint = [0, 0];

canvas.addEventListener('mousedown', (e) => {
  if (e.button === 1) { // not the browser's own scrolling, nor a paste
    e.preventDefault();
    closeMenu();
    panning = true;
    const [x, y] = lastPoint = viewPoint(e);
    ui._ui_middle_down(x, y);
    afterInput();
    return;
  }
  if (e.button !== 0)
    return;
  e.preventDefault();
  closeMenu();
  canvas.focus({preventScroll: true});
  startAudio();
  dragging = true;
  const [x, y] = lastPoint = viewPoint(e);
  // The second press of a double-click is one, as in Windows' WM_LBUTTONDBLCLK.
  if (e.detail === 2)
    ui._ui_double_click(x, y, e.shiftKey, ctrlOf(e));
  else
    ui._ui_mouse_down(x, y, e.shiftKey, ctrlOf(e));
  afterInput();
});

window.addEventListener('mousemove', (e) => {
  if (!dragging && !panning && e.target !== canvas)
    return;
  const [x, y] = lastPoint = viewPoint(e);
  afterInput(ui._ui_mouse_move(x, y, e.shiftKey, ctrlOf(e)) !== 0);
});

window.addEventListener('mouseup', (e) => {
  if (e.button === 1 && panning) {
    panning = false;
    const [x, y] = viewPoint(e);
    ui._ui_middle_up(x, y);
    if (e.target !== canvas && !dragging)
      ui._ui_mouse_leave();
    afterInput();
    return;
  }
  if (e.button !== 0 || !dragging)
    return;
  dragging = false;
  const [x, y] = viewPoint(e);
  ui._ui_mouse_up(x, y, e.shiftKey, ctrlOf(e));
  if (e.target !== canvas)
    ui._ui_mouse_leave();
  afterInput();
});

canvas.addEventListener('mouseleave', () => {
  if (dragging || panning)
    return;
  ui._ui_mouse_leave();
  afterInput();
});

// A wheel's delta in notches, down (or to the right) for more than 0.
function notchesOf(e, delta) {
  return e.deltaMode === 1 ? delta / 3 : e.deltaMode === 2 ? delta : delta / 100;
}

canvas.addEventListener('wheel', (e) => {
  e.preventDefault(); // neither the page scrolls nor the browser zooms
  // Browsers turn Shift+wheel sideways, which stays the wheel turned (with Shift, finely). Without Shift, a sideways
  // part (a touchpad swiped across, a tilted wheel) scrolls what scrolls sideways.
  const delta = e.deltaY !== 0 ? e.deltaY : e.shiftKey ? e.deltaX : 0;
  const across = e.shiftKey ? 0 : e.deltaX;
  if (delta === 0 && across === 0)
    return;
  const [x, y] = viewPoint(e);
  if (delta !== 0)
    ui._ui_wheel(x, y, -notchesOf(e, delta), e.shiftKey, ctrlOf(e));
  if (across !== 0)
    ui._ui_wheel_across(x, y, notchesOf(e, across));
  afterInput();
}, {passive: false});

// --- Touch --------------------------------------------------------------------------------------------------

// One finger works the editor: on something that drags (a knob, a card's title, a handle) it drags it, elsewhere
// over a panel that scrolls (the effects, the source cards, the MODS view's rows, a list) it pans it, and a tap is a
// click, two quick ones a double-click (see View::touchDown()). Two fingers pan what scrolls between them, as the
// middle button drags it. A long press opens the menu a right-click would.
const TOUCH_SLOP = 10;         // CSS pixels a finger may jitter and still tap
const DOUBLE_TAP_MS = 400;
let touchMode = null;          // 'one', 'two', or 'done' until the fingers left over are lifted
let touchId = null;            // the one finger's
let touchStart = [0, 0];       // where it landed, in CSS pixels
let touchMoved = false;
let touchPoint = [0, 0];       // where it, or the middle of two, last was, in the editor's units
let lastTap = null;            // {time, x, y}: the tap before, for a double one

function touchMiddle(e) {
  const [a, b] = [e.touches[0], e.touches[1]];
  return viewPoint({clientX: (a.clientX + b.clientX) / 2, clientY: (a.clientY + b.clientY) / 2});
}

function touchOf(list) {
  for (const t of list)
    if (t.identifier === touchId)
      return t;
  return null;
}

canvas.addEventListener('touchstart', (e) => {
  closeMenu();
  if (touchMode === null && e.touches.length === 1) {
    const t = e.changedTouches[0];
    touchId = t.identifier;
    touchStart = [t.clientX, t.clientY];
    touchMoved = false;
    const [x, y] = touchPoint = viewPoint(t);
    const second = lastTap !== null && performance.now() - lastTap.time < DOUBLE_TAP_MS &&
                   Math.hypot(x - lastTap.x, y - lastTap.y) < 2 * TOUCH_SLOP * viewWidth / canvas.getBoundingClientRect().width;
    const slop = TOUCH_SLOP * viewWidth / canvas.getBoundingClientRect().width;
    touchMode = 'one';
    ui._ui_touch_down(x, y, second ? 1 : 0, slop);
    afterInput();
    return;
  }
  if (e.touches.length >= 2 && touchMode !== 'two' && touchMode !== 'done') {
    e.preventDefault(); // no mouse events made up from it
    touchMode = 'two';
    lastTap = null;
    const [x, y] = touchPoint = touchMiddle(e);
    ui._ui_touch_pan_begin(x, y);
    afterInput();
  }
}, {passive: false});

canvas.addEventListener('touchmove', (e) => {
  if (touchMode === null)
    return;
  e.preventDefault();
  if (touchMode === 'one') {
    const t = touchOf(e.touches);
    if (!t)
      return;
    touchMoved ||= Math.hypot(t.clientX - touchStart[0], t.clientY - touchStart[1]) > TOUCH_SLOP;
    const [x, y] = touchPoint = viewPoint(t);
    afterInput(ui._ui_touch_move(x, y) !== 0);
  } else if (touchMode === 'two' && e.touches.length >= 2) {
    const [x, y] = touchPoint = touchMiddle(e);
    afterInput(ui._ui_touch_pan_move(x, y) !== 0);
  }
}, {passive: false});

canvas.addEventListener('touchend', (e) => {
  if (touchMode === 'one' && touchOf(e.changedTouches)) {
    e.preventDefault(); // no mouse events made up from it: the editor has it already
    canvas.focus({preventScroll: true});
    startAudio();
    ui._ui_touch_up(touchPoint[0], touchPoint[1]);
    lastTap = touchMoved ? null : {time: performance.now(), x: touchPoint[0], y: touchPoint[1]};
    afterInput();
    touchMode = e.touches.length > 0 ? 'done' : null;
  } else if (touchMode === 'two' && e.touches.length < 2) {
    ui._ui_touch_pan_end(touchPoint[0], touchPoint[1]);
    afterInput();
    touchMode = e.touches.length > 0 ? 'done' : null;
  } else if (e.touches.length === 0) {
    touchMode = null;
  }
}, {passive: false});

canvas.addEventListener('touchcancel', (e) => {
  if (touchMode === 'one')
    ui._ui_touch_cancel();
  else if (touchMode === 'two')
    ui._ui_touch_pan_end(touchPoint[0], touchPoint[1]);
  afterInput();
  touchMode = e.touches.length > 0 ? 'done' : null;
});

// --- Context menu -------------------------------------------------------------------------------------------

canvas.addEventListener('contextmenu', (e) => {
  e.preventDefault();
  if (dragging)
    return;
  if (touchMode === 'one') { // a long press: its menu, and nothing more from that finger
    ui._ui_touch_cancel();
    touchMode = 'done';
    lastTap = null;
  }
  closeMenu();
  const [x, y] = viewPoint(e);
  const count = ui._ui_context_menu(x, y);
  if (count === 0)
    return;
  for (let i = 0; i < count; ++i) {
    const command = ui._ui_menu_command(i);
    if (command === 0) {
      menu.append(document.createElement('hr'));
      continue;
    }
    const item = document.createElement('div');
    const checked = ui._ui_menu_checked(i) !== 0;
    item.setAttribute('role', checked ? 'menuitemcheckbox' : 'menuitem');
    if (checked)
      item.setAttribute('aria-checked', 'true');
    item.textContent = (checked ? '✓ ' : '') + ui.UTF8ToString(ui._ui_menu_label(i));
    item.addEventListener('click', () => {
      closeMenu();
      ui._ui_menu_pick(command);
      afterInput();
    });
    menu.append(item);
  }
  menu.hidden = false;
  const left = Math.min(e.clientX, window.innerWidth - menu.offsetWidth - 4);
  const top = Math.min(e.clientY, window.innerHeight - menu.offsetHeight - 4);
  menu.style.left = `${Math.max(4, left)}px`;
  menu.style.top = `${Math.max(4, top)}px`;
  afterInput();
});

function closeMenu() {
  menu.hidden = true;
  menu.replaceChildren();
}

document.addEventListener('mousedown', (e) => {
  if (!menu.hidden && !menu.contains(e.target))
    closeMenu();
  // Clicking out of the editor counts what was typed, as the plugin does when the host takes the keyboard.
  if (e.target !== canvas && ui._ui_typing()) {
    ui._ui_end_typing(1);
    afterInput();
  }
});

// --- Tempo --------------------------------------------------------------------------------------------------

// The page stands in for a host's transport: its tempo is what synced LFOs, delays and sweeps follow. The editor's
// title bar shows it, to drag, scroll or type (ui.cpp hands it here), and it is kept in the browser's storage like
// the theme, which the editor reads it from too.
const TEMPO_MIN = 20;
const TEMPO_MAX = 999;
let tempo = 140;

function clampTempo(bpm) {
  return Math.min(TEMPO_MAX, Math.max(TEMPO_MIN, Math.round(bpm * 100) / 100));
}

function setTempo(bpm) {
  if (!Number.isFinite(bpm))
    return;
  tempo = clampTempo(bpm);
  send({type: 'tempo', bpm: tempo});
  host.setSetting('tempo', String(tempo));
}

{
  const stored = parseFloat(host.setting('tempo') ?? '');
  if (Number.isFinite(stored))
    tempo = clampTempo(stored);
}

// --- Keyboard -----------------------------------------------------------------------------------------------

// The computer keyboard's piano: by key position, so it works on any layout. A is C, W the C# above it...
const PIANO = {
  KeyA: 0, KeyW: 1, KeyS: 2, KeyE: 3, KeyD: 4, KeyF: 5, KeyT: 6, KeyG: 7, KeyY: 8, KeyH: 9, KeyU: 10, KeyJ: 11,
  KeyK: 12, KeyO: 13, KeyL: 14, KeyP: 15, Semicolon: 16,
};
const held = new Map(); // key code -> the note it plays
let octave = 0;         // from middle C
const VELOCITIES = [1, 20, 40, 60, 80, 100, 120, 127]; // C and V step through them
let velocity = 5;       // in VELOCITIES

function showKeys() {
  octaveLabel.textContent = `C${4 + octave}`;
  velocityLabel.textContent = `${VELOCITIES[velocity]}`;
}

function releaseAll() {
  for (const note of held.values())
    host.note(note, false);
  held.clear();
}

// The message over the editor: a click on it, or any key, turns the sound on.
soundBanner.addEventListener('click', () => {
  canvas.focus({preventScroll: true});
  startAudio();
});

const MODIFIERS = new Set(['Shift', 'Control', 'Alt', 'Meta', 'AltGraph', 'CapsLock', 'Escape']);

window.addEventListener('keydown', (e) => {
  if (e.key === 'Control' && ui._ui_ctrl(1) !== 0) // a graph's grid shows while Ctrl is held
    afterInput(true);
  if (!audio && !MODIFIERS.has(e.key))
    startAudio();
  if (!menu.hidden && e.key === 'Escape') {
    closeMenu();
    return;
  }
  // A dialog over the editor takes Enter, which confirms it, and Escape, which cancels it.
  if (!ui._ui_typing() && ui._ui_dialog_open() && (e.key === 'Enter' || e.key === 'Escape')) {
    e.preventDefault();
    ui._ui_dialog_key(TYPING_KEYS[e.key]);
    afterInput();
    return;
  }
  // While a value is typed, every key goes to it; copying, cutting and pasting come as their own events.
  if (ui._ui_typing()) {
    if (ctrlOf(e) && !e.altKey && (e.code === 'KeyC' || e.code === 'KeyX' || e.code === 'KeyV'))
      return;
    e.preventDefault();
    if (ctrlOf(e) && e.code === 'KeyA')
      ui._ui_type_key(SELECT_ALL, 0);
    else if (e.code in TYPING_KEYS || e.key in TYPING_KEYS)
      ui._ui_type_key(TYPING_KEYS[e.code] ?? TYPING_KEYS[e.key], e.shiftKey ? 1 : 0);
    else if (!ctrlOf(e) && !e.altKey && [...e.key].length === 1)
      ui._ui_type_char(e.key.codePointAt(0));
    afterInput();
    return;
  }
  if (ctrlOf(e) && !e.altKey && (e.code === 'KeyZ' || e.code === 'KeyY')) {
    e.preventDefault();
    if (e.code === 'KeyY' || e.shiftKey)
      ui._ui_redo();
    else
      ui._ui_undo();
    afterInput();
    return;
  }
  if (ctrlOf(e) || e.altKey)
    return;
  if (e.code in PIANO) {
    e.preventDefault();
    startAudio();
    if (e.repeat || held.has(e.code))
      return;
    const note = 60 + 12 * octave + PIANO[e.code];
    held.set(e.code, note);
    host.note(note, true, VELOCITIES[velocity]);
  } else if (e.code === 'KeyZ' || e.code === 'KeyX') {
    e.preventDefault();
    startAudio();
    if (!e.repeat) {
      octave = Math.max(-4, Math.min(4, octave + (e.code === 'KeyX' ? 1 : -1)));
      showKeys();
    }
  } else if (e.code === 'KeyC' || e.code === 'KeyV') {
    e.preventDefault();
    startAudio();
    if (!e.repeat) {
      velocity = Math.max(0, Math.min(VELOCITIES.length - 1, velocity + (e.code === 'KeyV' ? 1 : -1)));
      showKeys();
    }
  }
});

// The clipboard, while a value or a search is typed.
for (const kind of ['copy', 'cut']) {
  document.addEventListener(kind, (e) => {
    if (!ui._ui_typing())
      return;
    e.preventDefault();
    const text = ui.UTF8ToString(ui._ui_copy(kind === 'cut' ? 1 : 0));
    if (text)
      e.clipboardData.setData('text/plain', text);
    afterInput();
  });
}
document.addEventListener('paste', (e) => {
  if (!ui._ui_typing())
    return;
  e.preventDefault();
  for (const ch of e.clipboardData.getData('text/plain').replace(/\s/g, ' '))
    ui._ui_type_char(ch.codePointAt(0));
  afterInput();
});

window.addEventListener('keyup', (e) => {
  if (e.key === 'Control' && ui._ui_ctrl(0) !== 0)
    afterInput(true);
  const note = held.get(e.code);
  if (note !== undefined) {
    held.delete(e.code);
    host.note(note, false);
  }
  // A Mac sends no keyup for keys let go while Command is down: none may hang once Command is let go too.
  if (e.key === 'Meta')
    releaseAll();
});

// No key may hang when the page loses the keyboard; what was being typed counts.
window.addEventListener('blur', () => {
  releaseAll();
  if (ui._ui_ctrl(0) !== 0) // no keyup comes once the page has lost the keyboard
    afterInput(true);
  if (ui._ui_typing()) {
    ui._ui_end_typing(1);
    afterInput();
  }
  // The button may come up elsewhere: the drag ends where it is, dropping nothing, as on WM_CAPTURECHANGED.
  if (dragging || panning) {
    dragging = panning = false;
    ui._ui_cancel_drag();
    ui._ui_mouse_leave();
    afterInput();
  }
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden)
    releaseAll();
});

window.addEventListener('resize', resize);
// Moved to a screen of another pixel density, the page may keep its size: watch the density itself.
function watchDensity() {
  matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`).addEventListener('change', () => {
    resize();
    watchDensity();
  }, {once: true});
}
watchDensity();
showKeys();
resize();
requestAnimationFrame(frame);
