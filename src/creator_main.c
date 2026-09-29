/* Lochner-style Creature Creator MVP for cpore.
 * Algorithms from Daniel Lochner's Creature Creator (MIT).
 * Modes: Build (spine + parts) / Paint / Test — matching Lochner's UI loop.
 */

#include "spore/bodymesh.h"
#include "spore/desc.h"
#include "spore/genome.h"
#include "spore/math3d.h"

#include <SDL.h>
#include <epoxy/gl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODE_BUILD = 0, MODE_PAINT = 1, MODE_TEST = 2 };

typedef struct {
    unsigned prog, vao, vbo, ebo;
    int u_mvp, u_model, u_color, u_light_dir, u_cam_pos, u_use_vert_color, u_alpha;
    unsigned bone_vao, bone_vbo;
} CreatorGL;

typedef struct {
    SpineColumn spine;
    Genome genome;
    Vec3 color, color2;
    int pattern;
    float scale;
    char name[24];
    int cash;
    int mode;
    int sel_bone;
    int part_slot;
    int dirty_mesh;
    BodyMesh mesh;
    BodyMeshSettings mesh_set;
    /* Lochner mirrored limb sockets: local X offsets; |x|<merge → centered */
    float limb_x[CREATURE_LEGS];
    float limb_y[CREATURE_LEGS];
    float limb_z[CREATURE_LEGS];
    int limb_bone[CREATURE_LEGS];
    float merge_threshold;
    /* Test mode: a real Creature driven on a Terrain */
    Terrain terrain;
    Creature testc;
    int test_live;
} CreatorState;

/* Mouse ray ∩ plane through plane_p with normal plane_n → world point. */
static int mouse_hit_plane(Vec3 eye, Vec3 cam_r, Vec3 cam_u, Vec3 cam_f,
                           float fovy_rad, float aspect,
                           int mx, int my, int ww, int wh,
                           Vec3 plane_p, Vec3 plane_n, Vec3 *out) {
    if (ww < 1 || wh < 1) return 0;
    float ndc_x = ((float)mx + 0.5f) / (float)ww * 2.0f - 1.0f;
    float ndc_y = 1.0f - ((float)my + 0.5f) / (float)wh * 2.0f;
    float th = tanf(fovy_rad * 0.5f);
    Vec3 dir = v3_norm(v3_add(cam_f,
                       v3_add(v3_mul(cam_r, ndc_x * th * aspect),
                              v3_mul(cam_u, ndc_y * th))));
    float denom = v3_dot(dir, plane_n);
    if (fabsf(denom) < 1e-5f) return 0;
    float t = v3_dot(v3_sub(plane_p, eye), plane_n) / denom;
    if (t < 0.05f) return 0;
    *out = v3_add(eye, v3_mul(dir, t));
    return 1;
}

static void cam_basis(float yaw, float pitch, float dist, float side,
                      Vec3 *eye, Vec3 *target, Vec3 *right, Vec3 *up, Vec3 *fwd) {
    float cp = cosf(pitch), sp = sinf(pitch);
    float cy = cosf(yaw), sy = sinf(yaw);
    *target = v3(side, 0.4f, 0.0f);
    *eye = v3_add(*target, v3(sy * cp * dist, sp * dist + 0.8f, cy * cp * dist));
    *fwd = v3_norm(v3_sub(*target, *eye));
    *right = v3_norm(v3_cross(*fwd, v3(0, 1, 0)));
    if (v3_len(v3_cross(*fwd, v3(0, 1, 0))) < 1e-4f)
        *right = v3(1, 0, 0);
    *up = v3_norm(v3_cross(*right, *fwd));
}

static Vec3 stretch_handle_pos(const CreatorState *st, int front) {
    int n = st->spine.count;
    if (n < 1) return v3(0, 0, 0);
    if (front) {
        Vec3 tip = v3(st->spine.v[0].x, st->spine.v[0].y, st->spine.v[0].z);
        Vec3 dir = v3(0, 0, 1);
        if (n >= 2) {
            dir = v3_norm(v3_sub(tip, v3(st->spine.v[1].x, st->spine.v[1].y, st->spine.v[1].z)));
        }
        return v3_add(tip, v3_mul(dir, st->spine.v[0].radius * 0.9f + 0.28f));
    }
    Vec3 tip = v3(st->spine.v[n - 1].x, st->spine.v[n - 1].y, st->spine.v[n - 1].z);
    Vec3 dir = v3(0, 0, -1);
    if (n >= 2) {
        dir = v3_norm(v3_sub(tip, v3(st->spine.v[n - 2].x, st->spine.v[n - 2].y, st->spine.v[n - 2].z)));
    }
    return v3_add(tip, v3_mul(dir, st->spine.v[n - 1].radius * 0.9f + 0.28f));
}

