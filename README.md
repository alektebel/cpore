# cpore

A Spore-like life simulation written from scratch in C, built to be a
reinforcement learning environment rather than a game.

![cell stage](docs/hero.png)

Dependencies: `libc` and `libm`. No SDL, no OpenGL, no zlib, no stb. The
renderer in that screenshot is a software rasteriser in this repo, and the PNG
it wrote was compressed by a DEFLATE implementation in this repo.

```
make && make test && make bench
./build/cpore_shot --seed 23 --steps 700 --morph 2,1,2,3,0,0 --out shot.png
```

## Why not just use Spore

Nobody has trained an RL agent on Spore, and nobody has published an LLM
playing it. The reasons are structural, and they are exactly the constraints
this project deletes:

| Spore | cpore |
| --- | --- |
| No scripting API, no headless mode | Sim core is a library with zero I/O |
| Runs at 1x real time, one instance | ~67k steps/s, 64 envs, one thread |
| A playthrough is tens of hours | An episode is 6000 steps (~2s of compute) |
| Five games with five interfaces | One world struct, per-stage rule sets |
| No way to save/restore mid-run | `memcpy`-able 22KB state |

The only real hook into the actual game is the
[Spore ModAPI](https://github.com/emd4600/Spore-ModAPI), a C++ library built on
Ghidra-assisted reverse engineering. It is enough to script the game — people
have written auto-expand/auto-terraform mods with it — but it does not fix
throughput, so it is a demo path, not a training path.

## What is implemented

Stage 1 of 5 (**Cell**), end to end and trainable.

- **World** — 2400x1400 bounded pool, 640 food slots, 48 NPC cells with
  diet-driven steering, predation between NPCs, corpses that become food, and
  repopulation that scales difficulty with the player's progress.
- **The editor as an action space.** A body plan is six integers drawn from a
  shared budget of 8 parts: filter mouths, jaws, spikes, cilia, flagella,
  electric. Every part trades something away — mass costs speed, a mouth you
  did not buy is food you cannot eat. The agent picks its embodiment, then has
  to live with it. This is the part that is actually novel; "reach the galactic
  core" is not.
- **Observations** — 84 floats, fully egocentric: self state, 8 nearest edible
  food, 6 nearest cells (relative size, threat, velocity), own body plan.
- **Actions** — 3 floats: steering x/y, plus a flagella burst trigger.
- **Reward** — dense DNA/health shaping, a small time cost, +1 per kill,
  −5 on death, +25 on evolving. Terminal states: `dead`, `evolved`, `timeout`.
- **Determinism** — the RNG lives inside the world struct. Same seed, same
  trajectory, bit for bit. Snapshot/restore round-trips the entire stochastic
  future, which is what makes replay, mid-episode curricula and tree search
  possible later.

## Layout

```
include/cpore/cpore.h   one public header, flat C ABI
src/world.c             the simulation (no I/O, no allocation, no globals)
src/morph.c             body plan -> derived stats
src/policy.c            scripted baseline
src/env.c               RL wrapper: reset/step/observe/save/load
src/render.c            software rasteriser (optional, links separately)
src/png.c               PNG + DEFLATE encoder
python/cpore/           ctypes binding, works without numpy
apps/                   cpore_shot, cpore_bench
tests/test_core.c       determinism, snapshot, obs bounds, termination
```

The sim never includes the renderer, so a training build can drop
`render.c`/`png.c` entirely.

## Python

```python
from cpore import CporeEnv
env = CporeEnv()
obs = env.reset(seed=23, morph=(2, 1, 2, 3, 0, 0))   # 2 mouths, 1 jaw, 2 spikes, 3 cilia
obs, reward, terminated, truncated, info = env.step([1.0, 0.0, 0.0])
env.save_png("frame.png")
```

`make lib && python3 python/smoke_test.py` checks the whole path. `make_gym_env()`
returns a `gymnasium.Env` if gymnasium and numpy are installed; nothing else
requires them.

## Numbers

Single thread, `-O2`, on the machine this was developed on:

```
world state: 22488 bytes/env   obs dim: 84   act dim: 3

64 envs                                  1 env
  step only              67k steps/s       159k steps/s
  step + observe         62k steps/s       139k steps/s
  step + observe + base  54k steps/s       131k steps/s
```

Read that as roughly **47M entity-updates/s** — every step advances ~700
entities, so this is not comparable to a 1M-steps/s Pong. The 64-env number is
lower than the 1-env number because 64 worlds is a 1.4MB working set and every
step touches all of it; that is the next thing to fix (structure-of-arrays,
then shard across cores).

Getting here took five passes: the first working version ran at 28k steps/s
aggregate. The wins, in order of size, were re-planning NPC foraging targets in
a staggered burst instead of every cell every frame, maintaining the food
lookup grid incrementally instead of rebuilding it each step, and resolving
NPC collisions through a coarse grid instead of all-pairs.

## Baseline

`cp_policy_greedy` is a scripted heuristic with no memory and no notion of the
DNA goal. It is the bar a learned policy has to clear. Over 20 seeds:

| body plan | parts | evolved | died | median episode |
| --- | --- | --- | --- | --- |
| grazer | 2 mouth, 4 cilia, 1 flagella | 20/20 | 0 | 829 |
| speedy | 1 mouth, 4 cilia, 2 flagella | 20/20 | 0 | 1290 |
| omnivore | 1/1 mouths, 1 spike, 3 cilia | 20/20 | 0 | 1596 |
| hunter | 2 jaws, 3 spikes, 2 cilia | 19/20 | 0 | 2158 |
| brute | 2 jaws, 4 spikes, 2 cilia | 18/20 | 0 | 2704 |
| tank | 1/1 mouths, 4 spikes, 1 electric | 20/20 | 0 | 2946 |
| minimal | 1 mouth, nothing else | 17/20 | 3 | 3144 |

Every branch of the editor is viable and the spread is real, which is the
property that makes morphology worth learning. Three balance bugs had to be
fixed to get there, and all three were found by running the table rather than
by reading the code:

- Pure carnivores could not bootstrap — no meat exists until something dies —
  so a fraction of ambient food is now dead plankton.
- The baseline refused to hunt below 55% health, which put predators in a
  starvation spiral: too weak to hunt, so only scavenging, so still too weak.
- The baseline pinned itself against its own wall-avoidance threshold. A
  constant push switched on at a fixed distance exactly cancels a unit-length
  target attraction, so the agent parked on that line and starved with prey
  100px away.

Known remaining weaknesses in the baseline, deliberately left in place: it has
no memory, does not path around threats, does not manage health, and ignores
the DNA goal entirely. The grazer being fastest is partly an artifact of a
heuristic that is good at chasing pellets.

## Roadmap

1. Structure-of-arrays world layout, then shard envs across cores.
2. PPO on the cell stage, jointly over morphology and control — does a learned
   policy reorder that table?
3. Creature stage: the same world struct, land/water regions, a 3D-ish body
   plan with limbs, and pack behaviour. Stage transition carries state forward
   rather than restarting.
4. Tribal, Civ, Space as further rule sets over the same state.

## Legal

Mechanics are reimplemented from scratch. No Spore assets, model data, or file
formats are used, and none should be added. All art here is procedural and
generated by `src/render.c`.
