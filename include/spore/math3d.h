#ifndef SPORE_MATH3D_H
#define SPORE_MATH3D_H

/* Minimal 3D math for the creature creator. Header-only on purpose: the
 * creator links against nothing but SDL2, epoxy, libm and the GL runtime, and
 * a separate math translation unit would be one more thing to keep in step.
 * Mat4 is column-major, the layout glUniformMatrix4fv(GL_FALSE) expects. */

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { float x, y, z; } Vec3;
typedef struct { float m[16]; } Mat4;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline Vec3 v3(float x, float y, float z) {
    Vec3 r = { x, y, z };
    return r;
}

static inline Vec3 v3_add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline Vec3 v3_mul(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float v3_dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static inline Vec3 v3_cross(Vec3 a, Vec3 b) {
    return v3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

static inline float v3_len(Vec3 a) { return sqrtf(v3_dot(a, a)); }

static inline Vec3 v3_norm(Vec3 a) {
    float l = v3_len(a);
    return l > 1e-8f ? v3_mul(a, 1.0f / l) : v3(0, 0, 0);
}

static inline Vec3 v3_lerp(Vec3 a, Vec3 b, float t) {
    return v3(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t));
}

static inline Mat4 m4_id(void) {
    Mat4 r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return r;
}

/* a * b */
static inline Mat4 m4_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

static inline Mat4 m4_translate(Vec3 t) {
    Mat4 r = m4_id();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

static inline Mat4 m4_scale(Vec3 s) {
    Mat4 r = m4_id();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}

static inline Mat4 m4_rotate_y(float a) {
    float c = cosf(a), s = sinf(a);
    Mat4 r = m4_id();
    r.m[0] = c;  r.m[2] = -s;
    r.m[8] = s;  r.m[10] = c;
    return r;
}

static inline Mat4 m4_look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = v3_norm(v3_sub(target, eye));
    Vec3 s = v3_norm(v3_cross(f, up));
    Vec3 u = v3_cross(s, f);
    Mat4 r = m4_id();
    r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
    r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -v3_dot(s, eye);
    r.m[13] = -v3_dot(u, eye);
    r.m[14] = v3_dot(f, eye);
    return r;
}

static inline Mat4 m4_perspective(float fovy_rad, float aspect, float near, float far) {
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    Mat4 r = { { 0 } };
    r.m[0] = f / (aspect > 1e-6f ? aspect : 1.0f);
    r.m[5] = f;
    r.m[10] = (far + near) / (near - far);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * far * near) / (near - far);
    return r;
}

/* World point → screen pixel. 0 if behind the eye. */
static inline int m4_project(Mat4 vp, Vec3 p, int w, int h, float *sx, float *sy) {
    float x = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8]  * p.z + vp.m[12];
    float y = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9]  * p.z + vp.m[13];
    float wc = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    if (wc < 1e-5f) return 0;
    *sx = (x / wc * 0.5f + 0.5f) * (float)w;
    *sy = (1.0f - (y / wc * 0.5f + 0.5f)) * (float)h;
    return 1;
}

/* Knee position for a two-bone leg from hip to foot, bent toward pole. */
static inline Vec3 ik_2bone(Vec3 hip, Vec3 foot, float upper, float lower, Vec3 pole) {
    Vec3 d = v3_sub(foot, hip);
    float dist = v3_len(d);
    float reach = upper + lower;
    if (dist > reach * 0.999f) dist = reach * 0.999f;
    if (dist < 1e-4f) dist = 1e-4f;
    Vec3 dir = v3_norm(d);
    float a = (upper * upper - lower * lower + dist * dist) / (2.0f * dist);
    float h2 = upper * upper - a * a;
    float hh = h2 > 0.0f ? sqrtf(h2) : 0.0f;
    Vec3 pd = v3_sub(pole, hip);
    Vec3 pdperp = v3_sub(pd, v3_mul(dir, v3_dot(pd, dir)));
    if (v3_len(pdperp) < 1e-4f) {
        pdperp = v3_cross(dir, v3(0, 0, 1));
        if (v3_len(pdperp) < 1e-4f) pdperp = v3_cross(dir, v3(1, 0, 0));
    }
    pdperp = v3_norm(pdperp);
    return v3_add(hip, v3_add(v3_mul(dir, a), v3_mul(pdperp, hh)));
}

#endif
