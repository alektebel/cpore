/* Land body plans - internal to src/, not part of the public API.
 *
 * Turning a stage-3 genome into a list of round cones is a pure function of
 * the animal and nothing else: no camera, no light, no palette. It sat inside
 * the pixel renderer only because that was the only renderer, and a second
 * one wanting the same four hundred lines is the usual signal that the code
 * was never really about drawing.
 *
 * What lives here is the skeleton, the three coats of paint over it, and the
 * per-part albedos. What does not is any decision about how the result is
 * lit - the two renderers disagree about that completely, which is the whole
 * point of there being two.
 *
 * Everything is static, so both may include it without colliding at link
 * time. Geometry comes from sdfbody.h, which the aquatic stage shares in
 * turn: a body plan that reads well in one stage reads well in the other.
 */
#ifndef CPORE_LANDBODY_H
#define CPORE_LANDBODY_H

#include "cpore/land.h"
#include "sdfbody.h"

#ifndef LB_PI
#define LB_PI CP_PI
#endif

static inline V3 cv(Cp4Vec v) { return v3(v.x, v.y, v.z); }

/* ---------------- creature geometry ---------------- */

typedef struct {
    V3    base, mark, detail;
    int   pattern, pattern2;
    float freq, freq2;
    V3    origin, fwd, right, up;
} Skin;

static V3 part_albedo4(int t, float *emissive)
{
    *emissive = 0.0f;
    switch (t) {
    case CP4_MOUTH_G: return v3(0.72f, 0.60f, 0.44f);
    case CP4_MOUTH_C: return v3(0.94f, 0.90f, 0.80f);
    case CP4_MOUTH_O: return v3(0.88f, 0.72f, 0.34f);
    case CP4_LEG:     return v3(0.54f, 0.46f, 0.36f);
    case CP4_FOOT:    return v3(0.38f, 0.32f, 0.26f);
    case CP4_CLAW:    return v3(0.20f, 0.19f, 0.22f);
    case CP4_HORN:    return v3(0.86f, 0.82f, 0.68f);
    case CP4_PLATE:   return v3(0.46f, 0.44f, 0.42f);
    case CP4_EYE:     return v3(0.97f, 0.98f, 1.00f);
    case CP4_EAR:     return v3(0.66f, 0.50f, 0.46f);
    case CP4_VOICE:   return v3(0.92f, 0.42f, 0.40f);
    case CP4_PLUME:   return v3(0.86f, 0.30f, 0.62f);
    case CP4_WING:    return v3(0.60f, 0.62f, 0.72f);
    case CP4_FIN:     return v3(0.42f, 0.70f, 0.80f);
    case CP4_GILL:    return v3(0.84f, 0.34f, 0.36f);
    case CP4_DIGGER:  return v3(0.74f, 0.68f, 0.52f);
    case CP4_ARM:     return v3(0.62f, 0.52f, 0.44f);
    case CP4_TAIL:    return v3(0.56f, 0.48f, 0.42f);
    default:          return v3(0.6f, 0.6f, 0.6f);
    }
}

/* The spine, on its own.
 *
 * Split out of build_prims4 because an editor has to answer questions the
 * renderer never asks - which vertebra is nearest this point, what yaw and
 * pitch would put a part here - and the only safe way to answer them is from
 * the same arithmetic that drew the picture. A second copy of this that
 * drifted by a degree would put every dropped part slightly off the surface
 * it was dropped on, which is the kind of wrongness a user feels immediately
 * and cannot describe. */
typedef struct {
    V3    pos[CP4_MAX_SEG];      /* vertebra centres, world space */
    float rad[CP4_MAX_SEG];      /* and how thick the body is there */
    int   n;                     /* how many are in use */
    V3    fwd, right, up;        /* the body's own frame */
    float R, L;                  /* nominal radius and length */
} LandSpine;

