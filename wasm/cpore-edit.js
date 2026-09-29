/* The browser side of the creature editor.
 *
 * The module underneath is cpore's own C, compiled to wasm32 by clang with no
 * toolchain runtime under it: the six transcendentals it cannot express as
 * WebAssembly instructions are imported from here, because a browser already
 * has a correctly rounded math library and compiling a second one into the
 * module would be taking on a dependency to avoid using one that is already
 * present.
 *
 * One hazard dominates this file. Growing WebAssembly's linear memory
 * *detaches* every ArrayBuffer view onto it, and the editor grows memory
 * whenever a session opens or a frame allocates. A view cached across such a
 * call throws on next use, or - worse, in a build without bounds checks -
 * reads a buffer that no longer means anything. So nothing here holds a view:
 * `heap()` remakes one on demand, and every call that crosses into C is
 * assumed to have invalidated whatever was held before it.
 */

const MATH_IMPORTS = {
  sinf: Math.sin, cosf: Math.cos, tanf: Math.tan,
  asinf: Math.asin, acosf: Math.acos, atanf: Math.atan,
  atan2f: Math.atan2, powf: Math.pow, expf: Math.exp, logf: Math.log,
  fmodf: (a, b) => a % b,
};

export const PARTS = [
  'none', 'graze', 'jaw', 'beak', 'leg', 'foot', 'claw', 'horn', 'plate',
  'eye', 'ear', 'voice', 'plume', 'wing', 'fin', 'gill', 'digger', 'arm',
  'tail',
];

export const PART_COST = [0, 6, 14, 18, 9, 7, 12, 13, 13, 7, 8, 12, 11, 16,
                          9, 11, 12, 13, 10];

export const PATTERNS = ['plain', 'bands', 'spots', 'counter', 'stripes',
                         'mottle', 'gradient', 'rings'];

export const STYLES = ['grazer', 'predator', 'charmer', 'swimmer', 'flyer',
                       'burrower'];

export const STAT_NAMES = ['speed', 'accel', 'turn', 'jump', 'grip', 'hp',
  'armor', 'bite', 'claw', 'graze', 'carn', 'sight', 'hearing', 'charm',
  'reach', 'carry', 'stamina', 'swim', 'fly', 'dig'];

const MAX_PARTS = 16;
const PART_FIELDS = 8;
const MAX_SEG = 16;

export class CreatureEditor {
  /* `source` is a URL, an ArrayBuffer or a Response - whatever the caller has.
   * Kept async because instantiation is, and pretending otherwise would only
   * push the await somewhere less obvious. */
  static async create(source, { width = 512, height = 512, budget = 248 } = {}) {
    let bytes;
    if (source instanceof ArrayBuffer) bytes = source;
    else if (source instanceof Uint8Array) bytes = source.buffer;
    else bytes = await (await fetch(source)).arrayBuffer();

    const { instance } = await WebAssembly.instantiate(bytes, { env: MATH_IMPORTS });
    return new CreatureEditor(instance, width, height, budget);
  }

  constructor(instance, width, height, budget) {
    this.x = instance.exports;
    this.width = width;
    this.height = height;
    this.handle = this.x.cp4_edit_create(width, height, budget);
    if (!this.handle) throw new Error('cp4_edit_create failed');

    /* Scratch that lives as long as the session. Allocated once through the
     * module's own allocator so it sits inside linear memory where C can
     * reach it, and re-found rather than cached for the reason above. */
    this.fbPtr = this.x.cp_wasm_alloc(width * height * 4);
    this.i32Ptr = this.x.cp_wasm_alloc(MAX_PARTS * PART_FIELDS * 4);
    this.spinePtr = this.x.cp_wasm_alloc(MAX_SEG * 2 * 4);
    this.f32Ptr = this.x.cp_wasm_alloc(64 * 4);
    this.image = new ImageDataLike(width, height);
  }

  destroy() {
    this.x.cp_wasm_free(this.fbPtr);
    this.x.cp_wasm_free(this.i32Ptr);
    this.x.cp_wasm_free(this.f32Ptr);
    this.x.cp4_edit_free(this.handle);
    this.handle = 0;
  }

  /* Never cache the result of these. See the note at the top. */
  u8() { return new Uint8Array(this.x.memory.buffer); }
  i32() { return new Int32Array(this.x.memory.buffer); }
  f32() { return new Float32Array(this.x.memory.buffer); }

  /* ---- loading ---- */
  loadStyle(style) {
    const i = typeof style === 'string' ? STYLES.indexOf(style) : style;
    this.x.cp4_edit_style(this.handle, i < 0 ? 0 : i);
    return this;
  }

