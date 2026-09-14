#include "spore/creature.h"
#include "spore/genome.h"

#include <math.h>
#include <string.h>

static const float k_phase[CREATURE_LEGS] = {
    0.00f, 0.50f, 0.33f, 0.83f, 0.66f, 0.16f
};

static float smooth_ease(float t) {
    return t * t * (3.0f - 2.0f * t);
}

static Vec3 body_to_world(const Creature *c, Vec3 local) {
    float cy = cosf(c->yaw), sy = sinf(c->yaw);
    Vec3 scaled = v3_mul(local, c->scale);
    Vec3 r = v3(scaled.x * cy + scaled.z * sy,
                scaled.y,
               -scaled.x * sy + scaled.z * cy);
    return v3_add(c->body, r);
}

static Vec3 world_forward(const Creature *c) {
    return v3(sinf(c->yaw), 0.0f, cosf(c->yaw));
}

static Vec3 world_right(const Creature *c) {
    return v3(cosf(c->yaw), 0.0f, -sinf(c->yaw));
}

static void resolve_leg_ik(Leg *L, Vec3 pole) {
    L->knee = ik_2bone(L->hip, L->foot, L->upper_len, L->lower_len, pole);
}

void creature_init(Creature *c, const Terrain *t, Vec3 pos,
                   Genome genome, Vec3 color, float scale, int player) {
    memset(c, 0, sizeof(*c));
    c->genome = genome;
    c->color = color;
    /* Belly paint defaults to a lighter complementary tint */
    c->color2 = v3(clampf(color.x * 0.55f + 0.40f, 0.15f, 1.0f),
                   clampf(color.y * 0.55f + 0.35f, 0.15f, 1.0f),
                   clampf(color.z * 0.45f + 0.30f, 0.15f, 1.0f));
    c->pattern = PATTERN_NONE;
    c->scale = scale < 0.4f ? 0.4f : scale;
    c->health = 100.0f;
    c->hunger = 70.0f;
    c->dna = player ? 40.0f : 0.0f;
    c->alive = 1;
    c->player = player;
    c->generation = 1;
    c->stride_scale = 1.0f;
    c->grounded = 1;
    c->stamina = 100.0f;
    c->vuln = 1.0f;
    genome_apply(c);

    c->pos = pos;
    c->pos.y = terrain_height(t, pos.x, pos.z);
    c->home = c->pos;
    c->elev = 0.0f;
    float stance0 = 1.10f * STANCES[c->genome.stance].upright
                  * REACHES[c->genome.reach].posture;
    if (genome_limb_count(&c->genome) == 2)
        stance0 *= 1.20f; /* true bipeds stand taller */
    c->body = v3(c->pos.x, c->pos.y + stance0 * c->scale, c->pos.z);

    float bone = 0.55f * c->scale * REACHES[c->genome.reach].limb;
    for (int i = 0; i < CREATURE_LEGS; i++) {
        /* hip_local already set by genome_apply (spine / asymmetry) */
        Leg *L = &c->legs[i];
        L->upper_len = bone;
        L->lower_len = bone;
        L->phase = k_phase[i];
        L->hip = body_to_world(c, c->hip_local[i]);
        L->foot = v3(L->hip.x, terrain_height(t, L->hip.x, L->hip.z), L->hip.z);
        L->plant = L->foot;
        resolve_leg_ik(L, v3_add(L->hip, v3(0, -1, 0)));
    }
    Vec3 fwd = world_forward(c);
    float spine_l = SPINES[c->genome.spine].length;
    float neck_r = NECKS[c->genome.neck].reach;
    c->head = v3_add(c->body, v3_add(v3_mul(fwd, 0.50f * spine_l * c->scale),
                                     v3(0, 0.28f * neck_r * c->scale, 0)));
    c->neck = v3_lerp(c->body, c->head, 0.42f);
    c->tail = v3_add(c->body, v3_mul(fwd, -0.70f * spine_l * c->scale));
    if (c->spine.active) spine_sync_anchors(c);
}

