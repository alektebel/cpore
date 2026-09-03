# cpore roadmap

Everything the project still needs, in the order it makes sense to do it.
Items are struck through as they land.

The game-facing plan — what to build so cpore is genuinely fun to play, and
where each piece of code goes — lives in [GAME_DESIGN.md](GAME_DESIGN.md).
The staged execution order (MVP → depth → creature pollination → the
tribe-and-civ arc) lives in [PLAN.md](PLAN.md); together they sequence most
of the items below.

## Done

- ~~**Widen the palette's hue coverage.**~~ 32 entries rebalanced: seven water
  steps with the darkest two reserved for silhouette, plus greens, warms,
  cyans, magentas and violets so a creature genome survives quantisation.
  Quantiser gained a chroma term so a dim red no longer lands on a mid grey.
- ~~**Rebuild the lighting.**~~ Key/fill/ambient split with a camera-side fill,
  filmic tonemapping instead of hard clipping, and fog held off the first 90
  units so foreground animals keep their colour.
- ~~**SDF ambient occlusion and soft shadows.**~~ Both straight out of the
  distance field; occlusion applies to the terms that come from everywhere,
  not to the key.
- ~~**Ground creatures with seabed shadows.**~~ Projected patches that widen
  and fade with height above the seabed.
- ~~**Caustics and light shafts.**~~ Two interfering ripples squared twice on
  the seabed, and a glow along the sun's bearing through open water.
- ~~**Depth-discontinuity outlines.**~~ Threshold scales with distance so far
  objects do not outline themselves into mush.
- ~~**Marine snow.**~~ Drifting, depth-faded, and streaked along its own motion.

- ~~**Stage 3: the creature stage on land.**~~ A ray-marched heightfield that
  is a pure function of seed and position, seven rival lineages that breed and
  are selected, and the impress-or-eat fork bought out of one DNA budget. All
  three starting builds are viable and each wins by its own mechanic.
- ~~**Stage 4: the civilisation stage.**~~ The same planet from above. Force,
  trade and faith as three genuinely different mechanics — they cost different
  things and leave the captured city in measurably different states — and the
  body a species evolved in stage 3 arrives as the multipliers that decide its
  doctrine.

- ~~**Four media, and a world with no edges.**~~ Ground, water, air and soil,
  each gated by a part and each with its own food. The bounded box is gone:
  terrain was always a pure function, so what keeps the world finite is now a
  resident window that recycles flora, animals and whole species from behind
  the player to in front of them. Snapshot, determinism and throughput are
  unchanged.
- ~~**Nests the player builds.**~~ Bank food, heal, and hatch followers that
  carry a mutated copy of your own genome.
- ~~**A spatial hash for stage 3's flora.**~~ The NPC feeding pass was
  beasts x flora every step and held the stage to 7.6k steps/s; hashed cells
  took it to 25.5k without changing any behaviour the tests can see.
- ~~**Species that remember you.**~~ Wariness, a learned guard against being
  struck from behind, and fatigue at a repeated display - all inside one
  episode, all fading, and all reported to the agent.
- ~~**Day and night.**~~ A cycle every 2000 steps that takes sight and leaves
  hearing, so an ear finally beats an eye at midnight. Night sky, stars, and a
  sun that actually rises and sets.
- ~~**Biomes.**~~ Temperature and moisture as two more pure functions of
  position, eight biomes off the pair, and fertility varying fivefold across
  them so a biome is a mechanic rather than a paint job. The same field colours
  the stage-4 map, because it is the same planet.
- ~~**Stage 5: the tribe stage.**~~ Members, stores, tools, huts and five
  neighbours on the same planet; raid and befriend as two measured mechanics
  (19/30 vs 15/30 over 30 seeds); imported share-code genomes found rival
  tribes, so invasions ride the arc into civ.
- ~~**The vector core and the pufferlib path.**~~ `src/vec.c` batches N
  worlds behind one call with flat buffers C writes straight into numpy;
  `python/cpore/puffer.py` is a native `PufferEnv` per stage (133k/16k
  steps/s cell/land, 64 envs). The old ctypes loop stays as the slow path.
