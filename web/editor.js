/* The creature editor.
 *
 * C owns the genome, prices it and turns it into cones; gl.js marches those
 * cones; genome.js knows the byte layout and the mounting maths; this file is
 * the hands. Every gesture ends as a byte written into the genome, and the
 * next frame is built from those bytes - there is no editor-side model of a
 * creature that could disagree with the one the simulation will run.
 */

const VIS_TERRA = 5;
const PIXEL_W = 480, PIXEL_H = 340;

const view = document.getElementById("view");
const overlay = document.getElementById("overlay");
const octx = overlay.getContext("2d");
const frame = document.getElementById("frame");

let M = null;      // the wasm module
let R = null;      // the GL viewport
let budget = 0, seed = 7;
let az = 2.30, el = 0.26, zoom = 1;
let animating = true, pixel = true;
let phase = 0, lastT = 0, fps = 0;
let W = 0, H = 0, cssW = 0, cssH = 0;

// what the last frame built, kept for picking and handles
let pose = null, prims = null, primCount = 0, cam = null, gene = null;

let sel = -1;          // selected part
let armed = 0;         // a part type waiting to be placed
let hover = null;      // handle under the cursor
let drag = null;
let flash = 0;
const refreshers = [];  // slider -> genome sync, run when the genome changes elsewhere

/* A creature is 157 bytes, so remembering every state you passed through costs
 * nothing - and an editor without undo makes you cautious, which is the
 * opposite of what this screen is for. */
const history = [];

Cpore().then((mod) => {
  M = mod;
  if (M._cpw_genome_bytes() !== G.BYTES) {
    throw new Error(`genome layout moved: C says ${M._cpw_genome_bytes()} bytes, JS says ${G.BYTES}`);
  }
  budget = M._cpw_budget();
  M._cpw_autodesign(1, budget, seed);

  R = makeGLView(view);
  const n = M._cpw_palette_load(VIS_TERRA);
  R.palette(new Uint8Array(M.HEAPU8.buffer, M._cpw_palette(), n * 3), n, M._cpw_dither());

  buildRails();
  fit();
  window.addEventListener("resize", fit);
  requestAnimationFrame(loop);
});

function geneBytes() {
  return new Uint8Array(M.HEAPU8.buffer, M._cpw_genome(), G.BYTES);
}

function snap() {
  history.push(geneBytes().slice());
  if (history.length > 200) history.shift();
}

function undo() {
  const prev = history.pop();
  if (!prev) return;
  geneBytes().set(prev);
  sel = -1;
  changed();
}

/* everything that has to catch up after the genome moves under it */
function changed() {
  refreshers.forEach((f) => f());
  refreshPalette();
  buildInspector();
  hint();
}

function fit() {
  const availW = Math.max(320, window.innerWidth - 460);
  const availH = Math.max(240, window.innerHeight - 170);
  if (pixel) {
    const s = Math.max(1, Math.floor(Math.min(availW / PIXEL_W, availH / PIXEL_H)));
    W = PIXEL_W; H = PIXEL_H;
    cssW = W * s; cssH = H * s;
  } else {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    cssW = availW; cssH = availH;
    W = Math.min(Math.round(cssW * dpr), 1800);
    H = Math.min(Math.round(cssH * dpr), 1300);
  }
  view.style.width = cssW + "px";
  view.style.height = cssH + "px";
  view.style.imageRendering = pixel ? "pixelated" : "auto";
  overlay.width = cssW; overlay.height = cssH;
  overlay.style.width = cssW + "px";
  overlay.style.height = cssH + "px";
}

function loop(t) {
  const dt = lastT ? Math.min((t - lastT) / 1000, 0.1) : 0;
  lastT = t;
  /* The gait stops the moment you reach for the animal. Handles that drift
   * while you aim at them is most of what made this feel clumsy: you were
   * grabbing at a moving target. */
  const working = !!drag || !!hover || !!armed;
  if (animating && !working) phase += dt * 4.0;
  if (flash > 0) flash = Math.max(0, flash - dt);

  primCount = M._cpw_build(phase);
  prims = new Float32Array(M.HEAPU8.buffer, M._cpw_prims(), M._cpw_max_prims() * 16);
  const meta = new Float32Array(M.HEAPU8.buffer, M._cpw_meta(), 56);
  gene = geneBytes();
  pose = readPose(meta);

  R.upload(prims, primCount);
  R.skin(meta);
  cam = R.camera(pose.centre, pose.bound, az, el, zoom);
  R.draw(W, H, animating && !working ? 1 : 2, pixel, seed % 128);
  drawHandles();

  if (dt > 0) fps += (1 / dt - fps) * 0.1;
  document.getElementById("perf").textContent =
    `${W}x${H}  ${primCount} cones  ${fps.toFixed(0)} fps`;
  readout();
  requestAnimationFrame(loop);
}