void creature_apply_damage(Creature *c, float amount) {
    if (!c->alive) return;
    if (c->player && c->spawn_protect > 0.0f) return;
    if (c->burrow_t > 0.0f) amount *= 0.15f; /* dug-in shell */
    float vul = (c->vuln > 0.05f) ? c->vuln : 1.0f;
    float dmg = amount * vul * (1.0f - clampf(c->armor, 0.0f, 0.6f));
    c->health -= dmg;
    c->hit_flash = 0.35f;
    if (c->health <= 0.0f) {
        c->health = 0.0f;
        c->alive = 0;
    }
}

int creature_try_attack(Creature *attacker, Creature *target) {
    if (!attacker->alive || !target->alive) return 0;
    if (attacker->attack_cd > 0.0f || attacker->stun_t > 0.0f) return 0;
    float reach = 1.6f * (attacker->scale + target->scale) * 0.5f;
    float dx = attacker->pos.x - target->pos.x;
    float dz = attacker->pos.z - target->pos.z;
    if (dx * dx + dz * dz > reach * reach) return 0;
    creature_apply_damage(target, attacker->attack_power);
    if (attacker->genome.weapon == WEAPON_POISON)
        target->poison_t = fmaxf(target->poison_t, 3.5f);
    /* Quills: Spore thorns — reflect a bite of melee back at the attacker */
    if (target->genome.detail == DETAIL_QUILLS && attacker->alive) {
        float reflect = attacker->attack_power * 0.28f;
        creature_apply_damage(attacker, reflect);
    }
    /* Knockback away from attacker */
    float len = sqrtf(dx * dx + dz * dz);
    if (len > 1e-4f) {
        float force = 4.5f + attacker->attack_power * 0.08f;
        if (attacker->genome.weapon == WEAPON_HORN) {
            force *= 1.55f; /* horns punch harder */
            if (attacker->charge_t > 0.0f) force *= 1.35f;
        } else if (attacker->genome.weapon == WEAPON_CLUB) {
            force *= 2.05f; /* blunt weapons launch foes */
            target->stun_t = fmaxf(target->stun_t, 0.65f);
        }
        target->knock.x += (-dx / len) * force;
        target->knock.z += (-dz / len) * force;
    }
    attacker->attack_cd = 0.55f;
    return 1;
}

