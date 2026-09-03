/* The viewport, marched on the GPU.
 *
 * This is a port of src/sdfbody.h and the creature half of src/render_land.c,
 * not a second art direction: same round cones, same smooth minimum, same
 * key/sky/bounce/rim split, same palette when the pixel pass is on. C still
 * decides what the body IS - cp4_pose_prims() hands over the cone list and the
 * skin, and the shader only draws it.
 *
 * What the GPU buys, and the CPU path could never afford at 30fps: full
 * resolution, 2x2 supersampling, a real shadow cast onto the ground, and the
 * ambient occlusion and soft shadow terms evaluated at every pixel instead of
 * being tuned down to fit a frame budget.
 */

const VERT = `#version 300 es
void main() {
  /* one oversized triangle, no buffers */
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

const FRAG = `#version 300 es
precision highp float;
precision highp sampler2D;

uniform sampler2D uPrims;      // 4 texels a primitive
uniform int  uN;
uniform vec2 uRes;
uniform vec3 uEye, uFwd, uRight, uUp;
uniform float uFocal;
uniform vec3 uCentre;
uniform float uBound;
uniform vec3 uSun, uSunC;
uniform vec3 uSkinO, uSkinF, uSkinR, uSkinU;
uniform vec3 uMark, uDetail;
uniform vec2 uPat, uFreq;
uniform int  uAA;
uniform int  uQuant, uPalN;
uniform float uDither;
uniform vec3 uPal[64];
uniform float uSeed;

out vec4 frag;

/* ---- the body ---- */

vec4 prim0(int i) { return texelFetch(uPrims, ivec2(i * 4 + 0, 0), 0); } // a.xyz  ra
vec4 prim1(int i) { return texelFetch(uPrims, ivec2(i * 4 + 1, 0), 0); } // b.xyz  rb
vec4 prim2(int i) { return texelFetch(uPrims, ivec2(i * 4 + 2, 0), 0); } // col    k
vec4 prim3(int i) { return texelFetch(uPrims, ivec2(i * 4 + 3, 0), 0); } // em body

float sdCone(vec3 p, vec3 a, vec3 b, float ra, float rb) {
  vec3 ba = b - a;
  float l2 = dot(ba, ba);
  vec3 pa = p - a;
  float t = l2 > 1e-6 ? clamp(dot(pa, ba) / l2, 0.0, 1.0) : 0.0;
  return length(pa - ba * t) - mix(ra, rb, t);
}

float smin(float a, float b, float k) {
  if (k <= 1e-4) return min(a, b);
  float h = clamp(0.5 + 0.5 * (a - b) / k, 0.0, 1.0);
  return mix(a, b, h) - k * h * (1.0 - h);
}

/* distance only - what the normal, occlusion and shadow taps use */
float map(vec3 q) {
  float d = 1e9, dmin = 1e9, kmax = 0.0;
  for (int i = 0; i < uN; i++) {
    vec4 p0 = prim0(i), p1 = prim1(i);
    float k = prim2(i).w;
    float di = sdCone(q, p0.xyz, p1.xyz, p0.w, p1.w);
    dmin = min(dmin, di);
    kmax = max(kmax, k);
    d = smin(d, di, k);
  }
  /* the same honesty clamp as the C field: chaining smin over a hundred cones
     compounds its correction term until the body swallows the frame */
  float floorD = dmin - kmax * 0.55;
  return d < floorD ? floorD : d;
}

/* distance plus the softmin-weighted coat, for the pixel that actually hit */
float mapCol(vec3 q, out vec3 col, out float em, out float bodyw) {
  float d = 1e9, dmin = 1e9, kmax = 0.0;
  vec3 c = vec3(0.0);
  float e = 0.0, bw = 0.0, wsum = 1e-6;
  for (int i = 0; i < uN; i++) {
    vec4 p0 = prim0(i), p1 = prim1(i), p2 = prim2(i), p3 = prim3(i);
    float di = sdCone(q, p0.xyz, p1.xyz, p0.w, p1.w);
    dmin = min(dmin, di);
    kmax = max(kmax, p2.w);
    d = smin(d, di, p2.w);
    float w = exp(-di * 0.30);
    c += p2.xyz * w;
    e += p3.x * w;
    bw += p3.y * w;
    wsum += w;
  }
  col = c / wsum; em = e / wsum; bodyw = bw / wsum;
  float floorD = dmin - kmax * 0.55;
  return d < floorD ? floorD : d;
}

vec3 sdfNormal(vec3 q) {
  const float h = 0.35;
  return normalize(vec3(
    map(q + vec3(h, 0, 0)) - map(q - vec3(h, 0, 0)),
    map(q + vec3(0, h, 0)) - map(q - vec3(0, h, 0)),
    map(q + vec3(0, 0, h)) - map(q - vec3(0, 0, h))));
}