/* ---- screen space ---- */

function project(p) {
  const d = vsub(p, cam.eye);
  const vz = vdot(d, cam.fwd);
  if (vz < 0.2) return null;
  const focal = W * 1.15;
  const sx = W * 0.5 + (focal * vdot(d, cam.right)) / vz;
  const sy = H * 0.5 - (focal * vdot(d, cam.up)) / vz;
  return [(sx * cssW) / W, (sy * cssH) / H, vz];
}

function rayThrough(cssX, cssY) {
  const focal = W * 1.15;
  const px = ((cssX * W) / cssW - W * 0.5) / focal;
  const py = -(((cssY * H) / cssH - H * 0.5) / focal);
  return vnorm(vadd(vadd(vmul(cam.right, px), vmul(cam.up, py)), cam.fwd));
}

function handles() {
  const out = [];
  for (let i = 0; i < MAX_PARTS; i++) {
    const o = G.PART + i * G.PART_STRIDE;
    const type = gene[o + P.TYPE];
    if (!type) continue;
    const copies = gene[o + P.MIRROR] ? 2 : 1;
    for (let c = 0; c < copies; c++) {
      const m = partMount(gene, pose, i, c);
      if (!m) continue;
      const s = project(m);
      if (!s) continue;
      /* Is the animal itself in the way? A handle on the far flank drawn as
       * solidly as one on the near flank makes the body read as flat, and you
       * end up grabbing a leg you cannot see. */
      const q = rayHitBody(prims, primCount, pose, cam.eye, vnorm(vsub(m, cam.eye)));
      const behind = q ? vlen(vsub(q, cam.eye)) < s[2] - 0.6 : false;
      out.push({ part: i, copy: c, type, x: s[0], y: s[1], z: s[2], behind });
    }
  }
  return out;
}

function drawHandles() {
  octx.clearRect(0, 0, cssW, cssH);
  if (!cam) return;

  if (armed || drag) {
    octx.strokeStyle = "rgba(255,255,255,0.30)";
    octx.lineWidth = 1;
    octx.beginPath();
    pose.spine.forEach((st, i) => {
      const s = project(st.pos);
      if (!s) return;
      i ? octx.lineTo(s[0], s[1]) : octx.moveTo(s[0], s[1]);
    });
    octx.stroke();
  }

  for (const h of handles()) {
    const on = h.part === sel;
    const hot = hover && hover.part === h.part && hover.copy === h.copy;
    octx.beginPath();
    octx.arc(h.x, h.y, (on ? 6 : 4.5) * (h.behind ? 0.72 : 1), 0, 6.2832);
    octx.fillStyle = on ? "rgba(255,246,214,0.95)"
                        : hot ? "rgba(255,255,255,0.85)"
                        : h.behind ? "rgba(255,255,255,0.14)" : "rgba(255,255,255,0.38)";
    octx.fill();
    if (on || hot) {
      octx.lineWidth = 1.5;
      octx.strokeStyle = "rgba(20,20,20,0.55)";
      octx.stroke();
    }
  }
}

/* ---- editing ---- */

const partName = (type) => M.UTF8ToString(M._cpw_part_name(type));
const partOff = (i) => G.PART + i * G.PART_STRIDE;

function affordable(type, mirrored) {
  return M._cpw_cost() + M._cpw_part_cost(type) * (mirrored ? 2 : 1) <= budget;
}

function placePart(type, q) {
  const mount = pointToMount(pose, q);
  let slot = -1;
  for (let i = 0; i < MAX_PARTS; i++) if (!gene[partOff(i) + P.TYPE]) { slot = i; break; }
  if (slot < 0) { flash = 1.2; setHint("no slots left - remove a part first"); return; }
  if (!affordable(type, true)) { flash = 1.2; setHint(`not enough DNA for a ${partName(type)}`); return; }

  snap();
  const o = partOff(slot);
  gene[o + P.TYPE] = type;
  gene[o + P.SEG] = mount.seg;
  gene[o + P.YAW] = mount.yawByte;
  gene[o + P.PITCH] = mount.pitchByte & 0xff;
  gene[o + P.SCALE] = 128;
  gene[o + P.MIRROR] = 1;          // symmetry on by default, as in Spore
  gene[o + P.LEN] = 128;
  gene[o + P.BEND] = 40;
  M._cpw_normalise(budget);
  sel = slot;
  armed = 0;
  changed();
}