void creature_update_locomotion(Creature *c, const Terrain *t, float dt,
                                float move_x, float move_z, int want_jump) {
    if (!c->alive) {
        c->pos.y = terrain_height(t, c->pos.x, c->pos.z);
        c->elev = 0.0f;
        c->body = v3(c->pos.x, c->pos.y + 0.25f * c->scale, c->pos.z);
        return;
    }

    if (c->attack_cd > 0.0f) c->attack_cd -= dt;
    if (c->ability_cd > 0.0f) c->ability_cd -= dt;
    if (c->spawn_protect > 0.0f) c->spawn_protect -= dt;
    if (c->jump_cd > 0.0f) c->jump_cd -= dt;
    if (c->hit_flash > 0.0f) c->hit_flash = fmaxf(0.0f, c->hit_flash - dt);
    if (c->threat_t > 0.0f) c->threat_t = fmaxf(0.0f, c->threat_t - dt);
    /* Apply knockback impulse */
    if (fabsf(c->knock.x) > 1e-4f || fabsf(c->knock.z) > 1e-4f) {
        c->pos.x += c->knock.x * dt;
        c->pos.z += c->knock.z * dt;
        c->knock.x *= fmaxf(0.0f, 1.0f - 6.0f * dt);
        c->knock.z *= fmaxf(0.0f, 1.0f - 6.0f * dt);
        float margin = 2.0f;
        c->pos.x = clampf(c->pos.x, margin, t->world_size - margin);
        c->pos.z = clampf(c->pos.z, margin, t->world_size - margin);
    }
    if (c->poison_t > 0.0f) {
        c->poison_t -= dt;
        creature_apply_damage(c, dt * 9.0f);
        if (!c->alive) return;
    }
    if (c->burrow_t > 0.0f) {
        c->burrow_t -= dt;
        move_x = move_z = 0.0f;
        want_jump = 0;
        c->speed = 0.0f;
        c->pos.y = terrain_height(t, c->pos.x, c->pos.z) - 0.35f * c->scale;
        c->elev = -0.35f * c->scale;
        c->body = v3(c->pos.x, terrain_height(t, c->pos.x, c->pos.z) + 0.15f * c->scale, c->pos.z);
        c->airborne = 0;
        c->grounded = 1;
        /* Still tick cooldowns above; skip normal locomotion while dug in */
        return;
    }
    /* Camouflage cloak — moves slowly, breaks if sprinting */
    if (c->camo_t > 0.0f) {
        c->camo_t -= dt;
        if (c->sprinting) c->camo_t = 0.0f;
        want_jump = 0;
        if (c->threat_t > 0.0f) c->threat_t = 0.0f;
    }
    if (c->stun_t > 0.0f) {
        c->stun_t -= dt;
        move_x = move_z = 0.0f;
        want_jump = 0;
    }

    /* Stealth crouch: slow, low, no leaping */
    if (c->crouching) {
        want_jump = 0;
        if (c->threat_t > 0.0f) c->threat_t = 0.0f; /* can't intimidate while sneaking */
    }

    Vec3 wish = v3(move_x, 0.0f, move_z);
    float wish_len = v3_len(wish);
    const float accel = 12.0f;
    float max_speed = c->max_speed;
    /* Gradual starvation limp — full belly gives a slight pep */
    {
        float h = clampf(c->hunger / 100.0f, 0.0f, 1.0f);
        max_speed *= 0.55f + 0.50f * h;
    }
    if (c->crouching) max_speed *= 0.48f;
    if (c->camo_t > 0.0f) max_speed *= 0.55f; /* cloaked creep */

    /* Sprint: burn stamina for a burst of speed */
    if (c->sprinting && !c->crouching && c->stamina > 0.5f && wish_len > 1e-4f
        && c->charge_t <= 0.0f && c->burrow_t <= 0.0f) {
        float mult = 1.48f;
        float drain = 26.0f;
        if (c->genome.legs == LEGS_SPRINT) { mult = 1.72f; drain = 18.0f; }
        else if (c->genome.legs == LEGS_STUB) { mult = 1.22f; drain = 34.0f; }
        else if (c->genome.legs == LEGS_JUMP) { mult = 1.55f; drain = 24.0f; }
        max_speed *= mult;
        c->stamina = fmaxf(0.0f, c->stamina - drain * dt);
        if (c->stamina <= 0.5f) c->sprinting = 0;
    } else {
        c->sprinting = 0;
        float regen = (wish_len < 1e-4f) ? 22.0f : 11.0f;
        if (c->crouching) regen *= 1.35f; /* catch breath while crouched */
        c->stamina = fminf(100.0f, c->stamina + regen * dt);
    }

    /* Climb legs handle slopes; stubs struggle uphill */
    {
        Vec3 nrm = terrain_normal(t, c->pos.x, c->pos.z);
        float slope = 1.0f - clampf(nrm.y, 0.0f, 1.0f); /* 0 flat, higher = steeper */
        if (c->genome.legs == LEGS_CLIMB)
            max_speed *= 1.0f + slope * 1.35f;
        else if (c->genome.legs == LEGS_STUB)
            max_speed *= 1.0f - slope * 0.85f;
        else if (c->genome.legs == LEGS_SPRINT)
            max_speed *= 1.0f - slope * 0.45f;
    }

    int will_swim = terrain_is_water(t, c->pos.x, c->pos.z);
    if (will_swim && c->charge_t <= 0.0f && c->elev < 0.3f) {
        if (c->genome.legs == LEGS_FLIPPER) max_speed *= 1.55f; /* paddle masters */
        else if (c->genome.detail == DETAIL_FIN) max_speed *= 1.20f;
        else if (c->genome.legs == LEGS_STUB) max_speed *= 0.45f;
        else max_speed *= 0.62f;
    } else if (!will_swim && c->genome.legs == LEGS_FLIPPER) {
        max_speed *= 0.72f; /* awkward on land */
    }

    if (c->charge_t > 0.0f) {
        c->charge_t -= dt;
        wish = v3(sinf(c->yaw), 0.0f, cosf(c->yaw));
        wish_len = 1.0f;
        max_speed = c->max_speed * 2.4f;
        if (c->genome.weapon == WEAPON_HORN) max_speed *= 1.15f; /* horned charge */
        c->speed = max_speed;
    }

    /* Airborne: weaker steering, wings keep forward speed */
    int gliding = c->airborne && c->genome.detail == DETAIL_WING && c->vel_y < 0.5f;
    if (gliding) max_speed *= 1.15f;

    if (wish_len > 1e-4f) {
        wish = v3_mul(wish, 1.0f / wish_len);
        if (c->charge_t <= 0.0f) {
            float turn = c->airborne ? 4.0f : 10.0f;
            float target_yaw = atan2f(wish.x, wish.z);
            float dy = target_yaw - c->yaw;
            while (dy > (float)M_PI) dy -= 2.0f * (float)M_PI;
            while (dy < -(float)M_PI) dy += 2.0f * (float)M_PI;
            c->yaw += dy * clampf(turn * dt, 0.0f, 1.0f);
            float a = c->airborne ? accel * 0.45f : accel;
            c->speed = clampf(c->speed + a * dt, 0.0f, max_speed);
        }
    } else {
        float decel = c->airborne ? accel * 0.4f : accel * 1.4f;
        c->speed = fmaxf(0.0f, c->speed - decel * dt);
    }
    if (c->speed > max_speed) c->speed = max_speed;

    Vec3 fwd = world_forward(c);
    if (c->speed > 1e-4f)
        c->pos = v3_add(c->pos, v3_mul(fwd, c->speed * dt));

    float margin = 3.0f;
    c->pos.x = clampf(c->pos.x, margin, t->world_size - margin);
    c->pos.z = clampf(c->pos.z, margin, t->world_size - margin);
    float ground = terrain_height(t, c->pos.x, c->pos.z);
    c->in_water = terrain_is_water(t, c->pos.x, c->pos.z);

    /* Jump / vertical */
    float float_line = c->in_water ? (t->water_y + 0.05f) : ground;
    if (want_jump && c->grounded && c->jump_cd <= 0.0f && c->charge_t <= 0.0f) {
        c->vel_y = c->jump_power;
        c->grounded = 0;
        c->airborne = 1;
        c->jump_cd = 0.35f;
        if (c->in_water) c->vel_y *= 0.65f; /* weaker leap from water */
    }

    if (!c->grounded || c->elev > 0.02f || c->vel_y > 0.0f) {
        float g = 18.0f;
        if (gliding) {
            /* Soft fall + slight lift from forward speed */
            g = 5.5f;
            c->vel_y += c->speed * 0.15f * dt;
            if (c->vel_y < -3.5f) c->vel_y = -3.5f;
        }
        c->vel_y -= g * dt;
        c->elev += c->vel_y * dt;
        c->airborne = 1;
        c->grounded = 0;
        if (c->elev <= 0.0f) {
            c->elev = 0.0f;
            c->vel_y = 0.0f;
            c->grounded = 1;
            c->airborne = 0;
        }
    } else {
        c->elev = 0.0f;
        c->vel_y = 0.0f;
        c->grounded = 1;
        c->airborne = 0;
    }

    c->pos.y = float_line; /* XZ contact reference */
    /* Stance height from legs — Spore creator "posture" feel */
    float stance = 1.10f;
    if (c->genome.legs == LEGS_STUB) stance = 0.82f;
    else if (c->genome.legs == LEGS_SPRINT) stance = 1.28f;
    else if (c->genome.legs == LEGS_CLIMB) stance = 1.00f;
    else if (c->genome.legs == LEGS_JUMP) stance = 1.18f;
    stance *= STANCES[c->genome.stance].upright; /* biped stands taller */
    stance *= REACHES[c->genome.reach].posture;  /* stubby / lanky stretch */
    if (genome_limb_count(&c->genome) == 2) stance *= 1.20f;
    if (c->crouching) stance *= 0.58f; /* belly-low sneak */
    float body_base;
    if (c->in_water && !c->airborne) {
        body_base = t->water_y + 0.25f * c->scale
                  + 0.06f * c->scale * sinf(c->gait_timer * 3.0f);
    } else {
        body_base = ground + stance * c->scale
                  + (c->airborne ? 0.0f : 0.04f * c->scale * sinf(c->gait_timer * 2.0f));
    }
    float body_y = body_base + c->elev;
    c->body = v3(c->pos.x, body_y, c->pos.z);
    float spine_l = SPINES[c->genome.spine].length;
    float neck_r = NECKS[c->genome.neck].reach;
    c->head = v3_add(c->body, v3_add(v3_mul(fwd, 0.50f * spine_l * c->scale),
                                     v3(0, 0.28f * neck_r * c->scale, 0)));
    /* Articulated neck + swaying tail shaped by morphs */
    {
        float sway = c->airborne ? 0.0f : 0.08f * c->scale * sinf(c->gait_timer * 2.4f);
        c->neck = v3_lerp(c->body, c->head, 0.40f + 0.08f * neck_r);
        c->neck.y += 0.05f * neck_r * c->scale;
        Vec3 back = v3_mul(fwd, -0.70f * spine_l * c->scale);
        c->tail = v3_add(c->body, v3(back.x + world_right(c).x * sway,
                                     -0.05f * c->scale,
                                     back.z + world_right(c).z * sway));
    }
    if (c->spine.active) spine_sync_anchors(c);

    float stride = 0.85f * c->scale * c->stride_scale;
    float step_speed = (max_speed > 1e-4f) ? (c->speed / max_speed) : 0.0f;
    if (!c->airborne)
        c->gait_timer += dt * (1.2f + step_speed * 3.5f);
    Vec3 right = world_right(c);
    int nlegs = genome_limb_count(&c->genome);

    for (int i = 0; i < CREATURE_LEGS; i++) {
        Leg *L = &c->legs[i];
        L->hip = body_to_world(c, c->hip_local[i]);

        /* Inactive limb sockets — keep tucked, no IK plant */
        if (i >= nlegs) {
            L->swinging = 0;
            float tuck = 0.18f * c->scale;
            L->foot = v3(L->hip.x, c->body.y - tuck, L->hip.z);
            L->plant = L->foot;
            resolve_leg_ik(L, v3_add(L->hip, v3(0, -1.0f, 0)));
            continue;
        }

        /* Stance biped + quad: rear legs stay tucked as decorative thighs */
        if (c->genome.stance == STANCE_BIPED && nlegs == 4 && i >= 2 && !c->airborne) {
            L->swinging = 0;
            float tuck = 0.22f * c->scale;
            L->foot = v3(L->hip.x, c->body.y - tuck, L->hip.z);
            L->plant = L->foot;
            resolve_leg_ik(L, v3_add(L->hip, v3(0, -1.0f, 0)));
            continue;
        }

        if (c->airborne) {
            /* Tuck legs under body while jumping / gliding */
            L->swinging = 0;
            float tuck = 0.35f * c->scale;
            L->foot = v3(L->hip.x, c->body.y - tuck, L->hip.z);
            L->plant = L->foot;
            resolve_leg_ik(L, v3_add(L->hip, v3(0, -1.0f, 0)));
            continue;
        }

        if (c->in_water) {
            L->swinging = 0;
            float paddle = 0.15f * sinf(c->gait_timer * 6.0f + L->phase * 6.28f);
            L->foot = v3(L->hip.x, t->water_y - 0.1f + paddle, L->hip.z);
            L->plant = L->foot;
            resolve_leg_ik(L, v3_add(L->hip, v3(0, -1.0f, 0)));
            continue;
        }

        float duty = fmodf(c->gait_timer + L->phase, 1.0f);
        int want_swing = (duty < 0.35f) && (c->speed > 0.4f);

        if (!L->swinging && want_swing) {
            L->swinging = 1;
            L->swing_t = 0.0f;
            L->swing_from = L->plant;
            Vec3 ahead = v3_mul(fwd, stride * 0.55f * (step_speed + 0.35f));
            Vec3 side = v3_mul(right, c->hip_local[i].x * 0.15f * c->scale);
            Vec3 target = v3_add(v3_add(L->hip, ahead), side);
            target.x = lerpf(target.x, L->hip.x, 0.35f);
            target.z = lerpf(target.z, L->hip.z, 0.35f);
            target.y = terrain_height(t, target.x, target.z);
            L->swing_to = target;
        }

        if (L->swinging) {
            float swing_dur = 0.28f / (0.6f + step_speed);
            L->swing_t += dt / swing_dur;
            if (L->swing_t >= 1.0f) {
                L->swing_t = 1.0f;
                L->swinging = 0;
                L->plant = L->swing_to;
                L->foot = L->plant;
            } else {
                float u = smooth_ease(L->swing_t);
                L->foot = v3_lerp(L->swing_from, L->swing_to, u);
                float lift = 0.35f * c->scale * sinf(u * (float)M_PI);
                L->foot.y = terrain_height(t, L->foot.x, L->foot.z) + lift;
            }
        } else {
            float dx = L->hip.x - L->plant.x;
            float dz = L->hip.z - L->plant.z;
            float stretch = sqrtf(dx * dx + dz * dz);
            if (stretch > stride * 1.15f && c->speed > 0.2f) {
                L->swinging = 1;
                L->swing_t = 0.0f;
                L->swing_from = L->plant;
                Vec3 target = v3_add(v3(L->hip.x, 0, L->hip.z), v3_mul(fwd, stride * 0.4f));
                target.y = terrain_height(t, target.x, target.z);
                L->swing_to = target;
            } else {
                L->plant.y = terrain_height(t, L->plant.x, L->plant.z);
                L->foot = L->plant;
            }
        }

        Vec3 pole = v3_add(L->hip, v3_add(v3_mul(fwd, 0.1f), v3(0, -1.5f, 0)));
        resolve_leg_ik(L, pole);
    }
}

