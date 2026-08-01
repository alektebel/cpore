#!/usr/bin/env python3
"""End-to-end check of the ctypes binding. Runs without numpy or gymnasium.

    make lib && python3 python/smoke_test.py
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from cpore import CporeEnv, CporeVecEnv, PART_NAMES

fails = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        fails.append(msg)


env = CporeEnv()
obs = env.reset(seed=23, morph=(2, 1, 2, 3, 0, 0))
check(len(obs) == env.obs_dim, f"reset returns {env.obs_dim}-dim observation")
check(all(abs(v) <= 4.0 for v in obs), "observation is in range")

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

# snapshot / restore across the ctypes boundary
env.reset(seed=5)
for _ in range(120):
    env.step(env.greedy_action())
blob = env.save_state()
a = [env.step(env.greedy_action())[1] for _ in range(60)]
env.load_state(blob)
b = [env.step(env.greedy_action())[1] for _ in range(60)]
check(a == b, f"save/load reproduces {len(a)} rewards exactly ({len(blob)} byte state)")

# morphology matters: a build with no jaws cannot convert meat into DNA
def run(morph, seed=23, n=1500):
    e = CporeEnv()
    e.reset(seed=seed, morph=morph)
    tot = 0.0
    for _ in range(n):
        _, r, te, tr, _ = e.step(e.greedy_action())
        tot += r
        if te or tr:
            break
    e.close()
    return tot

grazer = run((2, 0, 0, 4, 1, 0))
hunter = run((0, 2, 3, 2, 0, 0))
glass = run((1, 0, 0, 0, 0, 0))
print(f"        grazer {grazer:6.1f} | hunter {hunter:6.1f} | minimal {glass:6.1f}")
check(grazer != hunter, "different body plans produce different returns")

# throughput through the binding (this is the slow path, on purpose)
vec = CporeVecEnv(16, seed=100)
vec.reset()
acts = [[0.4, -0.3, 0.0]] * 16
t0 = time.time()
N = 400
for _ in range(N):
    vec.step(acts)
dt = time.time() - t0
print(f"        ctypes vec throughput: {N * 16 / dt:,.0f} steps/s (16 envs)")
vec.close()
env.close()

print("\nall smoke tests passed" if not fails else f"\n{len(fails)} failed")
sys.exit(1 if fails else 0)
