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

## Next

- ~~**A renderer that is not pixel art.**~~ Two of them. Stage 1 has `drop`, a
  darkfield microscope plate; stage 3 has `vista`, a landscape where the
  atmosphere does the drawing. Both are linear HDR at the output resolution
  with analytic antialiasing, depth of field, four octaves of bloom, a filmic
  tonemap and a lens pass, and neither has a palette, a dither or an upscale
  anywhere in it. They share a canvas and a film chain (`hdrcanvas.h`) and
  nothing else, because a bag of cytoplasm and a hillside disagree about
  everything between those two ends.

  `vista` also needed the terrain marcher rebuilt: the pixel renderer spent
  95% of its frame in `cp4_height`, sampling a two-dimensional function with
  a three-dimensional ray march at a hundred samples per pixel. Caching the
  field into a grid around the camera and marching that took it from eight
  seconds to a quarter of one, which is what paid for everything above.

  Stages 2 and 4 are still pixel-only. Stage 2 is the natural next target
  since it is already lit water, and much of `vista`'s shading model - banded
  response, hue-shifted ambient, transmission - transfers to it directly.

**8. Slim the HUD.** Done wherever there is a continuous-tone renderer to do
it in: hairlines and dot matrix, nothing filled, everything pushed to the
frame edge, and the whole layout in units of the frame so it holds its
proportion at any output size. Stage 1 puts the vitals in an arc around the
player; stage 3 keeps them as thin rules in the corner and adds a dial for the
hour. The two pixel-path HUDs still have their filled panels, and will keep
them - the readout that suits a 640x360 palette frame is not the one that
suits a 1280x720 continuous one, which is most of why these are separate
renderers rather than one with a flag.

**8b. The creature editor.** Everything below the front end is done.

`Cp4Studio` renders one animal through the world's own shading path, holds its
buffers across frames, and scales resolution and light transport together so a
caller draws at 60 fps while dragging and settles to a supersampled frame when
it stops. `cp4_studio_pick` turns a pixel into the genome slot that drew it and
`cp4_studio_surface` turns one into a place on the body; both run the same
march as the renderer, off the same `land_spine`, so none of the three can
disagree about where the animal is. On top of those: place, move, remove,
shape, mirror, the spine handles, and paint over the three coats. `Cp4Edit`
wraps the lot in a handle-and-flat-array ABI that ctypes and WebAssembly can
both call, and `CreatureEditor` is the Python side of it.

The front end exists too: `make wasm && make serve`. Every shape is made by
dragging, as in the editor it imitates - a part comes off the palette under
the cursor and follows it until you let go, a placed part is dragged to move,
its ring dragged to resize and its tip dragged to lengthen, a vertebra dragged
up to hump and sideways to fatten. No click-then-click mode anywhere.
`?selftest=1` drives the whole palette drag through the real pointer handlers,
so the wiring is checked rather than assumed.

And it is responsive now, which it was not. A press, twelve moves and a
release blocked the main thread for 1973ms; it is 123ms. Most of that was not
the marcher: pressing on a part ran a full settle frame inside the pointerdown
handler and then ten thousand ray-marched picks to find its drag handles, for
an event that changes no geometry. Handles are projected from the primitives
now (`cp4_studio_extent`, four microseconds), the ladder climbs one rung at a
time on a timer instead of jumping, and drag frames are coalesced onto the
animation frame. The renderer itself got about three times faster as well: the
contact shadow is a world-space grid rather than a per-screen-pixel trace, so
it stopped costing the square of the resolution, and the creature self-shadow
waits for the export instead of switching on at the same moment the resolution
quadruples. The self-test asserts the drag budget so it cannot quietly return.

Still open on the RL side: `cp4_genome_from_action` sets parts, nseg and girth
and nothing else, so colour, pattern and the spine genes remain unreachable
from the action space and every animal an agent designs is the same beige with
plain coats and a straight back. The editor can now set all of them, so the
work is widening the action head rather than plumbing - but it changes
CP4_ACT_DIM, which is an RL interface break and wants its own decision.

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

**12. Weather, and stage 2's turn.** Stage 3 now varies in space (eight
biomes) and in time (a day/night cycle that takes sight and leaves hearing).
What is still missing is weather — rain that cuts visibility, wind that pushes
a flier, seasons that move the biome boundaries over an episode. Stage 2 is
also still a fixed box and should inherit both the resident-window treatment
and a substrate/current equivalent of biomes.

**13. Articulated physics and learned locomotion.** Half done for stage 1.

The beat now drives the body: thrust is modulated by the same phase the
renderer draws the cilia and flagellum from, so what you see is the cause of
what you feel rather than a picture of it. A stroke has a duty cycle, and that
is where the three propulsors genuinely differ - cilia are many hairs out of
step, so the sum is nearly continuous; a jet is a hard pulse and a refill.
Holding one direction, measured speed ripple: cilia 0%, flagella 7.6%, jet
33.8%. The gain integrates to one over a cycle whatever the duty, so this
changed the texture of movement and not the balance, and nothing downstream
needed retuning.

What is still missing is the articulation itself. There are no jointed
segments and no per-segment fluid drag - the body is one disc with a scalar
thrust that now pulses. Stage 3's walk cycle is still decorative. Beyond that,
a morphology-conditioned policy (GNN or transformer over the body graph) so one
controller generalises across body plans, with full articulation for the player
and nearest N and cheap kinematics for the rest. Needs jointed segments with per-segment fluid drag, then a
morphology-conditioned policy (GNN or transformer over the body graph) so one
controller generalises across body plans. Full articulation for the player and
nearest N, cheap kinematics for the rest.

- ~~**14. WASM build and browser editor.**~~ Done for the creature editor, and
  without Emscripten: clang's wasm32 target plus wasm-ld, with a two-hundred
  line shim supplying the allocator and the memory functions, and the
  browser's own Math imported for the transcendentals. 57KB, six imports, no
  runtime. What is not compiled yet is the simulation itself - Test Drive in
  a browser wants cp4_world_step and the vista renderer across the same
  boundary, which is more surface but no new problem.

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