Vec3 creature_weapon_anchor(const Creature *c) {
    float s = c->scale;
    Vec3 fwd = world_forward(c);
    Vec3 rt = world_right(c);
    int sock = c->genome.wpn_sock;
    if (c->genome.weapon == WEAPON_HORN) sock = WSOCK_HEAD;
    if (sock == WSOCK_HEAD) {
        Vec3 tip = v3_add(c->head, v3_mul(fwd, 0.32f * s));
        tip.y += 0.12f * s;
        return tip;
    }
    if (sock == WSOCK_TAIL) {
        Vec3 tip = v3_add(c->tail, v3_mul(fwd, -0.20f * s));
        tip.y += 0.08f * s;
        return tip;
    }
    /* Flank — snap to spine vertebra, then push out sideways */
    int vi = creature_slide_vertebra(c, c->genome.wpn_slide);
    Vec3 bead = creature_spine_bead(c, vi);
    float u = (float)c->genome.wpn_slide / (float)(SLIDE_COUNT - 1);
    float out = lerpf(0.38f, 0.52f, u) * s;
    Vec3 tip = v3_add(bead, v3_mul(rt, out));
    tip.y += 0.06f * s;
    return tip;
}

Vec3 creature_detail_anchor(const Creature *c) {
    float s = c->scale;
    if (c->genome.dtl_sock == DSOCK_CREST)
        return v3_add(c->head, v3(0, 0.28f * s, 0));
    if (c->genome.dtl_sock == DSOCK_RUMP)
        return v3_add(c->tail, v3(0, 0.22f * s, 0));
    /* Back — sit on the chosen vertebra */
    int vi = creature_slide_vertebra(c, c->genome.dtl_slide);
    Vec3 back = creature_spine_bead(c, vi);
    back.y += 0.22f * s;
    return back;
}