  loadRandom(seed = 0) { this.x.cp4_edit_random(this.handle, seed >>> 0); return this; }
  mutate(seed = 0, rate = 0.25) { this.x.cp4_edit_mutate(this.handle, seed >>> 0, rate); return this; }

  /* parts: array of [type, seg, yaw, pitch, scale, mirror, len, bend] */
  load(parts, nseg = 3, girth = 130) {
    const base = this.i32Ptr >> 2;
    const m = this.i32();
    m.fill(0, base, base + MAX_PARTS * PART_FIELDS);
    parts.slice(0, MAX_PARTS).forEach((p, i) => {
      for (let j = 0; j < PART_FIELDS && j < p.length; j++) {
        const v = typeof p[j] === 'string' ? PARTS.indexOf(p[j]) : p[j];
        m[base + i * PART_FIELDS + j] = v | 0;
      }
    });
    this.x.cp4_edit_load(this.handle, this.i32Ptr, nseg, girth);
    return this;
  }

  /* ---- camera ---- */
  view({ azimuth, elev, zoom, phase } = {}) {
    const cur = this.getView();
    this.x.cp4_edit_view(this.handle,
      azimuth ?? cur.azimuth, elev ?? cur.elev, zoom ?? cur.zoom, phase ?? cur.phase);
    return this;
  }

  orbit(daz, del) { this.x.cp4_edit_orbit(this.handle, daz, del); return this; }

  getView() {
    this.x.cp4_edit_get_view(this.handle, this.f32Ptr);
    const f = this.f32(), b = this.f32Ptr >> 2;
    return { azimuth: f[b], elev: f[b + 1], zoom: f[b + 2], phase: f[b + 3] };
  }

  /* ---- pointer ---- */
  surface(x, y) {
    if (!this.x.cp4_edit_surface(this.handle, x | 0, y | 0, this.i32Ptr)) return null;
    const m = this.i32(), b = this.i32Ptr >> 2;
    return { seg: m[b], yaw: m[b + 1], pitch: m[b + 2] };
  }

  pick(x, y) {
    const s = this.x.cp4_edit_pick(this.handle, x | 0, y | 0);
    return s < 0 ? null : s;
  }

  /* Where a slot sits on screen, for hanging drag handles off. Projected from
   * the primitives rather than gathered by picking pixel by pixel, which is
   * the difference between a call you can make on every frame and one that
   * costs a fifth of a second. */
  extent(slot) {
    if (slot === null || !this.x.cp4_edit_extent(this.handle, slot | 0, this.i32Ptr))
      return null;
    const m = this.i32(), b = this.i32Ptr >> 2;
    return { cx: m[b], cy: m[b + 1], tip: [m[b + 2], m[b + 3]], r: m[b + 4] };
  }

  /* Returns the slot, or null if the pointer was off the body, no slot was
   * free, or the budget refused - and in every one of those nothing changed,
   * so a caller can simply try and report. */
  drop(x, y, part, mirror = -1) {
    const t = typeof part === 'string' ? PARTS.indexOf(part) : part;
    const s = this.x.cp4_edit_drop(this.handle, x | 0, y | 0, t, mirror);
    return s < 0 ? null : s;
  }

  move(slot, x, y) { return !!this.x.cp4_edit_move(this.handle, slot, x | 0, y | 0); }
  remove(slot) { return !!this.x.cp4_edit_remove(this.handle, slot); }
  shape(slot, { scale = -1, length = -1, bend = -1000 } = {}) {
    return !!this.x.cp4_edit_shape(this.handle, slot, scale, length, bend);
  }
  setMirror(slot, on) { return !!this.x.cp4_edit_mirror(this.handle, slot, on ? 1 : 0); }

  spinePick(x, y, grab = 14) {
    const v = this.x.cp4_edit_spine_pick(this.handle, x | 0, y | 0, grab);
    return v < 0 ? null : v;
  }
  /* Drag a control point to a pixel. All three axes, because the point is
   * the answer now - there is no longer a gene that can only say "higher". */
  spineMove(vert, x, y) { return !!this.x.cp4_edit_spine_move(this.handle, vert, x | 0, y | 0); }
  spineGirth(vert, amount) { return !!this.x.cp4_edit_spine_girth(this.handle, vert, amount); }

  /* Hold the viewport's automatic framing still. Called on pointerdown and
   * released on pointerup, so what you drag stays under the cursor instead of
   * being chased by the camera re-fitting to the body you are changing. */
  frameHold(on) { this.x.cp4_edit_frame_hold(this.handle, on ? 1 : 0); return this; }

