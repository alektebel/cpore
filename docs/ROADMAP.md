# cpore roadmap

Everything the project still needs, in the order it makes sense to do it.
Items are struck through as they land.

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

## Next

**8. Slim the HUD.** Two large filled panels eat the corners. Thin
edge-aligned readouts instead, centre of frame kept clear.

**9. Make player predation a real selection pressure.** The player killing
fish already removes genomes from the pool, but weakly. Strengthen it and
verify the population measurably shifts away from whatever body type the
player hunts — the census panel already makes that legible.

**10. In-session creature memory.** Per-creature statistics of player
behaviour (approach angle, favoured depth, attack cadence) biasing their
steering, so tactics get countered within an episode rather than only across
generations. This is where most of the *felt* intelligence lives.

**11. Evolving population in the cell stage.** Stage 1's NPCs are still
scripted with fixed stats. Give them genomes, energy, breeding and mutation so
every stage shares one selection machinery.

**11b. Stage 5: space.** The last stage Spore had, and the only one still
missing. It wants a different shape again — a galaxy of star systems rather
than one planet — so it is worth doing only once the four that exist are
properly balanced rather than merely working.

**11c. A real campaign wrapper.** All four stages chain today, but only by hand
in Python: play one, read what it hands forward, seed the next. A single
`Campaign` environment that runs the whole arc as one episode would make the
cross-stage credit assignment an actual research question rather than a
plumbing exercise.

## Larger

**12. Chunked infinite world with biomes.** The fixed 1400x620x1400 box is
what gives `memcpy` snapshot, determinism and 80k steps/s, and infinite worlds
want streaming — a real architectural tension. The resolution is procedural
chunks hashed from the world seed with a fixed resident window, so the active
region stays a POD. Biomes (temperature, current, substrate) matter as much as
extent: an infinite uniform ocean is not worth exploring.

**13. Articulated physics and learned locomotion.** Bodies do not swim. Thrust
is applied along a heading and the undulation is a decorative sine disconnected
from the physics. Needs jointed segments with per-segment fluid drag, then a
morphology-conditioned policy (GNN or transformer over the body graph) so one
controller generalises across body plans. Full articulation for the player and
nearest N, cheap kinematics for the rest.

**14. WASM build and browser editor.** Compile sim and renderer to WebAssembly
and put a drag-and-drop editor in the browser. Spore-like UX without adding a
single native dependency to the C core.

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