float sdfAO(vec3 q, vec3 n) {
  float occ = 0.0, sca = 1.0;
  for (int i = 1; i <= 4; i++) {
    float h = 0.55 * float(i);
    occ += (h - map(q + n * h)) * sca;
    sca *= 0.70;
  }
  return clamp(1.0 - 1.6 * occ, 0.20, 1.0);
}

float sdfShadow(vec3 q, vec3 ldir) {
  float res = 1.0, t = 0.9;
  for (int i = 0; i < 24; i++) {
    if (t >= 26.0) break;
    float d = map(q + ldir * t);
    if (d < 0.05) return 0.15;
    res = min(res, 7.0 * d / t);
    t += clamp(d, 0.4, 3.0);
  }
  return clamp(res, 0.15, 1.0);
}

/* ---- coats ---- */

vec3 patternLayer(vec3 q, vec3 albedo, float bodyw, float pattern, float freq,
                  vec3 ink, float strength) {
  if (bodyw <= 0.02 || pattern < 0.5) return albedo;
  vec3 d = q - uSkinO;
  float along = dot(d, uSkinF), side = dot(d, uSkinR), vert = dot(d, uSkinU);
  float m = 0.0;
  int p = int(pattern + 0.5);
  if (p == 1) {                                   // bands
    m = sin(along * freq * 6.0) > 0.15 ? 1.0 : 0.0;
  } else if (p == 2) {                            // spots
    m = (sin(along * freq * 5.0) * sin(side * freq * 5.0)
       * sin(vert * freq * 5.0)) > 0.30 ? 1.0 : 0.0;
  } else if (p == 3) {                            // countershading
    m = clamp(0.5 + vert * 0.16, 0.0, 1.0);
  } else if (p == 4) {                            // stripes
    m = sin(atan(vert, side) * 4.0 + along * freq * 0.9) > 0.1 ? 1.0 : 0.0;
  } else if (p == 5) {                            // mottle
    float a = sin(along * freq * 3.1) + sin(side * freq * 4.7)
            + sin(vert * freq * 2.3) + sin((along + side) * freq * 6.1);
    m = a > 0.55 ? 1.0 : 0.0;
  } else if (p == 6) {                            // gradient
    m = clamp(0.5 - along * freq * 0.55, 0.0, 1.0);
    m = m > 0.5 ? (m - 0.5) * 2.0 : 0.0;
  } else if (p == 7) {                            // rings
    m = sin(sqrt(side * side + vert * vert) * freq * 7.0) > 0.2 ? 1.0 : 0.0;
  }
  return mix(albedo, ink, m * bodyw * strength);
}

/* ---- shading ---- */

float tonemap(float x) {
  x = max(x, 0.0);
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

float hash21(vec2 p) {
  p = fract(p * vec2(123.34, 456.21) + uSeed);
  p += dot(p, p + 45.32);
  return fract(p.x * p.y);
}

/* Smooth value noise, two octaves. Flooring a hash straight into the ground
 * colour tiles the floor into visible squares, which is the one thing a studio
 * backdrop must not do - it competes with the subject. */
float vnoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x),
             mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), f.x), f.y);
}

vec3 shade(vec3 q, vec3 nrm, vec3 albedo, float em, float ao, float shadow) {
  float lam = clamp(-dot(nrm, uSun), 0.0, 1.0);
  float ndotv = clamp(-dot(nrm, uFwd), 0.0, 1.0);
  float rim = pow(1.0 - ndotv, 2.6);
  float upAmt = clamp(-nrm.y, 0.0, 1.0);
  float wrap = clamp((lam + 0.42) / 1.42, 0.0, 1.0);
  wrap *= wrap;
  float key  = 1.05 * wrap * shadow;
  float sky  = 0.34 * (0.35 + 0.65 * upAmt);
  float grnd = 0.20 * (1.0 - upAmt);
  float fill = 0.14 * ndotv;
  vec3 skyc = vec3(0.66, 0.80, 1.00);
  vec3 bncc = vec3(0.78, 0.70, 0.46);
  vec3 col = albedo * uSunC * key
           + albedo * skyc * (sky + fill) * ao
           + albedo * bncc * grnd * ao;
  col += vec3(0.72, 0.80, 0.92) * rim * 0.22 * ao;
  col += albedo * em;
  return col;
}

