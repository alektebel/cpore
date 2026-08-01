# cpore

A Spore-like life simulation written from scratch in C, built to be a
reinforcement learning environment rather than a game.

![cell stage](docs/hero.png)

Dependencies: `libc` and `libm`. No SDL, no OpenGL, no zlib, no stb.

That screenshot is a full pixel-art pipeline written from scratch: the scene is
rasterised into a 320x180 buffer with hard-edged primitives (coverage is
thresholded at the pixel centre, never blended), quantised to a fixed 32-colour
palette with 4x4 ordered dithering, then blown up 4x with nearest-neighbour.
The PNG it wrote was compressed by a DEFLATE implementation in this repo — the
palette also cut the file from ~500KB to ~64KB, since 32 colours is very
friendly to LZ77.

```
make && make test && make bench
./build/cpore_shot --list-parts
./build/cpore_shot --style hunter --seed 23 --steps 2600 --out shot.png
./build/cpore_shot --parts 2:0,7:16,4:112,4:144 --out custom.png
```

## Why not just use Spore

Nobody has trained an RL agent on Spore, and nobody has published an LLM
playing it. The reasons are structural, and they are exactly the constraints
this project deletes:

| Spore | cpore |
| --- | --- |
| No scripting API, no headless mode | Sim core is a library with zero I/O |
| Runs at 1x real time, one instance | ~81k steps/s, 64 envs, one thread |
| A playthrough is tens of hours | An episode is ≤9000 steps (~2s of compute) |
| Five games with five interfaces | One world struct, per-stage rule sets |
| No way to save/restore mid-run | `memcpy`-able 22KB state |

