/* Reading and writing a Cp4Genome from JavaScript, and the geometry an editor
 * needs to point at one.
 *
 * Cp4Genome is all one-byte fields, so a creature is edited here by writing
 * bytes into module memory - no marshalling, no mirror of the struct on this
 * side that could drift out of step. The offsets are asserted against
 * sizeof(Cp4Genome) at startup.
 *
 * The geometry half inverts what src/render_land.c does when it mounts a part:
 *   axis  = fwd*cos(yaw)*cos(pitch) + right*sin(yaw)*cos(pitch) + up*sin(pitch)
 *   mount = spine[seg] + axis * segrad[seg] * 0.60
 * Given a point on the body, running that backwards is what turns "the user
 * pointed here" into a segment and a yaw/pitch the genome can hold.
 */

const G = {
  PART: 0, PART_STRIDE: 8,
  NSEG: 128, GIRTH: 129, PROF: 130, LUMP: 134, RISE: 140,
  ARCH: 146, SWEEP: 147,
  HUE: 148, HUE2: 149, HUE3: 150, SAT: 151, VAL: 152,
  PATTERN: 153, PSCALE: 154, PATTERN2: 155, PSCALE2: 156,
  BYTES: 157,
};
const P = { TYPE: 0, SEG: 1, YAW: 2, PITCH: 3, SCALE: 4, MIRROR: 5, LEN: 6, BEND: 7 };
const MAX_PARTS = 16;

/* the parts built as jointed chains, where `len` is a reach and `bend` a fold */
const JOINTED = new Set(["leg", "arm", "tail"]);

const s8 = (v) => (v > 127 ? v - 256 : v);
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);

/* ---- small vector helpers ---- */
const vsub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const vadd = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const vmul = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
const vdot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const vlen = (a) => Math.hypot(a[0], a[1], a[2]);
function vnorm(a) {
  const l = vlen(a) || 1;
  return [a[0] / l, a[1] / l, a[2] / l];
}

/* ---- the body, as the last build left it ---- */

function readPose(meta) {
  const spine = [];
  const nseg = Math.round(meta[31]);
  for (let i = 0; i < nseg; i++) {
    spine.push({
      pos: [meta[32 + i * 4], meta[33 + i * 4], meta[34 + i * 4]],
      rad: meta[35 + i * 4],
    });
  }
  return {
    centre: [meta[0], meta[1], meta[2]],
    bound: meta[3],
    fwd: [meta[7], meta[8], meta[9]],
    right: [meta[10], meta[11], meta[12]],
    up: [meta[13], meta[14], meta[15]],
    spine,
  };
}

function partAxis(pose, yawByte, pitchByte) {
  const py = (yawByte & 0xff) * ((2 * Math.PI) / 256);
  const pp = s8(pitchByte & 0xff) * (Math.PI / 128);
  const cy = Math.cos(py), sy = Math.sin(py);
  const cp = Math.cos(pp), sp = Math.sin(pp);
  return vnorm(vadd(vadd(vmul(pose.fwd, cy * cp), vmul(pose.right, sy * cp)),
                    vmul(pose.up, sp)));
}

/* Where a part's mount sits in the world. `copy` 1 is the mirrored one, whose
 * yaw the builder reflects rather than storing twice. */
function partMount(gene, pose, i, copy) {
  const o = G.PART + i * G.PART_STRIDE;
  const seg = Math.min(gene[o + P.SEG], pose.spine.length - 1);
  const yaw = copy ? (256 - gene[o + P.YAW]) & 0xff : gene[o + P.YAW];
  const st = pose.spine[seg];
  if (!st) return null;
  const ax = partAxis(pose, yaw, gene[o + P.PITCH]);
  return vadd(st.pos, vmul(ax, st.rad * 0.6));
}

/* The inverse: a point in space becomes a segment and a direction on it. */
function pointToMount(pose, q) {
  let seg = 0, best = 1e30;
  pose.spine.forEach((st, i) => {
    const d = vlen(vsub(q, st.pos));
    if (d < best) { best = d; seg = i; }
  });
  const st = pose.spine[seg];
  const d = vnorm(vsub(q, st.pos));
  const along = vdot(d, pose.fwd), side = vdot(d, pose.right), vert = vdot(d, pose.up);
  const pitch = Math.asin(clamp(vert, -1, 1));
  const yaw = Math.atan2(side, along);
  let yawByte = Math.round((yaw / (2 * Math.PI)) * 256) & 0xff;
  const pitchByte = clamp(Math.round((pitch / Math.PI) * 128), -128, 127);
  return { seg, yawByte, pitchByte, dist: best, rad: st.rad };
}

/* ---- the distance field, for pointing at the body ---- */

function sdCone(q, ax, ay, az, bx, by, bz, ra, rb) {
  const bax = bx - ax, bay = by - ay, baz = bz - az;
  const l2 = bax * bax + bay * bay + baz * baz;
  const pax = q[0] - ax, pay = q[1] - ay, paz = q[2] - az;
  let t = l2 > 1e-6 ? (pax * bax + pay * bay + paz * baz) / l2 : 0;
  t = clamp(t, 0, 1);
  const dx = pax - bax * t, dy = pay - bay * t, dz = paz - baz * t;
  return Math.hypot(dx, dy, dz) - (ra + (rb - ra) * t);
}

function smin(a, b, k) {
  if (k <= 1e-4) return Math.min(a, b);
  const h = clamp(0.5 + (0.5 * (a - b)) / k, 0, 1);
  return a + (b - a) * h - k * h * (1 - h);
}

function bodyDistance(prims, count, q) {
  let d = 1e9, dmin = 1e9, kmax = 0;
  for (let i = 0; i < count; i++) {
    const o = i * 16;
    const di = sdCone(q, prims[o], prims[o + 1], prims[o + 2],
                      prims[o + 4], prims[o + 5], prims[o + 6],
                      prims[o + 3], prims[o + 7]);
    if (di < dmin) dmin = di;
    const k = prims[o + 11];
    if (k > kmax) kmax = k;
    d = smin(d, di, k);
  }
  const floorD = dmin - kmax * 0.55;
  return d < floorD ? floorD : d;
}

/* March one ray and return where it lands on the animal, or null. One ray of
 * the ninety thousand the GPU does is nothing, so picking stays on this side
 * where the interaction lives. */
function rayHitBody(prims, count, pose, eye, ray) {
  const oc = vsub(eye, pose.centre);
  const b = vdot(oc, ray);
  const cc = vdot(oc, oc) - pose.bound * pose.bound;
  const disc = b * b - cc;
  if (disc <= 0) return null;
  const sq = Math.sqrt(disc);
  let t = Math.max(-b - sq, 0);
  const tmax = -b + sq;
  for (let i = 0; i < 96 && t < tmax; i++) {
    const q = vadd(eye, vmul(ray, t));
    const d = bodyDistance(prims, count, q);
    if (d < 0.05) return q;
    t += d * 0.7;
  }
  return null;
}

/* When the cursor is off the body - dragging a limb out past the silhouette -
 * the gesture still has to mean something, so fall back to the closest the ray
 * comes to the segment the part is mounted on. */
function rayNearestTo(eye, ray, p) {
  const t = Math.max(vdot(vsub(p, eye), ray), 0.1);
  return vadd(eye, vmul(ray, t));
}