static void land_spine(const Cp4Beast *b, LandSpine *s)
{
    basis3(b->yaw, b->pitch, &s->fwd, &s->right, &s->up);
    int nseg = b->g.nseg < 2 ? 2 : b->g.nseg;
    if (nseg > CP4_MAX_SEG) nseg = CP4_MAX_SEG;
    s->n = nseg;
    float R = b->s.radius, L = b->s.length;
    s->R = R; s->L = L;

    /* The spine is read, not computed.
     *
     * It used to be a formula - one arch over the whole body, a per-vertebra
     * rise, a sweep to one side - and a vertebra's position was therefore
     * derived from three genes rather than stored anywhere. That is fine for a
     * generator and impossible for an editor: "drag this one wherever you
     * like" has nowhere to write the answer down. So the genome carries the
     * points and this walks them.
     *
     * The scales are the contract the header states: `along` spans half the
     * body length per 127, `side` and `up` 1.6 body radii. Everything the old
     * formula could say, a set of points can still say; everything a set of
     * points can say, the formula could not. */
    for (int i = 0; i < nseg; i++) {
        float t = (float)i / (float)(nseg - 1);
        const Cp4Vert *cp = &b->g.spine[i];
        float along = (float)cp->along / 127.0f * 0.5f * L;
        float side  = (float)cp->side  / 127.0f * R * 1.6f;
        float rise  = (float)cp->up    / 127.0f * R * 1.6f;
        /* a walking animal sways, it does not undulate - a tenth of the
         * amplitude the fish use */
        float sway = sinf(b->phase * 0.5f - t * 1.4f) * R * 0.10f;
        s->pos[i] = add(add(add(cv(b->p), mul(s->fwd, along)),
                            mul(s->right, sway + side)),
                        mul(s->up, rise));
        s->rad[i] = R * cp4_profile(&b->g, t);
        if (s->rad[i] < R * 0.15f) s->rad[i] = R * 0.15f;
    }
}

/* Build a land animal in world space.
 *
 * The one real departure from the fish is legs: they are not decorative studs
 * on the hull but a two-bone chain that reaches the ground and swings on the
 * gait phase, because on land the contact between animal and terrain is the
 * thing the eye checks first. */
