# Creating a creature: the minimal steps

The shortest path from a clean checkout to a creature of your own design,
using what exists today. (The drag-and-drop browser editor is planned —
see [PLAN.md](PLAN.md) stage 1.3; until it lands, the creature creator is
the CLI for looking and the Python API for building.)

## 1. Build

```
make && make lib
```

Needs a C compiler, `make`, and nothing else. `make lib` produces the
shared library the Python binding loads.

## 2. See the parts catalogue

```
./build/cpore_land --list-parts
```

19 part types with their DNA prices. A body is up to 16 parts on a spine of
2–6 segments, bought out of a budget (82 DNA at generation 0). Mouths gate
what you can eat, legs/fins/wings/diggers gate where you can go, eyes and
ears are how much of the world you get told about.

## 3. Look at finished creatures first

```
./build/cpore_land --creature --style predator --out predator.png
./build/cpore_land --creature --seed 42 --out random.png
```

Four views plus the stat block its parts actually bought. The six styles
(`grazer`, `predator`, `charmer`, `swimmer`, `flyer`, `burrower`) are the
reference builds worth imitating before designing from scratch.

## 4. Design your own, in Python

Each part is `(name, segment, yaw, pitch, scale, mirror, length, bend)` —
trailing fields optional. `segment` is which spine segment it mounts on,
`yaw` 0–255 around the body, `pitch` -64..63 up/down, `mirror=1` places it
on both flanks (and costs double), `length` and `bend` shape a limb.

```python
from cpore import LandEnv, land_genome

beast = land_genome([
    ("graze", 0, 0, -20),                    # a mouth, or you starve
    ("eye",   0, 40, 10, 128, 1),            # mirrored eyes
    ("leg",   1, 70, -50, 128, 1, 180, 60),  # mirrored front legs
    ("leg",   2, 70, -50, 128, 1, 180, 60),  # mirrored hind legs
    ("tail",  2, 128, 20, 128, 0, 220, 40),
], nseg=3, girth=130)

env = LandEnv(seed=7, genome=beast)
env.reset()
```

Overspending the budget is trimmed by the C side — you cannot smuggle in a
build you have not paid for.

## 5. Let it live, and read the result

```python
for _ in range(2000):
    obs, reward, term, trunc, _ = env.step(env.greedy_action())
    if term or trunc:
        break
print(env.census())          # what it ate, where it went, whether it held up
env.save_png("mybeast.png")  # your creature, in the world
```

If it starved: check the mouth matches the food where it woke up. If it
drowned: fins and gills gate water. If it never left the spawn valley:
longer legs are for exactly that.

That's the whole loop — parts, prices, a body, a life, a verdict. Repeat
from step 4 until the census says the design works.