/* Dragging moves a part and only moves it. It used to set the size from how
 * far out you dragged, which meant every reposition also resized - two things
 * happening for one gesture is exactly what "clumsy" feels like. Size is a
 * dial of its own now. */
function dragPart(cssX, cssY) {
  const o = partOff(drag.part);
  const ray = rayThrough(cssX, cssY);
  const seg = Math.min(gene[o + P.SEG], pose.spine.length - 1);
  let q = rayHitBody(prims, primCount, pose, cam.eye, ray);
  if (!q) q = rayNearestTo(cam.eye, ray, pose.spine[seg].pos);

  const mount = pointToMount(pose, q);
  gene[o + P.SEG] = mount.seg;
  // dragging the mirrored copy must not send the original the other way
  gene[o + P.YAW] = drag.copy ? (256 - mount.yawByte) & 0xff : mount.yawByte;
  gene[o + P.PITCH] = mount.pitchByte & 0xff;
}

function removeSelected() {
  if (sel < 0) return;
  snap();
  gene[partOff(sel) + P.TYPE] = 0;
  M._cpw_normalise(budget);
  sel = -1;
  changed();
}

function toggleMirror() {
  if (sel < 0) return;
  const o = partOff(sel);
  if (!gene[o + P.MIRROR] && !affordable(gene[o + P.TYPE], false)) { flash = 1.2; return; }
  snap();
  gene[o + P.MIRROR] = gene[o + P.MIRROR] ? 0 : 1;
  changed();
}

/* ---- pointer ---- */

function localPos(e) {
  const r = view.getBoundingClientRect();
  return [e.clientX - r.left, e.clientY - r.top];
}

function handleAt(x, y) {
  let best = null, bestd = 16 * 16;
  for (const h of handles()) {
    const d = (h.x - x) * (h.x - x) + (h.y - y) * (h.y - y) + (h.behind ? 60 : 0);
    if (d < bestd) { bestd = d; best = h; }
  }
  return best;
}

view.addEventListener("pointerdown", (e) => {
  const [x, y] = localPos(e);
  const h = handleAt(x, y);
  if (h) {
    sel = h.part;
    snap();
    drag = { mode: "part", part: h.part, copy: h.copy };
    buildInspector();
  } else if (armed) {
    const q = rayHitBody(prims, primCount, pose, cam.eye, rayThrough(x, y));
    if (q) placePart(armed, q);
    else setHint("point at the body - a part has to mount on it");
    return;
  } else {
    sel = -1;
    drag = { mode: "orbit", lastX: e.clientX, lastY: e.clientY };
    buildInspector();
  }
  frame.classList.add("dragging");
  view.setPointerCapture(e.pointerId);
  hint();
});

view.addEventListener("pointermove", (e) => {
  const [x, y] = localPos(e);
  if (!drag) {
    hover = handleAt(x, y);
    frame.classList.toggle("over", !!hover || !!armed);
    return;
  }
  if (drag.mode === "part") {
    dragPart(x, y);
    syncInspector();
  } else {
    az -= (e.clientX - drag.lastX) * 0.012;
    el = Math.max(-0.35, Math.min(1.2, el + (e.clientY - drag.lastY) * 0.006));
    drag.lastX = e.clientX; drag.lastY = e.clientY;
  }
});

view.addEventListener("pointerup", (e) => {
  drag = null;
  frame.classList.remove("dragging");
  view.releasePointerCapture(e.pointerId);
  hint();
});

view.addEventListener("pointerleave", () => { hover = null; });

/* The wheel means the obvious thing in both contexts: how big the selected
 * part is, or how close you are standing when nothing is selected. */
view.addEventListener("wheel", (e) => {
  e.preventDefault();
  if (sel >= 0) {
    const o = partOff(sel);
    gene[o + P.SCALE] = clamp(gene[o + P.SCALE] + (e.deltaY > 0 ? -8 : 8), 0, 255);
    syncInspector();
  } else {
    zoom = clamp(zoom * (e.deltaY > 0 ? 1.08 : 0.926), 0.35, 3);
  }
}, { passive: false });

window.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "z") { e.preventDefault(); undo(); return; }
  if (e.target.tagName === "INPUT") return;
  if (e.key === "Delete" || e.key === "Backspace" || e.key === "x") removeSelected();
  else if (e.key === "m") toggleMirror();
  else if (e.key === "Escape") { armed = 0; sel = -1; changed(); }
  hint();
});