/* one ray: the studio backdrop, the ground it stands on, and the animal */
vec3 trace(vec3 ray) {
  /* backdrop: dark, cool and almost flat, so it never competes with the
     subject - the same ramp the still renderer uses */
  float ty = clamp(0.5 - ray.y * 1.4, 0.0, 1.0);
  vec3 col = vec3(mix(0.055, 0.105, ty), mix(0.065, 0.115, ty), mix(0.095, 0.130, ty));

  /* the ground plane at y = 0, which is exactly where the legs reach */
  float tg = 1e30;
  if (ray.y > 0.001) {
    float t = (0.0 - uEye.y) / ray.y;
    if (t > 0.0 && t < 4000.0) {
      vec3 h = uEye + ray * t;
      float r = length(h.xz);
      float fade = clamp(1.0 - r / (uBound * 3.4), 0.0, 1.0);
      if (fade > 0.0) {
        float g = 0.24 + 0.07 * vnoise(h.xz * 0.25) + 0.03 * vnoise(h.xz * 0.9);
        vec3 gc = vec3(g * 1.02, g * 0.94, g * 0.72);   /* dirt, not lawn */
        /* the animal's shadow on the floor it stands on. The still renderer
           has no such thing - at 1.15us a pixel it could not pay for a second
           march - and it is most of what roots a body to the ground. */
        float sh = sdfShadow(h + vec3(0.0, -0.05, 0.0), -uSun);
        gc *= 0.45 + 0.55 * sh;
        col = mix(col, gc, fade);
        tg = t;
      }
    }
  }

  /* the animal */
  vec3 oc = uEye - uCentre;
  float b = dot(oc, ray);
  float cc = dot(oc, oc) - uBound * uBound;
  float disc = b * b - cc;
  if (disc <= 0.0) return col;
  float sq = sqrt(disc);
  float t = max(-b - sq, 0.0), tmax = -b + sq;
  if (tmax < 0.0) return col;

  for (int i = 0; i < 96; i++) {
    if (t >= tmax || t >= tg) break;
    vec3 q = uEye + ray * t;
    float d = map(q);
    if (d < 0.05) {
      vec3 albedo; float em, bodyw;
      mapCol(q, albedo, em, bodyw);
      albedo = patternLayer(q, albedo, bodyw, uPat.x, uFreq.x, uMark, 1.0);
      albedo = patternLayer(q, albedo, bodyw, uPat.y, uFreq.y, uDetail, 0.72);
      vec3 nrm = sdfNormal(q);
      return shade(q, nrm, albedo, em, sdfAO(q, nrm), sdfShadow(q, -uSun));
    }
    t += d * 0.7;
  }
  return col;
}

/* ---- the optional pixel-art pass ---- */

const int BAYER[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);

vec3 quantise(vec3 c, vec2 px) {
  ivec2 b = ivec2(px) & 3;
  float d = (float(BAYER[b.y * 4 + b.x]) / 16.0 - 0.469) * uDither;
  vec3 p = c * 255.0 + d;
  float pc = max(max(p.r, p.g), p.b) - min(min(p.r, p.g), p.b);
  int best = 0;
  float bestd = 1e18;
  for (int i = 0; i < uPalN; i++) {
    vec3 e = p - uPal[i];
    float err = e.r * e.r * 0.30 + e.g * e.g * 0.59 + e.b * e.b * 0.11;
    float qc = max(max(uPal[i].r, uPal[i].g), uPal[i].b)
             - min(min(uPal[i].r, uPal[i].g), uPal[i].b);
    err += (pc - qc) * (pc - qc) * 0.30;
    if (err < bestd) { bestd = err; best = i; }
  }
  return uPal[best] / 255.0;
}