/* Returns: >=0 bone index, -2 front stretch, -3 back stretch, -1 miss */
static int pick_tool(const CreatorState *st, Mat4 vp, int mx, int my, int ww, int wh) {
    int best = -1;
    float best_d2 = 36.0f * 36.0f;
    /* Prefer stretch handles (larger hit) */
    for (int h = 0; h < 2; h++) {
        Vec3 p = stretch_handle_pos(st, h == 0);
        float sx, sy;
        if (!m4_project(vp, p, ww, wh, &sx, &sy)) continue;
        float dx = sx - (float)mx, dy = sy - (float)my;
        float d2 = dx * dx + dy * dy;
        float rad = 42.0f;
        if (d2 < rad * rad && d2 < best_d2) {
            best_d2 = d2;
            best = (h == 0) ? -2 : -3;
        }
    }
    for (int i = 0; i < st->spine.count; i++) {
        float sx, sy;
        Vec3 p = v3(st->spine.v[i].x, st->spine.v[i].y, st->spine.v[i].z);
        if (!m4_project(vp, p, ww, wh, &sx, &sy)) continue;
        float dx = sx - (float)mx, dy = sy - (float)my;
        float d2 = dx * dx + dy * dy;
        if (d2 < 28.0f * 28.0f && d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

static void upload_unit_cube(CreatorGL *g) {
    static const float cube_pos[] = {
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,0.5f,-0.5f,
         0.5f,0.5f,-0.5f, -0.5f,0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
        -0.5f,-0.5f, 0.5f,  0.5f,0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
        -0.5f,-0.5f, 0.5f, -0.5f,0.5f, 0.5f,  0.5f,0.5f, 0.5f,
         0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,  0.5f,0.5f, 0.5f,
         0.5f,0.5f, 0.5f,  0.5f,0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f,0.5f,-0.5f, -0.5f,0.5f, 0.5f,
        -0.5f,0.5f, 0.5f, -0.5f,-0.5f, 0.5f, -0.5f,-0.5f,-0.5f,
        -0.5f,0.5f,-0.5f,  0.5f,0.5f,-0.5f,  0.5f,0.5f, 0.5f,
         0.5f,0.5f, 0.5f, -0.5f,0.5f, 0.5f, -0.5f,0.5f,-0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
         0.5f,-0.5f, 0.5f,  0.5f,-0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
    };
    /* one outward normal per face (6 verts each): -Z, +Z, +X, -X, +Y, -Y */
    static const float face_nrm[6][3] = {
        { 0, 0,-1 }, { 0, 0, 1 }, { 1, 0, 0 },
        { -1, 0, 0 }, { 0, 1, 0 }, { 0,-1, 0 },
    };
    float inter[36 * 9];
    for (int i = 0; i < 36; i++) {
        int face = i / 6;
        inter[i * 9 + 0] = cube_pos[i * 3 + 0];
        inter[i * 9 + 1] = cube_pos[i * 3 + 1];
        inter[i * 9 + 2] = cube_pos[i * 3 + 2];
        inter[i * 9 + 3] = face_nrm[face][0];
        inter[i * 9 + 4] = face_nrm[face][1];
        inter[i * 9 + 5] = face_nrm[face][2];
        inter[i * 9 + 6] = inter[i * 9 + 7] = inter[i * 9 + 8] = 1;
    }
    glBindVertexArray(g->bone_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->bone_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(inter), inter, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(6 * sizeof(float)));
}

static void draw_cube_model(CreatorGL *g, Mat4 vp, Vec3 eye, Mat4 model, Vec3 col) {
    Mat4 mvp = m4_mul(vp, model);
    glUniformMatrix4fv(g->u_mvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(g->u_model, 1, GL_FALSE, model.m);
    glUniform3f(g->u_color, col.x, col.y, col.z);
    glUniform3f(g->u_light_dir, 0.35f, 0.9f, 0.25f);
    glUniform3f(g->u_cam_pos, eye.x, eye.y, eye.z);
    glUniform1i(g->u_use_vert_color, 0);
    glUniform1f(g->u_alpha, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

static void draw_cube_gizmo(CreatorGL *g, Mat4 vp, Vec3 eye, Vec3 pos, Vec3 size, Vec3 col) {
    draw_cube_model(g, vp, eye, m4_mul(m4_translate(pos), m4_scale(size)), col);
}

/* A box from a to b with cross-section radius r — an oriented limb bone. */
static void draw_box_seg(CreatorGL *g, Mat4 vp, Vec3 eye, Vec3 a, Vec3 b,
                         float r, Vec3 col) {
    Vec3 d = v3_sub(b, a);
    float len = v3_len(d);
    if (len < 1e-4f) return;
    Vec3 dir = v3_mul(d, 1.0f / len);
    Vec3 up = fabsf(dir.y) > 0.98f ? v3(1, 0, 0) : v3(0, 1, 0);
    Vec3 right = v3_norm(v3_cross(up, dir));
    Vec3 fwd = v3_cross(right, dir);   /* right-handed, so normals face out */
    Mat4 R = m4_id();
    R.m[0] = right.x; R.m[1] = right.y; R.m[2] = right.z;
    R.m[4] = dir.x;   R.m[5] = dir.y;   R.m[6] = dir.z;
    R.m[8] = fwd.x;   R.m[9] = fwd.y;   R.m[10] = fwd.z;
    Mat4 model = m4_mul(m4_translate(v3_lerp(a, b, 0.5f)),
                        m4_mul(R, m4_scale(v3(r, len, r))));
    draw_cube_model(g, vp, eye, model, col);
}

static void draw_bones(CreatorGL *g, const CreatorState *st, Mat4 vp, Vec3 eye) {
    if (st->mode != MODE_BUILD || !st->spine.active) return;
    glDisable(GL_CULL_FACE);
    glUseProgram(g->prog);
    upload_unit_cube(g);

    int n = st->spine.count;
    /* Vertebra cubes (Core GL — GL_POINTS often invisible) */
    for (int i = 0; i < n; i++) {
        Vec3 p = v3(st->spine.v[i].x, st->spine.v[i].y, st->spine.v[i].z);
        float rad = 0.11f + st->spine.v[i].radius * 0.08f;
        int sel = (i == st->sel_bone);
        Vec3 col = sel ? v3(1.0f, 0.92f, 0.20f) : v3(0.95f, 0.75f, 0.25f);
        draw_cube_gizmo(g, vp, eye, p, v3(rad, rad, rad), col);
        /* connector toward next */
        if (i + 1 < n) {
            Vec3 nxt = v3(st->spine.v[i + 1].x, st->spine.v[i + 1].y, st->spine.v[i + 1].z);
            Vec3 mid = v3_lerp(p, nxt, 0.5f);
            draw_cube_gizmo(g, vp, eye, mid, v3(0.04f, 0.04f, 0.04f), v3(0.7f, 0.55f, 0.2f));
        }
    }

    /* Lochner stretch arrows — bright red (nose) / green (tail) */
    {
        Vec3 front = stretch_handle_pos(st, 1);
        Vec3 back = stretch_handle_pos(st, 0);
        Vec3 nose = v3(st->spine.v[0].x, st->spine.v[0].y, st->spine.v[0].z);
        Vec3 tail = v3(st->spine.v[n - 1].x, st->spine.v[n - 1].y, st->spine.v[n - 1].z);
        Vec3 fd = v3_norm(v3_sub(front, nose));
        Vec3 bd = v3_norm(v3_sub(back, tail));
        draw_cube_gizmo(g, vp, eye, front, v3(0.18f, 0.18f, 0.18f), v3(1.0f, 0.22f, 0.15f));
        draw_cube_gizmo(g, vp, eye, v3_add(front, v3_mul(fd, 0.16f)),
                        v3(0.26f, 0.26f, 0.14f), v3(1.0f, 0.50f, 0.22f));
        draw_cube_gizmo(g, vp, eye, back, v3(0.18f, 0.18f, 0.18f), v3(0.20f, 0.95f, 0.35f));
        draw_cube_gizmo(g, vp, eye, v3_add(back, v3_mul(bd, 0.16f)),
                        v3(0.26f, 0.26f, 0.14f), v3(0.40f, 1.0f, 0.50f));
    }

    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

static void sync_mirrored_limbs(CreatorState *st) {
    /* Lochner/Spore: limb *pairs* — left (+x) is source, right is exact mirror. */
    int n = genome_limb_count(&st->genome);
    int segs = st->spine.count;
    if (segs < 1) return;
    spine_enforce_symmetry(&st->spine);
    st->genome.asym = ASYM_BALANCED;
    int pairs = n / 2;
    if (pairs < 1) pairs = 1;
    for (int p = 0; p < pairs && (p * 2) < CREATURE_LEGS; p++) {
        int L = p * 2;
        int R = L + 1;
        int vi = st->limb_bone[L];
        if (vi < 0 || vi >= segs) {
            /* Spread pairs along spine: front pair near nose, rear near tail */
            float u = (pairs <= 1) ? 0.45f : (0.25f + 0.50f * (float)p / (float)(pairs - 1));
            vi = (int)(u * (float)(segs - 1) + 0.5f);
            st->limb_bone[L] = vi;
        }
        float r = st->spine.v[vi].radius;
        float ox = st->limb_x[L];
        if (fabsf(ox) < st->merge_threshold)
            ox = r * 0.95f; /* always off-midline so the flip twin shows */
        ox = fabsf(ox);
        st->limb_x[L] = ox;
        st->limb_y[L] = -r * 0.85f;
        st->limb_z[L] = st->spine.v[vi].z;
        st->limb_bone[L] = vi;
        if (R < CREATURE_LEGS) {
            st->limb_x[R] = -ox;
            st->limb_y[R] = st->limb_y[L];
            st->limb_z[R] = st->limb_z[L];
            st->limb_bone[R] = vi;
        }
    }
}

static const char *VERT_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_nrm;\n"
    "layout(location=2) in vec3 a_col;\n"
    "uniform mat4 u_mvp;\n"
    "uniform mat4 u_model;\n"
    "out vec3 v_nrm;\n"
    "out vec3 v_col;\n"
    "out vec3 v_world;\n"
    "void main(){\n"
    "  vec4 w = u_model * vec4(a_pos,1.0);\n"
    "  v_world = w.xyz;\n"
    "  v_nrm = mat3(u_model) * a_nrm;\n"
    "  v_col = a_col;\n"
    "  gl_Position = u_mvp * vec4(a_pos,1.0);\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 330 core\n"
    "in vec3 v_nrm;\n"
    "in vec3 v_col;\n"
    "in vec3 v_world;\n"
    "uniform vec3 u_color;\n"
    "uniform vec3 u_light_dir;\n"
    "uniform vec3 u_cam_pos;\n"
    "uniform int u_use_vert_color;\n"
    "uniform float u_alpha;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  vec3 n = normalize(v_nrm);\n"
    "  vec3 l = normalize(u_light_dir);\n"
    "  float diff = max(dot(n,l), 0.0);\n"
    "  vec3 view = normalize(u_cam_pos - v_world);\n"
    "  float fill = max(dot(n, view), 0.0) * 0.35;   /* headlight, keeps thin parts lit */\n"
    "  vec3 h = normalize(l + view);\n"
    "  float spec = pow(max(dot(n,h),0.0), 28.0) * 0.18;\n"
    "  vec3 base = (u_use_vert_color != 0) ? v_col : u_color;\n"
    "  frag = vec4(base * (0.30 + diff + fill) + vec3(spec), u_alpha);\n"
    "}\n";

static unsigned compile_shader(GLenum type, const char *src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

static int gl_init(CreatorGL *g) {
    memset(g, 0, sizeof(*g));
    unsigned vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    unsigned fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) return 0;
    g->prog = glCreateProgram();
    glAttachShader(g->prog, vs);
    glAttachShader(g->prog, fs);
    glLinkProgram(g->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    int ok = 0;
    glGetProgramiv(g->prog, GL_LINK_STATUS, &ok);
    if (!ok) return 0;

    g->u_mvp = glGetUniformLocation(g->prog, "u_mvp");
    g->u_model = glGetUniformLocation(g->prog, "u_model");
    g->u_color = glGetUniformLocation(g->prog, "u_color");
    g->u_light_dir = glGetUniformLocation(g->prog, "u_light_dir");
    g->u_cam_pos = glGetUniformLocation(g->prog, "u_cam_pos");
    g->u_use_vert_color = glGetUniformLocation(g->prog, "u_use_vert_color");
    g->u_alpha = glGetUniformLocation(g->prog, "u_alpha");

    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->vbo);
    glGenBuffers(1, &g->ebo);
    glGenVertexArrays(1, &g->bone_vao);
    glGenBuffers(1, &g->bone_vbo);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    return 1;
}

static void gl_shutdown(CreatorGL *g) {
    if (g->ebo) glDeleteBuffers(1, &g->ebo);
    if (g->vbo) glDeleteBuffers(1, &g->vbo);
    if (g->vao) glDeleteVertexArrays(1, &g->vao);
    if (g->bone_vbo) glDeleteBuffers(1, &g->bone_vbo);
    if (g->bone_vao) glDeleteVertexArrays(1, &g->bone_vao);
    if (g->prog) glDeleteProgram(g->prog);
}

static void state_reset(CreatorState *st) {
    memset(st, 0, sizeof(*st));
    spine_init_blob(&st->spine);
    /* Lochner Start(): two bones — blob already has 4; keep as amorphous starter */
    st->genome = genome_starter();
    st->color = v3(0.92f, 0.78f, 0.55f);
    st->color2 = v3(0.95f, 0.88f, 0.72f);
    st->pattern = PATTERN_NONE;
    st->scale = 1.0f;
    snprintf(st->name, sizeof(st->name), "Blobert");
    st->cash = 1000; /* Lochner startingCash */
    st->mode = MODE_BUILD;
    st->sel_bone = 1;
    st->part_slot = PART_SLOT_MOUTH;
    st->dirty_mesh = 1;
    bodymesh_defaults(&st->mesh_set);
    st->merge_threshold = 0.08f;
    for (int i = 0; i < CREATURE_LEGS; i++) {
        st->limb_x[i] = 0;
        st->limb_y[i] = 0;
        st->limb_z[i] = 0;
        st->limb_bone[i] = -1;
    }
    sync_mirrored_limbs(st);
    terrain_init(&st->terrain, 256.0f, -0.35f, 1234u);
    st->test_live = 0;
}

/* Drop the current build onto the terrain as a living creature the Test tab
 * can drive. The sculpted column is handed to it verbatim, so what you walk
 * is exactly what you built - not a genome approximation of it. */
static void enter_test(CreatorState *st) {
    Genome g = st->genome;
    genome_clamp(&g);
    creature_init(&st->testc, &st->terrain,
                  v3(st->terrain.world_size * 0.5f, 0.0f,
                     st->terrain.world_size * 0.5f),
                  g, st->color, st->scale, 1);
    if (st->spine.active && st->spine.count >= SPINE_MIN_VERTS) {
        st->testc.spine = st->spine;
        spine_sync_anchors(&st->testc);
    } else {
        spine_ensure_blob(&st->testc);
    }
    st->test_live = 1;
}

static void set_mode(CreatorState *st, int mode) {
    if (mode == st->mode) return;
    st->mode = mode;
    if (mode == MODE_TEST) enter_test(st);
    else st->test_live = 0;
}

static void rebuild_mesh(CreatorState *st) {
    bodymesh_build(&st->mesh, &st->spine, &st->mesh_set, st->color, st->color2);
    st->dirty_mesh = 0;
}

static void upload_mesh(CreatorGL *g, const CreatorState *st) {
    const BodyMesh *m = &st->mesh;
    if (m->count < 3 || m->idx_count < 3) return;

    float *inter = malloc((size_t)m->count * 9 * sizeof(float));
    if (!inter) return;
    for (int i = 0; i < m->count; i++) {
        float ny = m->nrm[i * 3 + 1];
        float belly = clampf((-ny + 0.15f) * 1.4f, 0.0f, 1.0f);
        Vec3 col = v3_lerp(st->color, st->color2, belly * 0.55f);
        if (st->pattern == PATTERN_STRIPES && ((i / 3) % 4) < 2)
            col = v3_mul(col, 0.72f);
        else if (st->pattern == PATTERN_SPOTS && ((i * 7) % 11) < 3)
            col = v3_mul(col, 0.65f);
        else if (st->pattern == PATTERN_BANDS && ((int)(m->pos[i * 3 + 2] * 8.0f) % 2) == 0)
            col = v3_mul(col, 0.78f);
        inter[i * 9 + 0] = m->pos[i * 3 + 0];
        inter[i * 9 + 1] = m->pos[i * 3 + 1];
        inter[i * 9 + 2] = m->pos[i * 3 + 2];
        inter[i * 9 + 3] = m->nrm[i * 3 + 0];
        inter[i * 9 + 4] = m->nrm[i * 3 + 1];
        inter[i * 9 + 5] = m->nrm[i * 3 + 2];
        inter[i * 9 + 6] = col.x;
        inter[i * 9 + 7] = col.y;
        inter[i * 9 + 8] = col.z;
    }

    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m->count * 9 * (int)sizeof(float)),
                 inter, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(m->idx_count * (int)sizeof(unsigned)),
                 m->idx, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(6 * sizeof(float)));
    glBindVertexArray(0);
    free(inter);
}

static void draw_mesh(CreatorGL *g, const CreatorState *st, Mat4 vp, Vec3 eye,
                      Mat4 model) {
    if (st->mesh.count < 3) return;
    Mat4 mvp = m4_mul(vp, model);
    glUseProgram(g->prog);
    glUniformMatrix4fv(g->u_mvp, 1, GL_FALSE, mvp.m);
    glUniformMatrix4fv(g->u_model, 1, GL_FALSE, model.m);
    glUniform3f(g->u_color, st->color.x, st->color.y, st->color.z);
    glUniform3f(g->u_light_dir, 0.35f, 0.85f, 0.25f);
    glUniform3f(g->u_cam_pos, eye.x, eye.y, eye.z);
    glUniform1i(g->u_use_vert_color, 1);
    glUniform1f(g->u_alpha, 1.0f);
    glBindVertexArray(g->vao);
    glDrawElements(GL_TRIANGLES, st->mesh.idx_count, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
}

static void draw_parts_gizmos(CreatorGL *g, const CreatorState *st, Mat4 vp, Vec3 eye) {
    /* Place simple marker cubes at mouth / eye / limb sockets on spine */
    if (st->mode == MODE_PAINT) return;
    int segs = st->spine.count;
    if (segs < 2) return;

    typedef struct { float x, y, z; float sx, sy, sz; Vec3 col; } Gizmo;
    Gizmo gz[12];
    int ng = 0;

    /* Mouth — offset below nose so it doesn't steal the red stretch handle */
    gz[ng++] = (Gizmo){st->spine.v[0].x, st->spine.v[0].y - st->spine.v[0].radius * 0.55f,
                       st->spine.v[0].z + st->spine.v[0].radius * 0.15f,
                       0.14f, 0.10f, 0.12f, v3(0.85f, 0.45f, 0.35f)};
    /* Eyes */
    float er = st->spine.v[0].radius * 0.55f;
    gz[ng++] = (Gizmo){st->spine.v[0].x - er, st->spine.v[0].y + er * 0.4f,
                       st->spine.v[0].z, 0.08f, 0.08f, 0.08f, v3(0.15f, 0.15f, 0.2f)};
    gz[ng++] = (Gizmo){st->spine.v[0].x + er, st->spine.v[0].y + er * 0.4f,
                       st->spine.v[0].z, 0.08f, 0.08f, 0.08f, v3(0.15f, 0.15f, 0.2f)};

    /* Legs are drawn as actual bones now (see draw_limbs), not markers. */

    /* Detail marker along spine (Lochner nearest-bone attach approximation) */
    if (st->genome.detail != DETAIL_NONE) {
        float u = (float)st->genome.dtl_slide / (float)(SLIDE_COUNT - 1);
        int vi = (int)(u * (float)(segs - 1) + 0.5f);
        gz[ng++] = (Gizmo){st->spine.v[vi].x, st->spine.v[vi].y + st->spine.v[vi].radius,
                           st->spine.v[vi].z, 0.12f, 0.12f, 0.16f, v3(0.7f, 0.5f, 0.2f)};
    }

    glUseProgram(g->prog);
    glUniform3f(g->u_light_dir, 0.3f, 0.9f, 0.2f);
    glUniform3f(g->u_cam_pos, eye.x, eye.y, eye.z);
    glUniform1i(g->u_use_vert_color, 0);
    glUniform1f(g->u_alpha, 1.0f);

    /* Full unit cube (36 tris) for part gizmos */
    static const float cube_pos[] = {
        /* -Z */ -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,0.5f,-0.5f,
                  0.5f,0.5f,-0.5f, -0.5f,0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
        /* +Z */ -0.5f,-0.5f, 0.5f,  0.5f,0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
                 -0.5f,-0.5f, 0.5f, -0.5f,0.5f, 0.5f,  0.5f,0.5f, 0.5f,
        /* +X */  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f,  0.5f,0.5f, 0.5f,
                  0.5f,0.5f, 0.5f,  0.5f,0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
        /* -X */ -0.5f,-0.5f,-0.5f, -0.5f,0.5f,-0.5f, -0.5f,0.5f, 0.5f,
                 -0.5f,0.5f, 0.5f, -0.5f,-0.5f, 0.5f, -0.5f,-0.5f,-0.5f,
        /* +Y */ -0.5f,0.5f,-0.5f,  0.5f,0.5f,-0.5f,  0.5f,0.5f, 0.5f,
                  0.5f,0.5f, 0.5f, -0.5f,0.5f, 0.5f, -0.5f,0.5f,-0.5f,
        /* -Y */ -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
                  0.5f,-0.5f, 0.5f,  0.5f,-0.5f,-0.5f, -0.5f,-0.5f,-0.5f,
    };
    /* one outward normal per face (6 verts each): -Z, +Z, +X, -X, +Y, -Y */
    static const float face_nrm[6][3] = {
        { 0, 0,-1 }, { 0, 0, 1 }, { 1, 0, 0 },
        { -1, 0, 0 }, { 0, 1, 0 }, { 0,-1, 0 },
    };
    float inter[36 * 9];
    for (int i = 0; i < 36; i++) {
        int face = i / 6;
        inter[i * 9 + 0] = cube_pos[i * 3 + 0];
        inter[i * 9 + 1] = cube_pos[i * 3 + 1];
        inter[i * 9 + 2] = cube_pos[i * 3 + 2];
        inter[i * 9 + 3] = face_nrm[face][0];
        inter[i * 9 + 4] = face_nrm[face][1];
        inter[i * 9 + 5] = face_nrm[face][2];
        inter[i * 9 + 6] = inter[i * 9 + 7] = inter[i * 9 + 8] = 1;
    }
    glBindVertexArray(g->bone_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->bone_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(inter), inter, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(6 * sizeof(float)));

    for (int i = 0; i < ng; i++) {
        Mat4 T = m4_translate(v3(gz[i].x, gz[i].y, gz[i].z));
        Mat4 S = m4_scale(v3(gz[i].sx, gz[i].sy, gz[i].sz));
        Mat4 model = m4_mul(T, S);
        Mat4 mvp = m4_mul(vp, model);
        glUniformMatrix4fv(g->u_mvp, 1, GL_FALSE, mvp.m);
        glUniformMatrix4fv(g->u_model, 1, GL_FALSE, model.m);
        glUniform3f(g->u_color, gz[i].col.x, gz[i].col.y, gz[i].col.z);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);
}

/* Limbs as tapered bones. In Build/Paint the sockets are body-local; in Test
 * the creature has already solved its own IK in world space. */
static void draw_limbs(CreatorGL *g, const CreatorState *st, Mat4 vp, Vec3 eye) {
    glDisable(GL_CULL_FACE);
    glUseProgram(g->prog);
    upload_unit_cube(g);

    Vec3 col = v3(0.80f, 0.76f, 0.68f);
    Vec3 col2 = v3(0.68f, 0.64f, 0.57f);
    if (st->mode == MODE_TEST && st->test_live) {
        const Creature *c = &st->testc;
        int legs = genome_limb_count(&c->genome);
        float s = c->scale;
        for (int i = 0; i < legs && i < CREATURE_LEGS; i++) {
            const Leg *L = &c->legs[i];
            draw_box_seg(g, vp, eye, L->hip, L->knee, 0.060f * s, col);
            draw_box_seg(g, vp, eye, L->knee, L->foot, 0.048f * s, col2);
        }
    } else {
        int legs = genome_limb_count(&st->genome);
        int segs = st->spine.count;
        int pairs = legs / 2;
        if (pairs < 1) pairs = 1;
        for (int p = 0; p < pairs && p * 2 < CREATURE_LEGS; p++) {
            int idx = p * 2;
            int vi = st->limb_bone[idx];
            if (vi < 0 || vi >= segs) continue;
            Vec3 base = v3(st->spine.v[vi].x, st->spine.v[vi].y, st->spine.v[vi].z);
            float lx = fabsf(st->limb_x[idx]);
            float ly = st->limb_y[idx];
            float lz = st->limb_z[idx];
            Vec3 hips[2] = { v3(base.x + lx, base.y + ly, lz),
                             v3(base.x - lx, base.y + ly, lz) };
            for (int k = 0; k < 2; k++) {
                Vec3 hip = hips[k];
                Vec3 foot = v3(hip.x, hip.y - 0.62f, hip.z);
                Vec3 knee = ik_2bone(hip, foot, 0.34f, 0.34f,
                                     v3(hip.x, hip.y - 1.0f, hip.z));
                draw_box_seg(g, vp, eye, hip, knee, 0.060f, col);
                draw_box_seg(g, vp, eye, knee, foot, 0.048f, col2);
            }
        }
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

/* A wire ground sampled from the same height field the creature walks. */
static void draw_ground(CreatorGL *g, const CreatorState *st, Mat4 vp, Vec3 eye,
                        Vec3 center) {
    const int N = 12;
    const float step = 3.0f;
    int lines = 2 * N + 1;
    int maxv = lines * (2 * N) * 2 * 2;
    float *buf = malloc((size_t)maxv * 9 * sizeof(float));
    if (!buf) return;
    int vi = 0;
    for (int dir = 0; dir < 2; dir++) {
        for (int li = -N; li <= N; li++) {
            float fixed = (dir == 0 ? center.x : center.z) + (float)li * step;
            Vec3 prev = v3(0, 0, 0);
            for (int s = -N; s <= N; s++) {
                float t = (dir == 0 ? center.z : center.x) + (float)s * step;
                Vec3 p = dir == 0 ? v3(fixed, 0, t) : v3(t, 0, fixed);
                p.y = terrain_height(&st->terrain, p.x, p.z) + 0.01f;
                if (s > -N && vi + 2 <= maxv) {
                    float *a = &buf[(size_t)vi * 9];
                    a[0] = prev.x; a[1] = prev.y; a[2] = prev.z;
                    a[3] = 0; a[4] = 1; a[5] = 0; a[6] = a[7] = a[8] = 1;
                    vi++;
                    float *b = &buf[(size_t)vi * 9];
                    b[0] = p.x; b[1] = p.y; b[2] = p.z;
                    b[3] = 0; b[4] = 1; b[5] = 0; b[6] = b[7] = b[8] = 1;
                    vi++;
                }
                prev = p;
            }
        }
    }
    glUseProgram(g->prog);
    glUniformMatrix4fv(g->u_mvp, 1, GL_FALSE, vp.m);
    glUniformMatrix4fv(g->u_model, 1, GL_FALSE, m4_id().m);
    glUniform3f(g->u_color, 0.16f, 0.22f, 0.16f);
    glUniform3f(g->u_light_dir, 0.35f, 0.9f, 0.25f);
    glUniform3f(g->u_cam_pos, eye.x, eye.y, eye.z);
    glUniform1i(g->u_use_vert_color, 0);
    glUniform1f(g->u_alpha, 1.0f);
    glBindVertexArray(g->bone_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->bone_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)((size_t)vi * 9 * sizeof(float)),
                 buf, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(6 * sizeof(float)));
    glDrawArrays(GL_LINES, 0, vi);
    glBindVertexArray(0);
    free(buf);
}

/* Weapon and detail sit where the simulation would put them, so the parts you
 * paid for are visible on the test drive. */
static void draw_test_markers(CreatorGL *g, const CreatorState *st, Mat4 vp, Vec3 eye) {
    if (st->mode != MODE_TEST || !st->test_live) return;
    const Creature *c = &st->testc;
    if (c->genome.weapon == WEAPON_NONE && c->genome.detail == DETAIL_NONE) return;
    float s = c->scale;
    glDisable(GL_CULL_FACE);
    glUseProgram(g->prog);
    upload_unit_cube(g);
    if (c->genome.weapon != WEAPON_NONE) {
        Vec3 p = creature_weapon_anchor(c);
        draw_cube_model(g, vp, eye,
                        m4_mul(m4_translate(p), m4_scale(v3(0.14f * s, 0.14f * s, 0.22f * s))),
                        v3(0.85f, 0.45f, 0.30f));
    }
    if (c->genome.detail != DETAIL_NONE) {
        Vec3 p = creature_detail_anchor(c);
        draw_cube_model(g, vp, eye,
                        m4_mul(m4_translate(p), m4_scale(v3(0.18f * s, 0.10f * s, 0.30f * s))),
                        v3(0.40f, 0.70f, 0.45f));
    }
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
}

static int save_creature(const CreatorState *st, const char *path) {
    CreatureDesc d;
    desc_defaults(&d);
    snprintf(d.name, sizeof(d.name), "%s", st->name);
    d.genome = st->genome;
    d.color = st->color;
    d.color2 = st->color2;
    d.pattern = st->pattern;
    d.scale = st->scale;
    d.spine = st->spine;               /* the sculpted column is the creature */
    /* Keep the genome's spine morph in step with the column's length. */
    float span = 0.0f;
    if (st->spine.count >= 2)
        span = fabsf(st->spine.v[0].z - st->spine.v[st->spine.count - 1].z);
    if (span > 1.6f) d.genome.spine = SPINE_SERPENTINE;
    else if (span > 1.1f) d.genome.spine = SPINE_LONG;
    else if (span < 0.7f) d.genome.spine = SPINE_COMPACT;
    else d.genome.spine = SPINE_STANDARD;
    return desc_save(&d, path);
}

static int load_creature(CreatorState *st, const char *path) {
    CreatureDesc d;
    if (!desc_load(&d, path)) return 0;
    st->genome = d.genome;
    st->color = d.color;
    st->color2 = d.color2;
    st->pattern = d.pattern;
    st->scale = d.scale;
    snprintf(st->name, sizeof(st->name), "%s", d.name);
    if (d.spine.active && d.spine.count >= SPINE_MIN_VERTS) {
        st->spine = d.spine;
        if (st->sel_bone >= st->spine.count) st->sel_bone = st->spine.count - 1;
        if (st->sel_bone < 0) st->sel_bone = 0;
    } else {
        spine_init_blob(&st->spine);
        st->sel_bone = 1;
    }
    sync_mirrored_limbs(st);
    st->dirty_mesh = 1;
    st->test_live = 0;
    if (st->mode == MODE_TEST) enter_test(st);
    return 1;
}

static void print_hud(const CreatorState *st) {
    const char *mode = st->mode == MODE_BUILD ? "BUILD"
                     : st->mode == MODE_PAINT ? "PAINT" : "TEST";
    int cx = genome_complexity(&st->genome) + st->spine.count; /* bones cost complexity */
    printf("\r[%s] %s | bones %d sel %d | $%d | complexity %d/%d | %s/%s/%s    ",
           mode, st->name, st->spine.count, st->sel_bone, st->cash, cx, COMPLEXITY_MAX,
           unlocks_part_name(PART_SLOT_MOUTH, st->genome.mouth),
           unlocks_part_name(PART_SLOT_LEGS, st->genome.legs),
           unlocks_part_name(PART_SLOT_EYES, st->genome.eyes));
    fflush(stdout);
}

static void unlock_all_parts(PartUnlocks *u) {
    memset(u, 0, sizeof(*u));
    for (int s = 0; s <= PART_SLOT_DETAIL; s++) {
        int max = (s == PART_SLOT_MOUTH) ? MOUTH_COUNT
                : (s == PART_SLOT_LEGS) ? LEGS_COUNT
                : (s == PART_SLOT_WEAPON) ? WEAPON_COUNT
                : (s == PART_SLOT_ABILITY) ? ABILITY_COUNT
                : (s == PART_SLOT_EYES) ? EYES_COUNT
                : (s == PART_SLOT_GRASPER) ? GRASPER_COUNT
                : DETAIL_COUNT;
        for (int i = 0; i < max; i++) unlocks_grant(u, s, i);
    }
}

static void cycle_part(CreatorState *st, int dir) {
    PartUnlocks all;
    unlock_all_parts(&all);
    int cur = 0;
    switch (st->part_slot) {
    case PART_SLOT_MOUTH: cur = st->genome.mouth; break;
    case PART_SLOT_LEGS: cur = st->genome.legs; break;
    case PART_SLOT_WEAPON: cur = st->genome.weapon; break;
    case PART_SLOT_ABILITY: cur = st->genome.ability; break;
    case PART_SLOT_EYES: cur = st->genome.eyes; break;
    case PART_SLOT_GRASPER: cur = st->genome.grasper; break;
    case PART_SLOT_DETAIL: cur = st->genome.detail; break;
    }
    int next = unlocks_cycle(&all, st->part_slot, cur);
    if (dir < 0) {
        /* cycle twice-ish backward: keep calling until wrap */
        for (int i = 0; i < 8; i++) next = unlocks_cycle(&all, st->part_slot, next);
    }
    switch (st->part_slot) {
    case PART_SLOT_MOUTH: st->genome.mouth = next; break;
    case PART_SLOT_LEGS: st->genome.legs = next; break;
    case PART_SLOT_WEAPON: st->genome.weapon = next; break;
    case PART_SLOT_ABILITY: st->genome.ability = next; break;
    case PART_SLOT_EYES: st->genome.eyes = next; break;
    case PART_SLOT_GRASPER: st->genome.grasper = next; break;
    case PART_SLOT_DETAIL: st->genome.detail = next; break;
    }
    genome_clamp(&st->genome);
}

/* --- headless still export: render a frame, read it back, write a PPM --- */
static void write_ppm(const char *path, int w, int h) {
    unsigned char *px = malloc((size_t)w * (size_t)h * 3);
    if (!px) return;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--)   /* GL origin is bottom-left */
            fwrite(px + (size_t)y * (size_t)w * 3, 1, (size_t)w * 3, f);
        fclose(f);
    }
    free(px);
}

int main(int argc, char **argv) {
    const char *out_path = "data/creatures/creator_mvp.creature";
    const char *shot_path = NULL;
    int start_mode = MODE_BUILD;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            start_mode = strcmp(m, "paint") == 0 ? MODE_PAINT
                       : strcmp(m, "test") == 0 ? MODE_TEST : MODE_BUILD;
        } else {
            out_path = argv[i];
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *win = SDL_CreateWindow(
        "cpore creator — Lochner MVP (Build / Paint / Test)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "gl: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);

    CreatorGL gl;
    if (!gl_init(&gl)) { fprintf(stderr, "gl init failed\n"); return 1; }

    CreatorState st;
    state_reset(&st);
    rebuild_mesh(&st);
    upload_mesh(&gl, &st);
    set_mode(&st, start_mode);

    float cam_yaw = 0.6f, cam_pitch = 0.35f, cam_dist = 4.5f;
    int running = 1;
    int shot_done = 0;
    int dragging = 0;
    int bone_drag = 0;
    int stretch_front = 0, stretch_back = 0;
    float stretch_acc = 0.0f;
    Mat4 last_vp = m4_id();

    printf("Lochner Creature Creator MVP (MIT algorithms)\n");
    printf("  1/2/3     Build / Paint / Test\n");
    printf("  LMB       drag RED/GREEN tips — column curves toward mouse & grows\n");
    printf("            drag gold bones to bend; empty drag = orbit\n");
    printf("  scroll    inflate selected bone (neighbor bleed)\n");
    printf("  = / -     extend / shorten (Shift = front)\n");
    printf("  Tab Q/E   part slot / cycle     C/P paint\n");
    printf("  TEST: WASD walk, Space jump\n");
    printf("  R reset   S save   L load   (%s)\n", out_path);
    printf("  Esc quit\n\n");

    Uint32 last = SDL_GetTicks();
    while (running) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - last) / 1000.0f;
        if (dt > 0.05f) dt = 0.05f;
        last = now;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                int sh = (e.key.keysym.mod & KMOD_SHIFT) != 0;
                if (k == SDLK_ESCAPE) running = 0;
                else if (k == SDLK_1) set_mode(&st, MODE_BUILD);
                else if (k == SDLK_2) set_mode(&st, MODE_PAINT);
                else if (k == SDLK_3) set_mode(&st, MODE_TEST);
                else if (k == SDLK_r) {
                    int was = st.mode;
                    state_reset(&st);
                    st.dirty_mesh = 1;
                    if (was != st.mode) set_mode(&st, was);
                } else if (k == SDLK_s) {
                    if (save_creature(&st, out_path))
                        printf("\nsaved %s\n", out_path);
                } else if (k == SDLK_l) {
                    if (load_creature(&st, out_path))
                        printf("\nloaded %s\n", out_path);
                    else
                        printf("\ncould not load %s\n", out_path);
                } else if (k == SDLK_LEFTBRACKET) {
                    if (st.sel_bone > 0) st.sel_bone--;
                } else if (k == SDLK_RIGHTBRACKET) {
                    if (st.sel_bone < st.spine.count - 1) st.sel_bone++;
                } else if (k == SDLK_EQUALS || k == SDLK_PLUS) {
                    if (spine_extend(&st.spine, sh ? 1 : 0)) {
                        st.sel_bone = sh ? 0 : st.spine.count - 1;
                        st.dirty_mesh = 1;
                        st.cash -= 10;
                    }
                } else if (k == SDLK_MINUS) {
                    if (spine_shorten(&st.spine, sh ? 1 : 0)) {
                        if (st.sel_bone >= st.spine.count)
                            st.sel_bone = st.spine.count - 1;
                        st.dirty_mesh = 1;
                        st.cash += 5;
                    }
                } else if (k == SDLK_TAB) {
                    st.part_slot = (st.part_slot + 1) % 7;
                } else if (k == SDLK_q) cycle_part(&st, -1);
                else if (k == SDLK_e) cycle_part(&st, +1);
                else if (k == SDLK_c && st.mode == MODE_PAINT) {
                    st.color = v3(0.35f + (st.color.x * 1.7f),
                                  0.25f + fmodf(st.color.y * 2.3f, 0.7f),
                                  0.20f + fmodf(st.color.z * 1.9f, 0.7f));
                    st.color2 = v3(st.color.x * 0.7f + 0.25f,
                                   st.color.y * 0.7f + 0.2f,
                                   st.color.z * 0.7f + 0.15f);
                    st.dirty_mesh = 1;
                } else if (k == SDLK_p && st.mode == MODE_PAINT) {
                    st.pattern = (st.pattern + 1) % PATTERN_COUNT;
                    st.dirty_mesh = 1;
                }
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                dragging = 1;
                bone_drag = 0;
                int mx = e.button.x, my = e.button.y;
                int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
                stretch_front = stretch_back = 0;
                stretch_acc = 0;
                if (st.mode == MODE_BUILD) {
                    int hit = pick_tool(&st, last_vp, mx, my, ww, wh);
                    if (hit == -2) {
                        stretch_front = 1;
                    } else if (hit == -3) {
                        stretch_back = 1;
                    } else if (hit >= 0) {
                        st.sel_bone = hit;
                        bone_drag = 1;
                    }
                }
            }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                dragging = 0;
                bone_drag = 0;
                stretch_front = stretch_back = 0;
                stretch_acc = 0;
            }
            if (e.type == SDL_MOUSEMOTION && dragging) {
                float dx = (float)e.motion.xrel;
                float dy = (float)e.motion.yrel;
                if (stretch_front || stretch_back) {
                    /* Lochner: tip follows mouse on the view plane; far → grow, near → shrink. */
                    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
                    int mx = e.motion.x, my = e.motion.y;
                    float side = (st.mode == MODE_BUILD) ? -0.6f : 0.0f;
                    Vec3 eye, target, cam_r, cam_u, cam_f;
                    cam_basis(cam_yaw, cam_pitch, cam_dist, side,
                              &eye, &target, &cam_r, &cam_u, &cam_f);
                    int tip_i = stretch_front ? 0 : st.spine.count - 1;
                    int prev_i = stretch_front ? 1 : st.spine.count - 2;
                    if (prev_i < 0) prev_i = 0;
                    if (prev_i >= st.spine.count) prev_i = st.spine.count - 1;
                    Vec3 tip = v3(st.spine.v[tip_i].x, st.spine.v[tip_i].y, st.spine.v[tip_i].z);
                    Vec3 prev = v3(st.spine.v[prev_i].x, st.spine.v[prev_i].y, st.spine.v[prev_i].z);
                    float aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
                    float fovy = 50.0f * (float)M_PI / 180.0f;
                    Vec3 hit;
                    if (mouse_hit_plane(eye, cam_r, cam_u, cam_f, fovy, aspect,
                                        mx, my, ww, wh, tip, cam_f, &hit)) {
                        hit.x = 0.0f; /* bilateral: sagittal plane only */
                        const float seg = 0.22f;
                        Vec3 to = v3_sub(hit, prev);
                        to.x = 0.0f;
                        float dist = v3_len(to);
                        if (dist > 1e-4f) {
                            Vec3 dir = v3_mul(to, 1.0f / dist);
                            /* Curve tip toward mouse every frame */
                            spine_aim_end(&st.spine, stretch_front ? 1 : 0,
                                          hit.x, hit.y, hit.z, seg);
                            spine_enforce_symmetry(&st.spine);
                            st.dirty_mesh = 1;
                            sync_mirrored_limbs(&st);

                            if (dist > seg * 1.45f) {
                                if (spine_extend_dir(&st.spine, stretch_front ? 1 : 0,
                                                     dir.x, dir.y, dir.z)) {
                                    tip_i = stretch_front ? 0 : st.spine.count - 1;
                                    st.sel_bone = tip_i;
                                    /* Aim the brand-new tip at the mouse */
                                    spine_aim_end(&st.spine, stretch_front ? 1 : 0,
                                                  hit.x, hit.y, hit.z, seg);
                                    spine_enforce_symmetry(&st.spine);
                                    st.cash -= 10;
                                    st.dirty_mesh = 1;
                                    sync_mirrored_limbs(&st);
                                }
                            } else if (dist < seg * 0.55f) {
                                if (spine_shorten(&st.spine, stretch_front ? 1 : 0)) {
                                    if (st.sel_bone >= st.spine.count)
                                        st.sel_bone = st.spine.count - 1;
                                    st.cash += 5;
                                    st.dirty_mesh = 1;
                                    sync_mirrored_limbs(&st);
                                }
                            }
                        }
                    } else {
                        /* Fallback: relative drag if ray miss */
                        spine_bend(&st.spine, tip_i, 0, -dy * 0.004f, 0);
                        spine_enforce_symmetry(&st.spine);
                        st.dirty_mesh = 1;
                    }
                    (void)stretch_acc;
                } else if (bone_drag && st.mode == MODE_BUILD) {
                    /* Bend selected vertebra toward mouse on the sagittal plane */
                    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
                    int mx = e.motion.x, my = e.motion.y;
                    float side = -0.6f;
                    Vec3 eye, target, cam_r, cam_u, cam_f;
                    cam_basis(cam_yaw, cam_pitch, cam_dist, side,
                              &eye, &target, &cam_r, &cam_u, &cam_f);
                    Vec3 bone = v3(0.0f,
                                   st.spine.v[st.sel_bone].y,
                                   st.spine.v[st.sel_bone].z);
                    float aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
                    float fovy = 50.0f * (float)M_PI / 180.0f;
                    Vec3 hit;
                    if (mouse_hit_plane(eye, cam_r, cam_u, cam_f, fovy, aspect,
                                        mx, my, ww, wh, bone, cam_f, &hit)) {
                        st.spine.v[st.sel_bone].x = 0.0f;
                        st.spine.v[st.sel_bone].y = clampf(hit.y, -0.8f, 1.4f);
                        st.spine.v[st.sel_bone].z = clampf(hit.z, -2.2f, 2.2f);
                    } else {
                        spine_bend(&st.spine, st.sel_bone, 0, -dy * 0.004f, dy * 0.0015f);
                    }
                    spine_enforce_symmetry(&st.spine);
                    st.dirty_mesh = 1;
                    sync_mirrored_limbs(&st);
                } else {
                    cam_yaw += dx * 0.007f;
                    cam_pitch = clampf(cam_pitch + dy * 0.005f, -0.2f, 1.2f);
                }
            }
            if (e.type == SDL_MOUSEWHEEL) {
                if (st.mode == MODE_BUILD && st.sel_bone >= 0) {
                    /* Lochner scroll inflate with neighbor bleed */
                    float d = (e.wheel.y > 0) ? 0.06f : -0.06f;
                    bodymesh_add_weight(&st.spine, st.sel_bone, d, 0);
                    st.dirty_mesh = 1;
                } else {
                    cam_dist = clampf(cam_dist - (float)e.wheel.y * 0.35f, 2.0f, 12.0f);
                }
            }
        }

        if (st.mode == MODE_TEST && st.test_live) {
            const Uint8 *ks = SDL_GetKeyboardState(NULL);
            float fwd_in = 0.0f, side_in = 0.0f;
            if (ks[SDL_SCANCODE_W]) fwd_in += 1.0f;
            if (ks[SDL_SCANCODE_S]) fwd_in -= 1.0f;
            if (ks[SDL_SCANCODE_A]) side_in -= 1.0f;
            if (ks[SDL_SCANCODE_D]) side_in += 1.0f;
            float cyw = cosf(cam_yaw), syw = sinf(cam_yaw);
            Vec3 fdir = v3(-syw, 0.0f, -cyw);   /* where the camera looks, on the ground */
            Vec3 rdir = v3(cyw, 0.0f, -syw);
            Vec3 wish = v3_add(v3_mul(fdir, fwd_in), v3_mul(rdir, side_in));
            creature_update_locomotion(&st.testc, &st.terrain, dt,
                                       wish.x, wish.z,
                                       ks[SDL_SCANCODE_SPACE] ? 1 : 0);
        }

        if (st.dirty_mesh) {
            rebuild_mesh(&st);
            upload_mesh(&gl, &st);
        }

        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        glViewport(0, 0, ww, wh);
        glClearColor(0.12f, 0.14f, 0.18f, 1.0f);
        if (st.mode == MODE_PAINT) glClearColor(0.18f, 0.16f, 0.14f, 1.0f);
        if (st.mode == MODE_TEST) glClearColor(0.32f, 0.42f, 0.55f, 1.0f); /* sky */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
        float cp = cosf(cam_pitch), sp = sinf(cam_pitch);
        float cy = cosf(cam_yaw), sy = sinf(cam_yaw);
        Vec3 target;
        if (st.mode == MODE_TEST && st.test_live) {
            target = st.testc.body;
            target.y += 0.15f;
        } else {
            /* Lochner Build offsets the camera slightly left */
            float side = (st.mode == MODE_BUILD) ? -0.6f
                       : (st.mode == MODE_PAINT ? 0.6f : 0.0f);
            target = v3(side, 0.4f, 0.0f);
        }
        Vec3 eye = v3_add(target, v3(sy * cp * cam_dist, sp * cam_dist + 0.8f, cy * cp * cam_dist));
        Mat4 view = m4_look_at(eye, target, v3(0, 1, 0));
        Mat4 proj = m4_perspective(50.0f * (float)M_PI / 180.0f, aspect, 0.05f, 80.0f);
        Mat4 vp = m4_mul(proj, view);
        last_vp = vp;

        Mat4 mesh_model = m4_id();
        if (st.mode == MODE_TEST && st.test_live) {
            const Creature *c = &st.testc;
            mesh_model = m4_mul(m4_translate(c->body),
                         m4_mul(m4_rotate_y(c->yaw),
                                m4_scale(v3(c->scale, c->scale, c->scale))));
            draw_ground(&gl, &st, vp, eye, st.testc.body);
        }
        draw_mesh(&gl, &st, vp, eye, mesh_model);
        if (st.mode == MODE_BUILD) {
            draw_bones(&gl, &st, vp, eye);
            draw_limbs(&gl, &st, vp, eye);
            draw_parts_gizmos(&gl, &st, vp, eye);
        } else if (st.mode == MODE_TEST) {
            draw_limbs(&gl, &st, vp, eye);
            draw_test_markers(&gl, &st, vp, eye);
        }

        /* Mode tab HUD bars */
        {
            Mat4 hud = m4_id();
            hud.m[0] = 2.0f / (float)ww;
            hud.m[5] = -2.0f / (float)wh;
            hud.m[10] = -1.0f;
            hud.m[12] = -1.0f;
            hud.m[13] = 1.0f;
            hud.m[15] = 1.0f;
            float quad[] = {
                -0.5f,-0.5f,0, 0.5f,-0.5f,0, 0.5f,0.5f,0,
                 0.5f,0.5f,0,-0.5f,0.5f,0,-0.5f,-0.5f,0
            };
            float inter[6 * 9];
            for (int i = 0; i < 6; i++) {
                inter[i * 9] = quad[i * 3];
                inter[i * 9 + 1] = quad[i * 3 + 1];
                inter[i * 9 + 2] = 0;
                inter[i * 9 + 3] = 0; inter[i * 9 + 4] = 0; inter[i * 9 + 5] = 1;
                inter[i * 9 + 6] = inter[i * 9 + 7] = inter[i * 9 + 8] = 1;
            }
            glDisable(GL_DEPTH_TEST);
            glUseProgram(gl.prog);
            glUniform1i(gl.u_use_vert_color, 0);
            glUniform1f(gl.u_alpha, 1.0f);
            glUniform3f(gl.u_light_dir, 0, 0, 1);
            glUniform3f(gl.u_cam_pos, 0, 0, 1);
            glBindVertexArray(gl.bone_vao);
            glBindBuffer(GL_ARRAY_BUFFER, gl.bone_vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(inter), inter, GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(6 * sizeof(float)));
            for (int m = 0; m < 3; m++) {
                float x = 74.0f + (float)m * 110.0f;
                Vec3 col = (st.mode == m) ? v3(0.95f, 0.78f, 0.25f) : v3(0.22f, 0.25f, 0.30f);
                Mat4 model = m4_mul(m4_translate(v3(x, 32.0f, 0)), m4_scale(v3(100.0f, 28.0f, 1)));
                Mat4 mvp = m4_mul(hud, model);
                glUniformMatrix4fv(gl.u_mvp, 1, GL_FALSE, mvp.m);
                glUniformMatrix4fv(gl.u_model, 1, GL_FALSE, model.m);
                glUniform3f(gl.u_color, col.x, col.y, col.z);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            {
                int cx = genome_complexity(&st.genome) + st.spine.count;
                float fill = clampf((float)cx / (float)COMPLEXITY_MAX, 0, 1);
                Mat4 model = m4_mul(m4_translate(v3(24.0f + 110.0f * fill, 64.0f, 0)),
                                    m4_scale(v3(220.0f * fill, 12.0f, 1)));
                Mat4 mvp = m4_mul(hud, model);
                glUniformMatrix4fv(gl.u_mvp, 1, GL_FALSE, mvp.m);
                glUniformMatrix4fv(gl.u_model, 1, GL_FALSE, model.m);
                glUniform3f(gl.u_color, 0.45f, 0.70f, 0.95f);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            glBindVertexArray(0);
            glEnable(GL_DEPTH_TEST);
        }

        {
            const char *mode = st.mode == MODE_BUILD ? "BUILD"
                             : st.mode == MODE_PAINT ? "PAINT" : "TEST";
            int cx = genome_complexity(&st.genome) + st.spine.count;
            int part_i = st.part_slot == 0 ? st.genome.mouth
                       : st.part_slot == 1 ? st.genome.legs
                       : st.part_slot == 2 ? st.genome.weapon
                       : st.part_slot == 3 ? st.genome.ability
                       : st.part_slot == 4 ? st.genome.eyes
                       : st.part_slot == 5 ? st.genome.grasper : st.genome.detail;
            char title[200];
            snprintf(title, sizeof(title),
                     "[%s] %s | bone %d/%d | $%d | cx %d/%d | %s",
                     mode, st.name, st.sel_bone + 1, st.spine.count,
                     st.cash, cx, COMPLEXITY_MAX,
                     unlocks_part_name(st.part_slot, part_i));
            SDL_SetWindowTitle(win, title);
        }

        if (shot_path && !shot_done) {
            write_ppm(shot_path, ww, wh);
            printf("\nwrote %s (%dx%d)\n", shot_path, ww, wh);
            shot_done = 1;
            running = 0;
        }

        SDL_GL_SwapWindow(win);
        print_hud(&st);
    }

    printf("\n");
    gl_shutdown(&gl);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
