#!/usr/bin/env python3
"""End-to-end check of the ctypes binding. Runs without numpy or gymnasium.

    make lib && python3 python/smoke_test.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cpore import CporeEnv, CporeVecEnv, genome, GEN_BUDGET, FRONT, BACK

fails = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        fails.append(msg)


env = CporeEnv()
obs = env.reset(seed=23)
check(len(obs) == env.obs_dim, f"reset returns {env.obs_dim}-dim observation")
check(all(abs(v) <= 3.0 for v in obs), "observation is in range")
check(env.act_dim == 4 + env.max_parts * 2,
      f"action is 4 control dims + {env.max_parts} design slots = {env.act_dim}")
check(env.genome() == [("filter", 0), ("cilia", 96), ("cilia", 160)],
      f"starter cell is {env.genome()}")

ret, steps = 0.0, 0
while True:
    o, r, term, trunc, info = env.step(env.greedy_action())
    ret += r
    steps += 1
    if term or trunc:
        break
check(steps > 100, f"baseline episode ran {steps} steps, return {ret:.1f}")
check(info["status"] in ("evolved", "dead", "truncated"),
      f"episode ended with status={info['status']}")
check(env.generation > 0,
      f"reached generation {env.generation + 1}, final build {env.genome()}")
check(env.genome_cost() <= GEN_BUDGET[env.generation],
      f"final build costs {env.genome_cost()} of {GEN_BUDGET[env.generation]} DNA")

# a caller cannot smuggle in a build it has not paid for
greedy = genome(*[("electric", i * 20) for i in range(12)])
env.reset(seed=1, parts=greedy)
check(env.genome_cost() <= GEN_BUDGET[0],
      f"12 electric parts trimmed to {env.genome_cost()} DNA: {env.genome()}")

# snapshot / restore across the ctypes boundary
env.reset(seed=5)
for _ in range(200):
    env.step(env.greedy_action())
blob = env.save_state()
a = [env.step(env.greedy_action())[1] for _ in range(60)]
env.load_state(blob)
b = [env.step(env.greedy_action())[1] for _ in range(60)]
check(a == b, f"save/load reproduces {len(a)} rewards exactly ({len(blob)} byte state)")
env.close()


def run(parts, seed=23, n=9000):
    e = CporeEnv()
    e.reset(seed=seed, parts=parts)
    tot = 0.0
    for _ in range(n):
        _, r, te, tr, info = e.step(e.greedy_action())
        tot += r
        if te or tr:
            break
    out = (info["status"], tot)
    e.close()
    return out

# placement, not just parts: the same spike aimed forward or aft
fwd = genome(("jaw", FRONT), ("cilia", 112), ("cilia", 144), ("spike", FRONT))
aft = genome(("jaw", FRONT), ("cilia", 112), ("cilia", 144), ("spike", BACK))
sf, rf = run(fwd)
sa, ra = run(aft)
print(f"        spike forward: {sf} {rf:.1f} | spike aft: {sa} {ra:.1f}")
check(True, "placement variants both run (see tests/test_core.c for the "
            "controlled measurement)")

# throughput through the binding (this is the slow path, on purpose)
vec = CporeVecEnv(16, seed=100)
vec.reset()
acts = [[0.4, -0.3] + [0.0] * (vec.act_dim - 2)] * 16
t0 = time.time()
N = 400
for _ in range(N):
    vec.step(acts)
dt = time.time() - t0
print(f"        ctypes vec throughput: {N * 16 / dt:,.0f} steps/s (16 envs)")
vec.close()

print("\nall smoke tests passed" if not fails else f"\n{len(fails)} failed")
sys.exit(1 if fails else 0)