  /* [{x, y}, ...] for every control point, in canvas pixels, so the overlay
   * never has to project anything itself. A point behind the camera comes
   * back as null in its own slot rather than being dropped, or the i-th entry
   * would stop being the i-th vertebra. */
  spinePoints() {
    const n = this.x.cp4_edit_spine_points(this.handle, this.spinePtr);
    const m = this.i32(), b = this.spinePtr >> 2, out = [];
    for (let i = 0; i < n; i++) {
      const x = m[b + 2 * i], y = m[b + 2 * i + 1];
      out.push(x === -32768 ? null : { x, y });
    }
    return out;
  }

  /* Grow or shrink from one end. Growing costs DNA and can be refused, which
   * is why it answers with the new count or null rather than a boolean. */
  spineAdd(front = false) {
    const n = this.x.cp4_edit_spine_add(this.handle, front ? 1 : 0);
    return n < 0 ? null : n;
  }
  spineRemove(front = false) {
    const n = this.x.cp4_edit_spine_remove(this.handle, front ? 1 : 0);
    return n < 0 ? null : n;
  }
  spine({ nseg = -1, girth = -1 } = {}) {
    this.x.cp4_edit_spine_set(this.handle, nseg, girth);
    return this;
  }

  /* ---- paint ---- */
  paint({ hue = -1, hue2 = -1, hue3 = -1, sat = -1, val = -1 } = {}) {
    this.x.cp4_edit_paint(this.handle, hue, hue2, hue3, sat, val);
    return this;
  }

  coats({ pattern = -1, scale = -1, pattern2 = -1, scale2 = -1 } = {}) {
    const a = typeof pattern === 'string' ? PATTERNS.indexOf(pattern) : pattern;
    const b = typeof pattern2 === 'string' ? PATTERNS.indexOf(pattern2) : pattern2;
    this.x.cp4_edit_coats(this.handle, a, scale, b, scale2);
    return this;
  }

  /* ---- readback ---- */
  get cost() { return this.x.cp4_edit_cost(this.handle); }
  get budget() { return this.x.cp4_edit_budget_get(this.handle); }
  canAfford(part, mirror = 0) {
    const t = typeof part === 'string' ? PARTS.indexOf(part) : part;
    return !!this.x.cp4_edit_can_afford(this.handle, t, mirror);
  }

  parts() {
    this.x.cp4_edit_genome(this.handle, this.i32Ptr);
    const m = this.i32(), b = this.i32Ptr >> 2, out = [];
    for (let i = 0; i < MAX_PARTS; i++) {
      const f = b + i * PART_FIELDS;
      if (!m[f]) continue;
      out.push({
        slot: i, type: m[f], name: PARTS[m[f]], seg: m[f + 1], yaw: m[f + 2],
        pitch: m[f + 3], scale: m[f + 4], mirror: !!m[f + 5],
        len: m[f + 6], bend: m[f + 7],
      });
    }
    return out;
  }

  body() {
    this.x.cp4_edit_body(this.handle, this.i32Ptr);
    const m = this.i32(), b = this.i32Ptr >> 2;
    const k = ['nseg', 'girth', 'hue', 'hue2', 'hue3', 'sat',
               'val', 'pattern', 'pscale', 'pattern2', 'pscale2'];
    return Object.fromEntries(k.map((n, i) => [n, m[b + i]]));
  }

  stats() {
    this.x.cp4_edit_stats(this.handle, this.f32Ptr);
    const f = this.f32(), b = this.f32Ptr >> 2;
    return Object.fromEntries(STAT_NAMES.map((n, i) => [n, f[b + i]]));
  }

  finish() {
    this.x.cp4_edit_finish(this.handle, this.i32Ptr);
    return this.parts();
  }

  /* ---- pixels ----
   * Copies out of linear memory rather than viewing into it: an ImageData
   * has to own its bytes, and the next allocation would detach a view anyway.
   */
  render(quality = 2) {
    this.x.cp4_edit_render(this.handle, this.fbPtr, quality);
    const src = this.u8().subarray(this.fbPtr, this.fbPtr + this.width * this.height * 4);
    this.image.data.set(src);
    return this.image;
  }

  drawTo(ctx, quality = 2) {
    const img = this.render(quality);
    ctx.putImageData(img.toImageData ? img.toImageData() : img, 0, 0);
  }
}

/* ImageData exists in a browser and not in node, and the difference is one
 * constructor - not enough to justify two code paths through the renderer. */
class ImageDataLike {
  constructor(w, h) {
    this.width = w;
    this.height = h;
    this.data = new Uint8ClampedArray(w * h * 4);
  }
  toImageData() {
    return typeof ImageData !== 'undefined'
      ? new ImageData(this.data, this.width, this.height) : this;
  }
}

export default CreatureEditor;
