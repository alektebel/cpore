# spore — creature creator

A standalone, single-binary creature creator in C on SDL2 + OpenGL 3.3:
build a body by dragging its spine, paint it, and test-drive it. The mesh
technique (skinned cylinder, hemisphere caps, cosine blend-shape inflate) is
ported from Daniel Lochner's *Creature Creator* — see
[docs/CREATOR_ATTRIBUTION.md](docs/CREATOR_ATTRIBUTION.md).

## Build and run

```
sudo apt install libsdl2-dev libepoxy-dev
make
./creator                          # save goes to data/creatures/creator_mvp.creature
./creator path/to/my.creature      # or choose the output file
```

Header-only 3D math, a small part catalogue and a sum-of-sines height field;
nothing else is linked in.

## Controls

```
  1 / 2 / 3   Build / Paint / Test
  LMB drag    the RED (nose) / GREEN (tail) tip — the column curves toward the
              mouse and grows when you pull past a segment; drag a gold bone
              to bend it; drag empty space to orbit
  scroll      inflate the selected bone (neighbour bleed, Lochner-style)
  = / -       extend / shorten (Shift = front)
  Tab         part slot       Q / E   cycle part
  C / P       paint / pattern (Paint mode)
  [ / ]       select bone
  R           reset           S       save
  Esc         quit
```

## Layout

| file | what |
|---|---|
| `src/creator_main.c` | the app: SDL window, GL 3.3 renderer, Build/Paint/Test loop |
| `src/bodymesh.c` | Lochner skinned body mesh from the spine column |
| `src/creature.c` | locomotion, legs, spine column, weapon/detail anchors |
| `src/genome.c` | part catalogue, morph tables, genome → creature stats |
| `src/desc.c` | named-creature descriptor and its text save |
| `src/terrain.c` | deterministic height field the creature stands on |
| `include/spore/` | the headers above |

## Status / provenance

`creator_main.c`, `creature.c`, `bodymesh.c` and the two original headers are
the Lochner-ported creator. The support layer they `#include` —
`spore/math3d.h`, `spore/genome.h`, `spore/terrain.h`, `spore/desc.h` and
`src/genome.c`, `src/terrain.c`, `src/desc.c` — was never committed and does
not exist in the project's history, so it has been reconstructed here to make
the creator build and run. It is deliberately simple: a small part catalogue,
a sum-of-sines terrain, and a diffable text save. Drop the originals in their
place if you have them.