/* ---- readouts ---- */

const STAT_NAMES = [
  "speed", "accel", "turn", "jump", "grip", "hp", "armour", "bite", "claw",
  "charm", "voice", "sight", "hearing", "reach", "carry", "stamina", "upkeep",
  "swim", "fly", "dig", "breath", "parts",
];
const SHOWN = ["speed", "jump", "hp", "armour", "bite", "charm", "sight", "reach", "stamina", "swim", "fly", "dig"];

function readout() {
  const cost = M._cpw_cost();
  const dna = document.getElementById("dnatext");
  dna.textContent = `${cost} / ${budget} DNA`;
  dna.style.color = flash > 0 ? "#a8443a" : "";
  const bar = document.querySelector("#meter > i");
  bar.style.width = Math.min(100, (100 * cost) / budget) + "%";
  bar.style.background = flash > 0 ? "#a8443a" : "";

  const st = new Float32Array(M.HEAPF32.buffer, M._cpw_stats(), STAT_NAMES.length);
  const cells = [];
  STAT_NAMES.forEach((name, i) => {
    if (!SHOWN.includes(name)) return;
    const v = st[i];
    if (v <= 0.001) return;
    cells.push(`<span>${name} <b>${v >= 10 ? v.toFixed(0) : v.toFixed(1)}</b></span>`);
  });
  document.getElementById("stats").innerHTML = cells.join("");
}

const setHint = (text) => { document.getElementById("hint").textContent = text; };

function hint() {
  if (armed) return setHint(`click the body to attach a ${partName(armed)}`);
  if (sel >= 0) return setHint(`${partName(gene[partOff(sel) + P.TYPE])} — drag it anywhere on the body · wheel: size · m: mirror · x: remove`);
  setHint("drag a part to move it · drag the background to turn · wheel to zoom · ctrl+z undoes");
}

/* ---- sliders ---- */

function slider(parent, label, min, max, get, set) {
  const row = document.createElement("label");
  row.className = "slider";
  row.innerHTML = `<span><i>${label}</i><b></b></span><input type="range" min="${min}" max="${max}">`;
  const input = row.querySelector("input");
  const num = row.querySelector("b");
  const sync = () => { input.value = get(); num.textContent = get(); };
  input.addEventListener("pointerdown", snap);
  input.addEventListener("input", () => {
    set(+input.value);
    num.textContent = input.value;
    readoutSoon();
  });
  parent.appendChild(row);
  sync();
  return sync;
}

let soon = 0;
function readoutSoon() {
  clearTimeout(soon);
  soon = setTimeout(() => { refreshPalette(); }, 60);
}

/* ---- the panels ---- */

function buildBodyPanel() {
  const body = document.getElementById("body");
  body.innerHTML = "";
  refreshers.length = 0;
  const g = () => geneBytes();
  refreshers.push(slider(body, "segments", 2, 6,
    () => g()[G.NSEG], (v) => { g()[G.NSEG] = v; M._cpw_normalise(budget); }));
  refreshers.push(slider(body, "girth", 40, 255,
    () => g()[G.GIRTH], (v) => { g()[G.GIRTH] = v; }));
  refreshers.push(slider(body, "arch", -127, 127,
    () => s8(g()[G.ARCH]), (v) => { g()[G.ARCH] = v & 0xff; }));
  refreshers.push(slider(body, "sweep", -127, 127,
    () => s8(g()[G.SWEEP]), (v) => { g()[G.SWEEP] = v & 0xff; }));

  const paint = document.getElementById("paint");
  paint.innerHTML = "";
  refreshers.push(slider(paint, "base hue", 0, 255,
    () => g()[G.HUE], (v) => { g()[G.HUE] = v; }));
  refreshers.push(slider(paint, "marking hue", 0, 255,
    () => g()[G.HUE2], (v) => { g()[G.HUE2] = v; }));
  refreshers.push(slider(paint, "detail hue", 0, 255,
    () => g()[G.HUE3], (v) => { g()[G.HUE3] = v; }));
  refreshers.push(slider(paint, "saturation", 0, 255,
    () => g()[G.SAT], (v) => { g()[G.SAT] = v; }));
  refreshers.push(slider(paint, "value", 60, 255,
    () => g()[G.VAL], (v) => { g()[G.VAL] = v; }));
  refreshers.push(slider(paint, "pattern", 0, 7,
    () => g()[G.PATTERN], (v) => { g()[G.PATTERN] = v; }));
  refreshers.push(slider(paint, "pattern scale", 0, 255,
    () => g()[G.PSCALE], (v) => { g()[G.PSCALE] = v; }));
  refreshers.push(slider(paint, "detail pattern", 0, 7,
    () => g()[G.PATTERN2], (v) => { g()[G.PATTERN2] = v; }));
}