int creature_spine_count(const Creature *c) {
    if (c->spine.active && c->spine.count >= SPINE_MIN_VERTS)
        return c->spine.count;
    Genome g = c->genome;
    genome_clamp(&g);
    int segs = SPINES[g.spine].segs;
    return segs < 3 ? 3 : segs;
}

Vec3 creature_spine_bead(const Creature *c, int index) {
    if (c->spine.active && c->spine.count >= SPINE_MIN_VERTS)
        return spine_world(c, index);
    Genome g = c->genome;
    genome_clamp(&g);
    float s = c->scale;
    float spine_l = SPINES[g.spine].length;
    float body_l = 1.00f * s * spine_l;
    Vec3 fwd = world_forward(c);
    int segs = creature_spine_count(c);
    int i = index;
    if (i < 0) i = 0;
    if (i >= segs) i = segs - 1;
    float u = (segs <= 1) ? 0.0f : (float)i / (float)(segs - 1);
    float along = (0.38f - 0.95f * u) * body_l;
    float arch = 0.06f * s * sinf(u * 3.14159f) * (spine_l - 0.7f);
    Vec3 bead = v3_add(c->body, v3_mul(fwd, along));
    bead.y += arch;
    return bead;
}

float creature_spine_radius(const Creature *c, int index) {
    if (c->spine.active && c->spine.count >= SPINE_MIN_VERTS) {
        int i = index;
        if (i < 0) i = 0;
        if (i >= c->spine.count) i = c->spine.count - 1;
        return c->spine.v[i].radius * c->scale;
    }
    return 0.28f * c->scale;
}