static int build_prims4(const Cp4Beast *b, int is_player, Prim *out,
                        V3 *centre, float *bound, Skin *skin)
{
    V3 fwd, right, up;
    basis3(b->yaw, b->pitch, &fwd, &right, &up);

    int nseg = b->g.nseg < 2 ? 2 : b->g.nseg;
    float R = b->s.radius;
    float L = b->s.length;

    float base[3], mark[3], detail[3];
    cp4_genome_colour(&b->g, base, mark, detail);
    V3 body = v3(base[0], base[1], base[2]);
    if (is_player) {
        /* Brighten the agent's own animal without touching its hue.
         *
         * The obvious tint - lerp toward a fixed colour - produces a
         * low-chroma mid tone, and a low-chroma mid tone is the one thing a
         * 32-entry palette has no good answer for: the quantiser reached for
         * the nearest dark violet and the player rendered as a black
         * silhouette in its own frame. Scaling value up keeps the chroma the
         * palette needs to place it correctly. */
        float mx = body.x > body.y ? body.x : body.y;
        if (body.z > mx) mx = body.z;
        if (mx > 1e-3f && mx < 0.86f) body = mul(body, 0.86f / mx);
    }
    skin->base = body;
    skin->mark = v3(mark[0], mark[1], mark[2]);
    skin->detail = v3(detail[0], detail[1], detail[2]);
    skin->pattern = b->g.pattern;
    skin->pattern2 = b->g.pattern2;
    skin->freq = 0.02f + 0.16f * ((float)b->g.pscale / 255.0f);
    /* The detail coat runs finer than the marking coat. Two patterns at the
     * same frequency beat against each other into moire; an octave and a bit
     * apart they read as a marking with detail on it. */
    skin->freq2 = 0.05f + 0.30f * ((float)b->g.pscale2 / 255.0f);
    skin->origin = cv(b->p);
    skin->fwd = fwd; skin->right = right; skin->up = up;

    LandSpine sp;
    land_spine(b, &sp);
    V3 *segpos = sp.pos;
    float *segrad = sp.rad;
    /* take the segment count from the spine too, so the two can never
     * disagree about how many vertebrae there are */
    nseg = sp.n;

    int n = 0;
    float bk = R * 0.34f;
    for (int i = 0; i + 1 < nseg; i++)
        push(out, &n, segpos[i], segpos[i + 1], segrad[i], segrad[i + 1],
             bk, body, 0.0f, 1.0f);
    {
        V3 tail = add(segpos[nseg - 1], mul(fwd, -L * 0.20f));
        push(out, &n, segpos[nseg - 1], tail, segrad[nseg - 1],
             R * 0.14f, bk, body, 0.0f, 1.0f);
    }

    int legi = 0;
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        int t = b->g.part[i].type;
        if (t == CP4_NONE) continue;
        int sg = b->g.part[i].seg;
        if (sg >= nseg) sg = nseg - 1;
        /* Everything pushed from here to the end of this iteration belongs to
         * genome slot i, whatever shape it turned out to be - a spike is one
         * cone and a tail is five. Stamping the range afterwards is what lets
         * an editor turn a pixel back into the gene that drew it. */
        int prim0 = n;

        float er;
        V3 col = part_albedo4(t, &er);
        /* Limbs wear the animal's own skin.
         *
         * Given their own grey they read as prosthetics bolted to a coloured
         * torso - and with sixteen slots and jointed arms and tails, most of
         * the silhouette is limb, so most of the animal came out the same pale
         * grey whatever its genome said. Hard parts - claw, horn, plate, eye,
         * beak - keep their own material, because those are the ones that are
         * supposed to look like a different substance. */
        if (t == CP4_LEG || t == CP4_ARM || t == CP4_TAIL || t == CP4_FIN
            || t == CP4_WING || t == CP4_FOOT)
            col = mul(body, t == CP4_TAIL ? 0.94f : 0.86f);
        float sc = 0.45f + 1.45f * ((float)b->g.part[i].scale / 255.0f);
        int copies = b->g.part[i].mirror ? 2 : 1;

        for (int m = 0; m < copies; m++) {
            int yaw_u = m ? ((256 - b->g.part[i].yaw) & 0xFF) : b->g.part[i].yaw;
            float py = (float)yaw_u * (2.0f * LB_PI / 256.0f);
            float pp = (float)b->g.part[i].pitch * (LB_PI / 128.0f);
            float cy = cosf(py), sy = sinf(py), cpp = cosf(pp), spp = sinf(pp);
            V3 ax = norm(add(add(mul(fwd, cy * cpp), mul(right, sy * cpp)), mul(up, spp)));
            V3 bs = add(segpos[sg], mul(ax, segrad[sg] * 0.60f));

            switch (t) {
            case CP4_LEG: {
                /* A leg is the one part that is not free to point wherever the
                 * gene says. It sprouts from the lower flank and it ends on the
                 * ground, because on land the contact between animal and
                 * terrain is the first thing the eye checks - a leg hanging in
                 * space reads as a broken model, not as an odd body plan.
                 *
                 * The foot plane is exact rather than estimated: p.y is the
                 * ground minus stand, so ground is p.y + stand. */
                float side = m ? -1.0f : 1.0f;
                V3 down = v3(0.0f, 1.0f, 0.0f);          /* y is down */
                V3 hip = add(add(segpos[sg], mul(right, side * segrad[sg] * 0.78f)),
                             mul(down, segrad[sg] * 0.34f));
                float ground_y = b->p.y + b->s.stand;
                float len = ground_y - hip.y;
                if (len < R * 0.35f) len = R * 0.35f;

                /* alternating gait: opposite legs a half cycle apart */
                float ph = b->phase + (float)legi * 1.7f;
                float stride = cosf(ph) * len * 0.34f;
                float lift = clampf(sinf(ph), 0.0f, 1.0f) * len * 0.26f;
                V3 foot = v3(hip.x + fwd.x * stride, ground_y - lift,
                             hip.z + fwd.z * stride);
                /* Where the knee sits, from the gene rather than from a
                 * constant. The foot has to reach the ground whatever the
                 * gene says - that is not negotiable - so what `bend` buys is
                 * how far the joint is thrown forward or back off the straight
                 * line between hip and foot, which is the difference between a
                 * digitigrade sprinter and a squat digger standing on the same
                 * two points. `len` decides how much of the limb is thigh. */
                float bend_g = (float)b->g.part[i].bend / 127.0f;
                float lenf   = (float)b->g.part[i].len / 255.0f;
                float split  = 0.32f + 0.40f * lenf;
                V3 knee = add(add(add(mul(hip, 1.0f - split), mul(foot, split)),
                                  mul(fwd, len * (0.06f + 0.42f * bend_g))),
                              mul(right, side * len * (0.06f + 0.16f * lenf)));

                float tk = R * (0.34f - 0.13f * lenf) * sc;
                push(out, &n, hip, knee, tk, tk * 0.76f, R * 0.16f, col, 0.0f, 0.0f);
                push(out, &n, knee, foot, tk * 0.76f, tk * 0.55f, R * 0.12f, col, 0.0f, 0.0f);
                push(out, &n, foot, add(foot, mul(fwd, R * 0.30f * sc)),
                     tk * 0.62f, tk * 0.46f, R * 0.08f,
                     part_albedo4(CP4_FOOT, &er), 0.0f, 0.0f);
                legi++;
                break;
            }
            case CP4_FOOT: {
                /* a broad pad, spread flat on the ground under the body */
                float side = m ? -1.0f : 1.0f;
                V3 at = v3(segpos[sg].x + right.x * side * R * 0.62f,
                           b->p.y + b->s.stand - R * 0.10f,
                           segpos[sg].z + right.z * side * R * 0.62f);
                push(out, &n, at, add(at, mul(fwd, R * 0.46f * sc)),
                     R * 0.30f * sc, R * 0.22f * sc, R * 0.10f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_HORN:
                push(out, &n, bs, add(bs, mul(ax, R * 1.45f * sc)),
                     R * 0.28f * sc, R * 0.03f, R * 0.14f, col, 0.0f, 0.0f);
                break;
            case CP4_CLAW:
                push(out, &n, bs, add(bs, mul(ax, R * 0.60f * sc)),
                     R * 0.16f * sc, R * 0.02f, R * 0.07f, col, 0.0f, 0.0f);
                break;
            case CP4_PLATE: {
                /* a shield lying against the flank, not a lump stuck on it */
                V3 e = add(bs, mul(ax, R * 0.10f));
                push(out, &n, e, add(e, mul(fwd, R * 0.55f * sc)),
                     R * 0.62f * sc, R * 0.50f * sc, R * 0.30f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_MOUTH_C:
            case CP4_MOUTH_O:
            case CP4_MOUTH_G: {
                float jl = (t == CP4_MOUTH_C ? 0.95f : t == CP4_MOUTH_O ? 0.80f : 0.62f);
                push(out, &n, bs, add(bs, mul(ax, R * jl * sc)),
                     R * 0.46f * sc, R * 0.22f * sc, R * 0.22f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_EYE: {
                V3 e = add(bs, mul(ax, R * 0.20f));
                float er2 = R * 0.26f * (0.7f + 0.5f * sc);
                push(out, &n, e, e, er2, er2, R * 0.04f, col, 0.0f, 0.0f);
                V3 pu = add(e, mul(ax, er2 * 0.70f));
                push(out, &n, pu, pu, er2 * 0.46f, er2 * 0.46f, R * 0.03f,
                     v3(0.05f, 0.05f, 0.08f), 0.0f, 0.0f);
                break;
            }
            case CP4_EAR: {
                V3 tip = add(add(bs, mul(ax, R * 0.85f * sc)), mul(up, R * 0.35f * sc));
                push(out, &n, bs, tip, R * 0.26f * sc, R * 0.07f, R * 0.10f,
                     col, 0.0f, 0.0f);
                break;
            }
            case CP4_VOICE: {
                /* the throat sac inflates on the call - the one part of the
                 * body that animates because of what the animal is doing */
                float puff = 1.0f + 0.35f * clampf(sinf(b->sing_t * 9.0f), 0.0f, 1.0f)
                                  * (b->sing_t > 0.0f ? 1.0f : 0.0f);
                V3 e = add(bs, mul(ax, R * 0.30f));
                float rr = R * 0.42f * sc * puff;
                push(out, &n, e, e, rr, rr, R * 0.20f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_PLUME: {
                /* a fan, because one spike does not read as display */
                for (int f = -1; f <= 1; f++) {
                    V3 dir = norm(add(ax, mul(right, (float)f * 0.45f)));
                    V3 tip = add(bs, mul(dir, R * 1.5f * sc));
                    push(out, &n, bs, tip, R * 0.10f * sc, R * 0.30f * sc,
                         R * 0.08f, f ? skin->mark : col, 0.0f, 0.0f);
                }
                break;
            }
            case CP4_WING: {
                float beat = sinf(b->phase * 0.8f + (float)m * 3.1f) * 0.25f;
                V3 dir = norm(add(ax, mul(up, beat)));
                push(out, &n, bs, add(bs, mul(dir, R * 2.0f * sc)),
                     R * 0.46f * sc, R * 0.10f * sc, R * 0.22f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_FIN: {
                /* a broad blade that sweeps, so a swimmer reads as propelling
                 * itself rather than as a legged animal that fell in */
                float sweep_f = sinf(b->phase * 1.4f + (float)i + (float)m * 3.1f) * 0.40f;
                V3 dir = norm(add(ax, mul(up, sweep_f)));
                push(out, &n, bs, add(bs, mul(dir, R * 1.5f * sc)),
                     R * 0.42f * sc, R * 0.09f * sc, R * 0.24f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_GILL: {
                /* three slits along the flank, the one part that says "this
                 * body belongs in the water" while it is standing on a beach */
                for (int q = 0; q < 3; q++) {
                    V3 at = add(bs, mul(fwd, -(float)q * R * 0.26f));
                    push(out, &n, at, add(at, mul(up, -R * 0.34f * sc)),
                         R * 0.10f * sc, R * 0.05f * sc, R * 0.05f, col, 0.0f, 0.0f);
                }
                break;
            }
            case CP4_DIGGER: {
                /* a heavy spade, blunter and wider than a claw */
                V3 dir = norm(add(ax, mul(fwd, 0.55f)));
                push(out, &n, bs, add(bs, mul(dir, R * 0.85f * sc)),
                     R * 0.30f * sc, R * 0.34f * sc, R * 0.12f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_ARM: {
                /* An arm is a leg that does not have to reach the ground, and
                 * that one difference is what makes it worth having as a
                 * separate part: freed from the floor it can point anywhere,
                 * so where the gene aims it actually shows. Two bones and a
                 * hand, folded by `bend`, reaching by `len`. */
                float side = m ? -1.0f : 1.0f;
                float lenf = (float)b->g.part[i].len / 255.0f;
                float bend_g = (float)b->g.part[i].bend / 127.0f;
                float reach = R * (0.85f + 1.85f * lenf) * sc;
                /* Hung, not splayed. Aimed purely where the gene points, arms
                 * came out as horizontal outriggers - correct to the genome
                 * and wrong as an animal. Biasing the root forward and down
                 * puts them where a limb that has to reach things actually
                 * hangs, and leaves the gene deciding the rest. */
                V3 down = v3(0.0f, 1.0f, 0.0f);
                V3 adir = norm(add(add(mul(ax, 1.0f), mul(fwd, 0.34f)),
                                   mul(down, 0.46f)));
                V3 shoulder = bs;
                V3 elbow = add(shoulder, mul(adir, reach * 0.52f));
                /* the fold happens across the limb, so it reads as a joint
                 * rather than as a kink in a straight tube */
                V3 across = norm(add(mul(right, side * 0.75f), mul(up, -0.66f)));
                V3 hand = add(add(elbow, mul(adir, reach * 0.48f * (1.0f - 0.55f * fabsf(bend_g)))),
                              mul(across, reach * 0.62f * bend_g));
                float tk = R * (0.24f - 0.07f * lenf) * sc;
                push(out, &n, shoulder, elbow, tk, tk * 0.82f, R * 0.13f, col, 0.0f, 0.0f);
                push(out, &n, elbow, hand, tk * 0.82f, tk * 0.60f, R * 0.11f, col, 0.0f, 0.0f);
                /* the hand: three short digits, which is the cheapest thing
                 * that reads as "this end grasps" */
                for (int f = 0; f < 3; f++) {
                    float a2 = (float)f * 2.094f + 0.4f;
                    V3 off = add(mul(right, cosf(a2) * 0.55f), mul(up, sinf(a2) * 0.55f));
                    V3 tip = add(add(hand, mul(adir, reach * 0.16f)),
                                 mul(off, reach * 0.12f));
                    push(out, &n, hand, tip, tk * 0.46f, tk * 0.16f, R * 0.06f,
                         col, 0.0f, 0.0f);
                }
                break;
            }
            case CP4_TAIL: {
                /* A tail is a chain, not a spike: it has to taper over several
                 * links and it has to swing, or it reads as a stick glued to
                 * the back. The swing is driven off the gait phase, so it
                 * counterweights the stride the way a real one does. */
                float lenf = (float)b->g.part[i].len / 255.0f;
                float bend_g = (float)b->g.part[i].bend / 127.0f;
                float total = R * (1.2f + 3.4f * lenf) * sc;
                const int LINKS = 5;
                V3 at = add(segpos[nseg - 1], mul(fwd, -segrad[nseg - 1] * 0.55f));
                V3 dir = norm(add(mul(fwd, -1.0f), mul(up, bend_g * 0.55f)));
                float rr = R * (0.40f + 0.22f * lenf) * sc;
                for (int k = 0; k < LINKS; k++) {
                    float f = (float)k / (float)LINKS;
                    float seglen = total / (float)LINKS;
                    /* the whip: later links lag further behind the swing */
                    float swing = sinf(b->phase * 0.5f - f * 2.1f) * 0.20f;
                    V3 d2 = norm(add(add(dir, mul(right, swing)),
                                     mul(up, bend_g * 0.30f * f)));
                    V3 nx2 = add(at, mul(d2, seglen));
                    float r2a = rr * (1.0f - 0.72f * f);
                    float r2b = rr * (1.0f - 0.72f * (f + 1.0f / (float)LINKS));
                    push(out, &n, at, nx2, r2a, r2b, R * 0.12f, col, 0.0f, 0.55f);
                    at = nx2;
                    dir = d2;
                }
                break;
            }
            default:
                push(out, &n, bs, add(bs, mul(ax, R * 0.30f * sc)),
                     R * 0.32f * sc, R * 0.28f * sc, R * 0.18f, col, er, 0.0f);
                break;
            }
        }
        /* close the range opened above: every primitive this iteration pushed,
         * across both mirror copies, belongs to genome slot i */
        for (int z = prim0; z < n; z++) out[z].part = i;
    }

    prim_bounds(out, n, cv(b->p), centre, bound);
    return n;
}

/* Markings, applied only where the surface is trunk rather than appendage.
 * Colour carries as much perceived variety as shape, and quantising to 32
 * colours punishes gradients, so the patterns are deliberately crisp. */
static V3 pattern_layer(const Skin *sk, V3 q, V3 albedo, float bodyw,
                        int pattern, float freq, V3 ink, float strength)
{
    if (bodyw <= 0.02f || pattern == CP4_PAT_PLAIN) return albedo;
    V3 d = sub(q, sk->origin);
    float along = dot(d, sk->fwd), side = dot(d, sk->right), vert = dot(d, sk->up);
    float m = 0.0f;
    switch (pattern) {
    case CP4_PAT_BANDS:
        m = sinf(along * freq * 6.0f) > 0.15f ? 1.0f : 0.0f;
        break;
    case CP4_PAT_SPOTS:
        m = (sinf(along * freq * 5.0f) * sinf(side * freq * 5.0f)
           * sinf(vert * freq * 5.0f)) > 0.30f ? 1.0f : 0.0f;
        break;
    case CP4_PAT_COUNTER:
        m = clampf(0.5f + vert * 0.16f, 0.0f, 1.0f);
        break;
    case CP4_PAT_STRIPES:
        m = sinf(atan2f(vert, side) * 4.0f + along * freq * 0.9f) > 0.1f ? 1.0f : 0.0f;
        break;
    case CP4_PAT_MOTTLE: {
        float a = sinf(along * freq * 3.1f) + sinf(side * freq * 4.7f)
                + sinf(vert * freq * 2.3f) + sinf((along + side) * freq * 6.1f);
        m = a > 0.55f ? 1.0f : 0.0f;
        break;
    }
    case CP4_PAT_GRADIENT:
        m = clampf(0.5f - along * freq * 0.55f, 0.0f, 1.0f);
        m = m > 0.5f ? (m - 0.5f) * 2.0f : 0.0f;
        break;
    case CP4_PAT_RINGS:
        m = sinf(sqrtf(side * side + vert * vert) * freq * 7.0f) > 0.2f ? 1.0f : 0.0f;
        break;
    default: break;
    }
    m *= bodyw * strength;
    return v3(mixf(albedo.x, ink.x, m), mixf(albedo.y, ink.y, m),
              mixf(albedo.z, ink.z, m));
}

/* Three coats, applied in order: base, marking, detail. Spore's paint mode is
 * exactly this and it is most of why two creatures with the same skeleton do
 * not look like the same creature - shape is expensive and colour is free. */
static V3 apply_pattern(const Skin *sk, V3 q, V3 albedo, float bodyw)
{
    albedo = pattern_layer(sk, q, albedo, bodyw, sk->pattern, sk->freq,
                           sk->mark, 1.0f);
    albedo = pattern_layer(sk, q, albedo, bodyw, sk->pattern2, sk->freq2,
                           sk->detail, 0.72f);
    return albedo;
}


#endif /* CPORE_LANDBODY_H */