The only real hook into the actual game is the
[Spore ModAPI](https://github.com/emd4600/Spore-ModAPI), a C++ library built on
Ghidra-assisted reverse engineering. It is enough to script the game, but it
does not fix throughput, so it is a demo path, not a training path.

## The editor

Stage 1 of 5 (**Cell**), with Spore's full cell-stage part roster. A genome is
up to 12 parts, each with a type and a body-relative mounting angle, bought
with DNA out of a per-generation budget.

| part | DNA | what it does |
| --- | --- | --- |
| filter mouth | 5 | eats plants |
| jaw | 10 | eats meat, and bites other cells through a 54° arc |
| proboscis | 16 | eats both, less efficiently than either specialist |
| cilia | 5 | sustained speed |
| flagella | 10 | acceleration, and the burst trigger |
| jet | 16 | top speed — but only its rearward component pushes |
| spike | 9 | contact damage through a 41° arc, and armour over that arc |
| electric | 20 | radial discharge on trigger, costs health, 1.6s cooldown |
| poison | 14 | retaliation damage to whatever is biting that side |
| eye | 6 | perception radius, 210 → 620 units |

Generation budgets are 30 / 55 / 85 / 125 DNA. Every part trades something
away: mass costs speed, weapons cost mobility, and a mouth you did not buy is
food you cannot eat.

**Placement is read by the simulation, not just the renderer.** Contact
damage, armour, poison retaliation and jet thrust are all resolved against the
angle a part is mounted at. Measured directly (`make test`, with the player
pinned and a target held dead ahead):

```
spike forward: dealt 600  taken 624   |  spike aft: dealt 0  taken 760
jet aft thrust 165                    |  jet forward thrust 0
cell sightings over 1200 steps - blind: 1626  |  two eyes: 3762
```

Same parts, same cost, different outcome.

## Generations

Spore hands you the editor every time the DNA meter fills a segment. Here the
**design head of the action vector is sampled at exactly that moment** and
ignored on every other step, which keeps the whole thing a plain Box action
space that ordinary PPO can drive:

```
action[0..1]   steering x/y
action[2]      flagella burst
action[3]      electric discharge
action[4..27]  (part type, mount angle) x 12 slots   <- read at generation boundaries
```

A policy that only drives the first four dimensions leaves the design head at
exactly zero; that is the signal to fall back on the scripted designer, so a
control-only agent still works out of the box.

Observations are 97 floats: self state, 8 nearest edible food, 6 nearest cells,
own part counts, and derived stats. Food and cells beyond the build's
perception radius are zeroed — eyes buy information.

## Layout

```
include/cpore/cpore.h   one public header, flat C ABI
src/genome.c            parts, costs, budgets, the editor's action decoding
src/world.c             the simulation (no I/O, no allocation, no globals)
src/policy.c            scripted baseline, design head included
src/env.c               RL wrapper: reset/step/observe/save/load
src/render.c            pixel-art rasteriser: 320x180, 32 colours, dithered
src/png.c               PNG + DEFLATE encoder
python/cpore/           ctypes binding, works without numpy
apps/                   cpore_shot, cpore_bench
tests/test_core.c       determinism, snapshot, budgets, placement mechanics
```

The sim never includes the renderer, so a training build can drop
`render.c`/`png.c` entirely.

## Python

```python
from cpore import CporeEnv, genome, FRONT, BACK

env = CporeEnv()
obs = env.reset(seed=23, parts=genome(
    ("jaw",   FRONT),      # teeth in front
    ("spike", 16),
    ("cilia", 112),        # propulsion aft
    ("cilia", 144),
    ("eye",   32),
))
obs, reward, terminated, truncated, info = env.step(env.greedy_action())
print(env.genome(), env.genome_cost(), env.generation)
env.save_png("frame.png")
```

`make lib && python3 python/smoke_test.py` checks the whole path. Overspending
is trimmed by the C side, so a caller cannot smuggle in a build it has not paid
for. `make_gym_env()` returns a `gymnasium.Env` if gymnasium and numpy are
installed; nothing else requires them.

## Numbers

Single thread, `-O2`, on the machine this was developed on:

```
world state: 22776 bytes/env   obs dim: 97   act dim: 28

64 envs                                  1 env
  step only              81k steps/s       182k steps/s
  step + observe         73k steps/s       178k steps/s
  step + observe + base  60k steps/s       153k steps/s
```

Read that as roughly **30M entity-updates/s** — every step advances ~370
entities, so this is not comparable to a 1M-steps/s Pong. The 64-env number is
lower than the 1-env number because 64 worlds is a 1.4MB working set and every
step touches all of it; structure-of-arrays, then sharding across cores, is the
next fix.

The first working version ran at 28k steps/s aggregate. The wins, in order of
size, were re-planning NPC foraging targets in a staggered burst instead of
every cell every frame, maintaining the food lookup grid incrementally instead
of rebuilding it per step, and resolving NPC collisions through a coarse grid
instead of all-pairs.

## Baseline

`cp_policy_greedy` is a scripted heuristic with no memory and no notion of the
DNA goal. It is the bar a learned policy has to clear. Starting from each
scripted style, 40 seeds each:

| style | evolved | died | mean episode | kills | damage dealt |
| --- | --- | --- | --- | --- | --- |
| grazer | 38/40 | 2 | 2049 | 28 | 526 |
| scout | 37/40 | 3 | 3925 | 191 | 8330 |
| tank | 34/40 | 6 | 3049 | 26 | 551 |
| hunter | 33/40 | 6 | 3900 | 166 | 5990 |

Every branch of the editor is viable, and the styles genuinely play
differently — grazing finishes in half the time, hunting does 15x the damage.

## What had to be fixed to get here

All of it found by running the table, not by reading the code:

- **Pure carnivores could not bootstrap** — no meat exists until something
  dies — so a fraction of ambient food is now dead plankton.
- **The baseline refused to hunt below 55% health**, putting predators in a
  starvation spiral: too weak to hunt, so only scavenging, so still too weak.
- **The baseline pinned itself against its own wall-avoidance threshold.** A
  constant push switching on at a fixed distance exactly cancels a unit-length
  target attraction, so the agent parked on that line and starved with prey
  100px away.
- **Combat was a rounding error.** With 470 food in the pool, grazing filled
  the DNA meter so easily that weapons never mattered — 1 kill per 7,500 steps,
  and the best build was the one that never fought. Thinning the pool to 320
  and trimming plant DNA made the pool contested; kills went up ~15x and the
  weapon parts started earning their cost.
- **The scripted designer bought its signature part first.** At a 30-DNA
  gen-0 budget the tank spent everything on spikes, ended up with no cilia, and
  died to the first thing that chased it. Buy order is now mobility first.
- **`jet_thrust` counted jets instead of reading their angles**, so the
  header's "only rear-facing jets help" was a comment describing code that did
  not exist. Now it sums each nozzle's rearward component.

## Known limits

- Placement is decisive at the mechanic level (see the numbers above), but
  across full episodes under the scripted baseline the effect is **within
  seed-to-seed noise**. The baseline always faces its direction of travel and
  charges prey head-on, so it never exercises the trade-off between forward
  weapons and rear armour. Demonstrating that trade-off needs a learned policy;
  the mechanic is in place and measured, the behaviour is not yet shown.
- The scripted designer spends 107 of 125 DNA at the final generation. It is a
  fixed buy list, not a planner.
- Grazing is still the shortest path. That is partly true to Spore and partly
  an artifact of a heuristic that is good at chasing pellets.

## Roadmap

1. Structure-of-arrays world layout, then shard envs across cores.
2. PPO on the cell stage, jointly over control and the design head — does a
   learned policy reorder that table, and does it learn to put spikes forward?
3. Creature stage: the same world struct, land/water regions, limbs instead of
   membrane-mounted parts, and pack behaviour. The stage transition carries
   state forward rather than restarting.
4. Tribal, Civ, Space as further rule sets over the same state.

## Rendering

The renderer is a debug view, not a product, and that shaped the choices. At
320x180 there is no room for soft shading, so every cell gets a hard dark
keyline, a fill and one highlight — without the keyline everything dissolves
into the water. The player is marked with four corner brackets rather than
concentric rings, because a ring drawn around a 10px cell is just noise on top
of the cell. Cells under 4px across skip their appendages entirely and draw as
blobs; there is nothing to be gained from a 1px spike.

Two HUD elements exist to make the editor legible: a swatch-and-count strip for
the parts owned, and a **placement dial** showing where each part actually sits
on the membrane, front pointing right. Since the simulation resolves damage,
armour and thrust against those angles, the dial is a readout of live state,
not decoration.

The whole file links separately from the sim, so a training build drops it.

## Legal

Mechanics are reimplemented from scratch. No Spore assets, model data, or file
formats are used, and none should be added. All art here is procedural and
generated by `src/render.c`.