int creature_slide_vertebra(const Creature *c, int slide) {
    int segs = creature_spine_count(c);
    if (slide < 0) slide = 0;
    if (slide >= SLIDE_COUNT) slide = SLIDE_COUNT - 1;
    if (segs <= 1) return 0;
    float u = (float)slide / (float)(SLIDE_COUNT - 1);
    return (int)(u * (float)(segs - 1) + 0.5f);
}

/* --- Spore CC column -------------------------------------------------- */

void spine_init_blob(SpineColumn *s) {
    memset(s, 0, sizeof(*s));
    s->active = 1;
    s->count = 4;
    /* Amorphous starter blob — sagittal midline (x=0), nose at +Z */
    s->v[0] = (Vertebra){ 0.0f, 0.02f,  0.38f, 0.48f };
    s->v[1] = (Vertebra){ 0.0f, 0.05f,  0.12f, 0.58f };
    s->v[2] = (Vertebra){ 0.0f, 0.05f, -0.12f, 0.58f };
    s->v[3] = (Vertebra){ 0.0f, 0.00f, -0.38f, 0.48f };
}

void spine_ensure_blob(Creature *c) {
    if (!c->spine.active || c->spine.count < SPINE_MIN_VERTS) {
        spine_init_blob(&c->spine);
        spine_sync_anchors(c);
    }
}