- ~~**Share codes.**~~ Every genome as a pasteable, checksum-checked string
  (`src/genome_codec.c`), with live apply + redesign entry points at the C
  and Python levels. No server; the string is the transport.
- ~~**A real campaign wrapper.**~~ `python/cpore/campaign.py` runs
  cell → aqua → land → tribe → civ as one episode with legacy handed
  forward (seed 25 plays the full arc under the scripted baseline).
- ~~**The text face for LLMs.**~~ `python/cpore/textgym.py`: situation
  reports + a verb DSL with frame-skip, JSONL transcripts, fixed-seed
  leaderboard scaffold.
- ~~**The native game.**~~ `apps/cpore_game.c` + `src/glview.c`: all five
  stages playable in one X11+GL window, GPU-presented, with an editor-lite
  (N), share codes (C) and full-quality photo stills (F). WASM is demoted
  to a legacy target; training and iteration happen natively.

## Next

**8. Slim the HUD.** Two large filled panels eat the corners. Thin
edge-aligned readouts instead, centre of frame kept clear.

**9. Make player predation a real selection pressure.** The player killing
fish already removes genomes from the pool, but weakly. Strengthen it and
verify the population measurably shifts away from whatever body type the
player hunts — the census panel already makes that legible.

**10. In-session memory, for stage 2.** Stage 3 has it: wariness, guard and
display fatigue, all decaying, all in the observation. The aquatic stage still
has none — its fish react to what you are, never to what you have done.

**11. Evolving population in the cell stage.** Stage 1's NPCs are still
scripted with fixed stats. Give them genomes, energy, breeding and mutation so
every stage shares one selection machinery.

**11b. Stage 6: space.** The last stage Spore had, and the only one still
missing. It wants a different shape again — a galaxy of star systems rather
than one planet — so it is worth doing only once the five that exist are
properly balanced rather than merely working.

**11c. A real campaign wrapper.** ~~All four stages chain today, but only by
hand in Python~~ Done: `python/cpore/campaign.py` runs the five-stage arc as
one episode with legacy handed forward. What remains is the fixed-shape
padded-observation variant that lets one PPO policy train across the arc —
that is the long-horizon credit-assignment benchmark proper.

## Larger

**12. Weather, and stage 2's turn.** Stage 3 now varies in space (eight
biomes) and in time (a day/night cycle that takes sight and leaves hearing).
What is still missing is weather — rain that cuts visibility, wind that pushes
a flier, seasons that move the biome boundaries over an episode. Stage 2 is
also still a fixed box and should inherit both the resident-window treatment
and a substrate/current equivalent of biomes.

**13. Articulated physics and learned locomotion.** Bodies do not swim. Thrust
is applied along a heading and the undulation is a decorative sine disconnected
from the physics. Needs jointed segments with per-segment fluid drag, then a
morphology-conditioned policy (GNN or transformer over the body graph) so one
controller generalises across body plans. Full articulation for the player and
nearest N, cheap kinematics for the rest.

**14. WASM build and browser editor.** Demoted: the native game
(`apps/cpore_game.c`) is the play path now, and training happens natively.
The emscripten target still builds (`make wasm`) for link-sharing, but the
drag-and-drop browser editor waits until the native game says it is needed.

**15. Animated output and an offline path tracer.** APNG is a short step from
the existing DEFLATE encoder, and stills badly undersell a world whose whole
point is motion. Separately a third render path — SDF path tracing with
volumetric single scattering, real caustics and depth of field — for showcase
frames at seconds per frame, leaving the training path untouched.

## Deliberately not doing

- **A mesh pipeline.** Ray-marched SDFs sidestep meshing, UVs, rigging and
  skinning, which is precisely why they suit bodies that are generated rather
  than authored. Adopting meshes would reintroduce every problem the current
  design avoids.
- **Neural networks in the C inner loop.** The policy boundary already exists
  and lives in Python. Keeping the core dependency-free is the project's
  actual selling point.
- **A learned generative prior over morphology.** Worth it only once a corpus
  of plausible body plans exists; trained on this generator's own output it
  would be circular. A developmental grammar gets the same guarantee with no
  dataset.