void main() {
  vec3 sum = vec3(0.0);
  float n = float(uAA * uAA);
  for (int sy = 0; sy < uAA; sy++) {
    for (int sx = 0; sx < uAA; sx++) {
      vec2 o = (vec2(float(sx), float(sy)) + 0.5) / float(uAA);
      vec2 p = vec2(gl_FragCoord.x, uRes.y - gl_FragCoord.y) - 0.5 + o;
      float px = (p.x - uRes.x * 0.5) / uFocal;
      float py = -(p.y - uRes.y * 0.5) / uFocal;
      vec3 ray = normalize(uRight * px + uUp * py + uFwd);
      vec3 c = trace(ray);
      sum += vec3(tonemap(c.r), tonemap(c.g), tonemap(c.b));
    }
  }
  vec3 col = sum / n;
  if (uQuant == 1) col = quantise(col, gl_FragCoord.xy);
  frag = vec4(col, 1.0);
}`;

function makeGLView(canvas) {
  const gl = canvas.getContext("webgl2", { antialias: false, preserveDrawingBuffer: false });
  if (!gl) throw new Error("this browser has no WebGL2");
  if (!gl.getExtension("EXT_color_buffer_float") && !gl.getExtension("OES_texture_float_linear")) {
    /* only needed for float textures as render targets; sampling is core in
       WebGL2, so carry on if the extension is absent */
  }

  const compile = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
      throw new Error(gl.getShaderInfoLog(s) || "shader failed");
    }
    return s;
  };
  const prog = gl.createProgram();
  gl.attachShader(prog, compile(gl.VERTEX_SHADER, VERT));
  gl.attachShader(prog, compile(gl.FRAGMENT_SHADER, FRAG));
  gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
    throw new Error(gl.getProgramInfoLog(prog) || "link failed");
  }
  gl.useProgram(prog);
  const vao = gl.createVertexArray();
  gl.bindVertexArray(vao);

  const U = {};
  const u = (name) => (U[name] !== undefined ? U[name] : (U[name] = gl.getUniformLocation(prog, name)));

  // the cone list, as a 4-texels-per-primitive strip
  const MAXP = 176;
  const tex = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, tex);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, MAXP * 4, 1, 0, gl.RGBA, gl.FLOAT, null);
  gl.uniform1i(u("uPrims"), 0);

  const SUN = normalize([0.38, 0.72, -0.26]);   // a portrait is always noon
  gl.uniform3f(u("uSun"), SUN[0], SUN[1], SUN[2]);
  const sc = sunColour(SUN[1]);
  gl.uniform3f(u("uSunC"), sc[0], sc[1], sc[2]);

  function sunColour(elev) {
    let t = Math.min(Math.max(elev / 0.55, 0), 1);
    t = t * t * (3 - 2 * t);
    return [1.30 + (1.00 - 1.30) * t, 0.46 + (0.96 - 0.46) * t, 0.16 + (0.88 - 0.16) * t];
  }
  function normalize(v) {
    const l = Math.hypot(v[0], v[1], v[2]) || 1;
    return [v[0] / l, v[1] / l, v[2] / l];
  }

  return {
    gl,
    palette(rgb, n, dither) {
      const flat = new Float32Array(64 * 3);
      for (let i = 0; i < n * 3; i++) flat[i] = rgb[i];
      gl.uniform3fv(u("uPal"), flat);
      gl.uniform1i(u("uPalN"), n);
      gl.uniform1f(u("uDither"), dither);
    },

    /* prims: Float32Array of count*16 straight out of cp4_pose_prims */
    upload(prims, count) {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, count * 4, 1, gl.RGBA, gl.FLOAT,
                       prims.subarray(0, count * 16));
      gl.uniform1i(u("uN"), count);
    },

    skin(meta) {
      gl.uniform3f(u("uCentre"), meta[0], meta[1], meta[2]);
      gl.uniform1f(u("uBound"), meta[3]);
      gl.uniform3f(u("uSkinO"), meta[4], meta[5], meta[6]);
      gl.uniform3f(u("uSkinF"), meta[7], meta[8], meta[9]);
      gl.uniform3f(u("uSkinR"), meta[10], meta[11], meta[12]);
      gl.uniform3f(u("uSkinU"), meta[13], meta[14], meta[15]);
      gl.uniform3f(u("uMark"), meta[19], meta[20], meta[21]);
      gl.uniform3f(u("uDetail"), meta[22], meta[23], meta[24]);
      gl.uniform2f(u("uPat"), meta[25], meta[27]);
      gl.uniform2f(u("uFreq"), meta[26], meta[28]);
    },

    /* The same camera the still renderer frames with: back far enough to leave
       a margin, aimed a little above the bounding centre because an animal
       grows upward off the ground. */
    camera(centre, bound, az, el, zoom) {
      const dir = normalize([Math.cos(az) * Math.cos(el), -Math.sin(el), Math.sin(az) * Math.cos(el)]);
      const dist = (bound * 3.30 + 10.0) * (zoom || 1);
      const aim = [centre[0], centre[1] - bound * 0.16, centre[2]];
      const eye = [aim[0] + dir[0] * dist, aim[1] + dir[1] * dist, aim[2] + dir[2] * dist];
      const look = normalize([aim[0] - eye[0], aim[1] - eye[1], aim[2] - eye[2]]);
      const right = normalize([-look[2], 0, look[0]]);
      const up = normalize([
        right[2] * look[1] - right[1] * look[2],
        right[0] * look[2] - right[2] * look[0],
        right[1] * look[0] - right[0] * look[1],
      ]);
      gl.uniform3f(u("uEye"), eye[0], eye[1], eye[2]);
      gl.uniform3f(u("uFwd"), look[0], look[1], look[2]);
      gl.uniform3f(u("uRight"), right[0], right[1], right[2]);
      gl.uniform3f(u("uUp"), up[0], up[1], up[2]);
      return { eye, fwd: look, right, up };
    },

    draw(w, h, aa, quant, seed) {
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
      }
      gl.viewport(0, 0, w, h);
      gl.uniform2f(u("uRes"), w, h);
      gl.uniform1f(u("uFocal"), w * 1.15);
      gl.uniform1i(u("uAA"), aa);
      gl.uniform1i(u("uQuant"), quant ? 1 : 0);
      gl.uniform1f(u("uSeed"), seed);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
    },
  };
}