Vec3 spine_world(const Creature *c, int i) {
    if (!c->spine.active || c->spine.count < 1) return c->body;
    if (i < 0) i = 0;
    if (i >= c->spine.count) i = c->spine.count - 1;
    const Vertebra *v = &c->spine.v[i];
    float cy = cosf(c->yaw), sy = sinf(c->yaw);
    float sx = v->x * c->scale, syy = v->y * c->scale, sz = v->z * c->scale;
    Vec3 local = v3(sx * cy + sz * sy, syy, -sx * sy + sz * cy);
    return v3_add(c->body, local);
}

void spine_sync_anchors(Creature *c) {
    if (!c->spine.active || c->spine.count < SPINE_MIN_VERTS) return;
    c->head = spine_world(c, 0);
    c->tail = spine_world(c, c->spine.count - 1);
    c->neck = v3_lerp(c->body, c->head, 0.55f);
}

static void spine_dir_at_end(const SpineColumn *s, int front, float *dx, float *dy, float *dz) {
    if (s->count < 2) { *dx = 0; *dy = 0; *dz = front ? 1.0f : -1.0f; return; }
    if (front) {
        *dx = s->v[0].x - s->v[1].x;
        *dy = s->v[0].y - s->v[1].y;
        *dz = s->v[0].z - s->v[1].z;
    } else {
        int n = s->count;
        *dx = s->v[n - 1].x - s->v[n - 2].x;
        *dy = s->v[n - 1].y - s->v[n - 2].y;
        *dz = s->v[n - 1].z - s->v[n - 2].z;
    }
    float len = sqrtf((*dx) * (*dx) + (*dy) * (*dy) + (*dz) * (*dz));
    if (len < 1e-4f) { *dx = 0; *dy = 0; *dz = front ? 1.0f : -1.0f; return; }
    *dx /= len; *dy /= len; *dz /= len;
}

int spine_extend(SpineColumn *s, int front) {
    float dx, dy, dz;
    spine_dir_at_end(s, front, &dx, &dy, &dz);
    return spine_extend_dir(s, front, dx, dy, dz);
}