let syncInspector = () => {};

function buildInspector() {
  const box = document.getElementById("inspector");
  const label = document.getElementById("sellabel");
  box.innerHTML = "";
  if (sel < 0 || !gene[partOff(sel) + P.TYPE]) {
    label.textContent = "selected";
    box.innerHTML = `<div style="opacity:.45">nothing — click a part</div>`;
    syncInspector = () => {};
    return;
  }
  const o = partOff(sel);
  const type = gene[o + P.TYPE];
  const jointed = JOINTED.has(partName(type));
  label.textContent = partName(type);

  const syncs = [];
  const g = () => geneBytes();
  syncs.push(slider(box, "size", 0, 255, () => g()[o + P.SCALE], (v) => { g()[o + P.SCALE] = v; }));
  if (jointed) {
    syncs.push(slider(box, "length", 0, 255, () => g()[o + P.LEN], (v) => { g()[o + P.LEN] = v; }));
    syncs.push(slider(box, "fold", -127, 127,
      () => s8(g()[o + P.BEND]), (v) => { g()[o + P.BEND] = v & 0xff; }));
  }
  syncs.push(slider(box, "segment", 0, gene[G.NSEG] - 1,
    () => g()[o + P.SEG], (v) => { g()[o + P.SEG] = v; }));

  const mirror = document.createElement("button");
  mirror.style.display = "block";
  mirror.style.width = "100%";
  const label2 = () => (gene[o + P.MIRROR] ? "mirrored — one side only" : "one side — mirror it");
  mirror.textContent = label2();
  mirror.onclick = () => { toggleMirror(); mirror.textContent = label2(); };
  box.appendChild(mirror);

  const del = document.createElement("button");
  del.style.display = "block";
  del.style.width = "100%";
  del.textContent = "remove this part";
  del.onclick = removeSelected;
  box.appendChild(del);

  syncInspector = () => syncs.forEach((f) => f());
}

/* ---- rails ---- */

function refreshPalette() {
  [...document.getElementById("parts").children].forEach((el) => {
    const type = +el.dataset.type;
    const can = M._cpw_cost() + M._cpw_part_cost(type) * 2 <= budget;
    el.classList.toggle("on", armed === type);
    el.style.opacity = can || armed === type ? "" : "0.28";
  });
}

function buildRails() {
  buildBodyPanel();
  buildInspector();

  const styles = document.getElementById("styles");
  for (let s = 0; s < M._cpw_style_count(); s++) {
    const b = document.createElement("button");
    b.textContent = M.UTF8ToString(M._cpw_style_name(s));
    b.style.display = "block";
    b.onclick = () => {
      snap();
      M._cpw_autodesign(s, budget, seed);
      sel = -1; armed = 0;
      [...styles.children].forEach((c) => c.classList.remove("on"));
      b.classList.add("on");
      changed();
    };
    styles.appendChild(b);
  }
  styles.children[1].classList.add("on");

  document.getElementById("random").onclick = () => {
    snap();
    seed = (seed * 1664525 + 1013904223) >>> 0;
    M._cpw_random(budget, seed);
    sel = -1; changed();
  };
  document.getElementById("mutate").onclick = () => {
    snap();
    seed = (seed * 1664525 + 1013904223) >>> 0;
    M._cpw_mutate(budget, seed, 0.5);
    changed();
  };
  document.getElementById("undo").onclick = undo;

  const hold = document.getElementById("hold");
  hold.onclick = () => {
    animating = !animating;
    hold.textContent = animating ? "hold still" : "let it move";
  };
  const look = document.getElementById("look");
  look.onclick = () => {
    pixel = !pixel;
    look.textContent = pixel ? "smooth" : "pixel art";
    fit();
  };

  const parts = document.getElementById("parts");
  parts.innerHTML = "";
  for (let t = 1; t < M._cpw_part_count(); t++) {
    const b = document.createElement("button");
    b.dataset.type = t;
    b.style.display = "block";
    b.style.width = "100%";
    b.innerHTML = `${M.UTF8ToString(M._cpw_part_name(t))} <b>${M._cpw_part_cost(t)}</b>`;
    b.onclick = () => { armed = armed === t ? 0 : t; refreshPalette(); hint(); };
    parts.appendChild(b);
  }
  refreshPalette();
  hint();
}
