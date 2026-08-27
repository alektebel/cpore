# cpore as a PufferLib environment

```
pip install pufferlib "numpy<2"
python3 puffer/setup.py build_ext --inplace     # or: pip install -e puffer/
python3 -m cpore_puffer.cell                    # throughput
```

```python
from cpore_puffer.cell import Cell
env = Cell(num_envs=1024)
obs, _ = env.reset()
obs, rew, term, trunc, info = env.step(actions)
```

One stage so far: the cell. The others exist as simulations already and are not
wrapped yet.

## What is actually here

The simulation is cpore's own C — `src/world.c` and friends, compiled straight
into the extension. This is a second doorway into the same simulation the
ctypes environment steps and both renderers draw, not a second copy of it. The
binding layer is PufferLib's own `ocean/env_binding.h`, taken from the
installed PufferLib rather than vendored, so the two cannot drift apart.

| | |
| --- | --- |
| observation | `Box(97,) float32` — vitals, then the nearest 8 food and 6 cells, egocentric |
| action | `MultiDiscrete([9, 2, 2])` — drift or a compass point, boost, discharge |
| episode | until the meter fills or the clock stops |
| throughput | ~58k steps/s on one core at `num_envs=1024` |

## Three decisions that are the environment's, not the simulation's

**The action space is the one a person plays with.** `cp_world_step` takes a
28-float vector with a design head on the end. A policy learns far better from
nine directions, two buttons; so does a human. The property worth protecting is
that the translation is the same in both directions — a key press and an action
index are the same number — so the agent and the person are playing the same
game rather than two games that resemble each other. Index 0 is *drift*, which
is a real choice in water and not a no-op.

**Death is a setback, not an ending.** Being eaten costs a chunk of the meter
and puts the cell back somewhere else; the episode runs until the meter fills
or the clock stops. That is how the stage is played, and it is also the better
learning signal — an episode that ends on the first mistake teaches an agent to
hide in a corner. `respawn=False` gets the conventional framing back, and the
difference is visible in one run: 3000-step episodes with it on, ~760-step
episodes with it off.

**The editor designs itself, for now.** The meter crossing a segment opens the
editor, and `cp_world_step` reads a design head at exactly that moment. Since
the design head is not in the MultiDiscrete, it is left null and the
simulation's own designer fills it — which is the right behaviour for a policy
that was never given the choice. Handing the editor to a policy is a second
action space and a separate piece of work; the creature-stage editor already
proved out the machinery for it.

## Is it learnable?

The question every new environment has to answer before a training curve means
anything. `env.greedy()` is cpore's scripted baseline projected onto the *same*
nine directions the learner gets — a baseline that steered with a richer action
space would not be answering the question. Over ~1000 episodes at
`episode_len=3000`:

| | random | scripted |
| --- | --- | --- |
| return | −22.5 | **+69.5** |
| meter filled | 0.8% | **95.7%** |
| stage completed | 0% | **69%** |
| size tier reached | 0.0 | 1.9 / 2 |
| times the editor opened | 0.0 | 2.9 |
| food eaten | 7.4 | 80.3 |
| times eaten | 4.2 | 0.1 |

Solvable, and random play gets nowhere near it. That gap is the room a policy
has to learn in.

## Frames

`env.render(env_id, width, height, style)` returns an `(H, W, 4)` uint8 array
drawn by cpore's own renderer. There is no window and no windowing library in
here: the renderer writes bytes and has since before this package existed, so
the same frame goes to a window, a PNG, a video or a browser without the
environment knowing which. `style="drop"` is the continuous-tone darkfield
path; the six pixel-art styles are also available by name.