int spine_extend_dir(SpineColumn *s, int front, float dx, float dy, float dz) {
    if (!s->active || s->count >= SPINE_MAX_VERTS) return 0;
    /* Bilateral: no sideways growth — stay in YZ sagittal plane */
    dx = 0.0f;
    float len = sqrtf(dy * dy + dz * dz);
    if (len < 1e-4f) {
        spine_dir_at_end(s, front, &dx, &dy, &dz);
        dx = 0.0f;
        len = sqrtf(dy * dy + dz * dz);
        if (len < 1e-4f) { dy = 0; dz = front ? 1.0f : -1.0f; len = 1.0f; }
    }
    dy /= len; dz /= len;
    float step = 0.22f;
    if (front) {
        memmove(&s->v[1], &s->v[0], (size_t)s->count * sizeof(Vertebra));
        s->v[0].x = 0.0f;
        s->v[0].y = clampf(s->v[0].y + dy * step, -0.8f, 1.4f);
        s->v[0].z = clampf(s->v[0].z + dz * step, -2.2f, 2.2f);
        s->v[0].radius = s->v[1].radius * 0.92f;
        if (s->v[0].radius < 0.18f) s->v[0].radius = 0.18f;
    } else {
        Vertebra tip = s->v[s->count - 1];
        tip.x = 0.0f;
        tip.y = clampf(tip.y + dy * step, -0.8f, 1.4f);
        tip.z = clampf(tip.z + dz * step, -2.2f, 2.2f);
        tip.radius *= 0.92f;
        if (tip.radius < 0.18f) tip.radius = 0.18f;
        s->v[s->count] = tip;
    }
    s->count++;
    return 1;
}

int spine_shorten(SpineColumn *s, int front) {
    if (!s->active || s->count <= SPINE_MIN_VERTS) return 0;
    if (front)
        memmove(&s->v[0], &s->v[1], (size_t)(s->count - 1) * sizeof(Vertebra));
    s->count--;
    return 1;
}

void spine_bend(SpineColumn *s, int i, float dx, float dy, float dz) {
    if (!s->active || i < 0 || i >= s->count) return;
    (void)dx; /* ignore lateral — bilateral symmetry */
    s->v[i].x = 0.0f;
    s->v[i].y = clampf(s->v[i].y + dy, -0.8f, 1.4f);
    s->v[i].z = clampf(s->v[i].z + dz, -2.2f, 2.2f);
}

void spine_aim_end(SpineColumn *s, int front, float tx, float ty, float tz, float seg_len) {
    if (!s || !s->active || s->count < 2) return;
    if (seg_len < 0.08f) seg_len = 0.08f;
    int tip_i = front ? 0 : s->count - 1;
    int prev_i = front ? 1 : s->count - 2;
    /* Project target onto sagittal plane */
    tx = 0.0f;
    float dx = tx - s->v[prev_i].x;
    float dy = ty - s->v[prev_i].y;
    float dz = tz - s->v[prev_i].z;
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 1e-4f) return;
    dx /= len; dy /= len; dz /= len;
    s->v[tip_i].x = 0.0f;
    s->v[tip_i].y = clampf(s->v[prev_i].y + dy * seg_len, -0.8f, 1.4f);
    s->v[tip_i].z = clampf(s->v[prev_i].z + dz * seg_len, -2.2f, 2.2f);
}

void spine_enforce_symmetry(SpineColumn *s) {
    if (!s || !s->active) return;
    for (int i = 0; i < s->count; i++)
        s->v[i].x = 0.0f;
}

void spine_inflate(SpineColumn *s, int i, float delta) {
    /* Lochner-style weight: inflate selected bone, bleed half to neighbours. */
    if (!s || !s->active || i < 0 || i >= s->count) return;
    s->v[i].radius = clampf(s->v[i].radius + delta, 0.12f, 1.20f);
    if (i > 0)
        s->v[i - 1].radius = clampf(s->v[i - 1].radius + delta * 0.5f, 0.12f, 1.20f);
    if (i + 1 < s->count)
        s->v[i + 1].radius = clampf(s->v[i + 1].radius + delta * 0.5f, 0.12f, 1.20f);
}
