# cpore, the game: design direction

This document is the answer to one question: **what exactly do we build so
that cpore is genuinely fun to play**, while staying the RL environment it
already is. It names the loops, the risks, and the file every new piece of
code should land in. It deliberately contains no code.

The ambition, stated plainly: Breath of the Wild's *pull* — geography that
promises something over every ridge — feeding Spore's *pleasure* — a body you
designed, doing things because of how you designed it. Plus a third face
nobody else has: the same world as a pufferlib environment and an LLM
benchmark.

## The thesis: one loop, three faces

The fun loop, in one breath:

> You see something strange on the horizon. Crossing the country to reach it
> costs you, because terrain resists your current body. The strange thing is
> a **species that has never existed before** — it goes in your codex. What
> you learned from it (it digs, it glows, it sings back) is a trick you can
> steal in the editor. The new body opens terrain the old one couldn't
> touch, and the new terrain has a new horizon.

Exploration pays in *knowledge of species*. The editor converts knowledge
into *capability*. Capability opens *more world*. Each half feeds the other,
which is the property neither BOTW nor Spore actually has — BOTW's shrines
don't change your body, Spore's world isn't worth crossing.

Everything ships as **one C core with three faces**:

| face | what it is | who it's for |
|---|---|---|
| the game | real-time, in the browser, WASD and a mouse | anyone with a link |
| the editor | the creature creator as a standalone toy | the Spore half of the audience |
| the lab | pufferlib env + text protocol, no pixels | RL training and LLM benchmarking |

One rule keeps all three honest, and it is already the repo's law: **the sim
never knows about the renderer or the UI**. The human player is just another
policy writing the same action vector `cp4_env_step` already accepts. That
buys something for free that neither Spore nor BOTW could ever offer: every
minute of human play is a recorded (obs, action) trajectory — demonstration
data for the lab, at zero extra cost.

## What exists vs. what's missing

Worth stating, because the gap is smaller than the wishlist implies. Already
built and measured: a procedural planet with folded ranges, rivers that reach
the sea, eight biomes, day/night, an unbounded streamed world; a 19-part
genome with jointed limbs, three coats and bilateral symmetry; rival species
that breed, are selected, and remember what you did to them; four stages that
chain; 25.5k steps/s on the land stage; a renderer that already does golden
hour, valley mist, wind and cloud shadows.

What is actually missing, in order of importance:

1. **Interactivity.** Nothing anywhere responds to a keypress. Every frame
   ever rendered was a PNG written after the fact.
2. **A reason to walk.** The world varies, but discovery is a census counter,
   not an event. Nothing celebrates, records, or rewards the first sighting.
3. **Direct manipulation.** The genome is editor-*shaped* (length, fold,
   mirror, coats) but the only editors are `--style` presets and the RL
   design head.
4. **A pufferlib-native path and a text face.** The ctypes binding works but
   vectorises in Python, and no LLM can currently be handed the world.

Those four gaps are Parts I–IV below. Nothing else in the wishlist requires
new architecture.

## Part I — Making it playable

### The player is a policy

No new sim entry points. An interactive frontend reads input, maps it onto
the existing 58-float land-stage action vector, calls `cp4_env_step`, renders
the world, repeats. Camera yaw + WASD become the steering dims; bite, sing
and build-nest get keys; the design head stays untouched during play and is
driven by the editor screen at generation boundaries — exactly the moment
Spore hands you the editor.

Pacing falls out of numbers we already have: run the sim at ~15 steps/s and a
9000-step episode is a ten-minute session, with a full day/night cycle every
~2¼ minutes. Render at whatever the machine can do; sim ticks stay fixed-rate
so determinism and the test suite are untouched.

### Platform: native first, browser on the side

The repo's selling point is libc+libm and nothing else, and training happens
natively — so the game is native too: `apps/cpore_game.c` over `src/glview.c`
(X11+GLX from stock headers, no engine, no SDL). The sim core never links
windowing or GL; the shell maps input onto the action vectors the RL loop
already drives. The emscripten target still builds for link-sharing, but it
is not the path anything depends on.

Where the code goes:

| piece | location |
|---|---|
| native game shell: input → action vector, HUD, map view | `apps/cpore_game.c` |
| GPU present path: GLX context, integer-scale blit, FPS | `src/glview.c` (never in libcpore) |
| interactive render tiers | map view in the shell now; shader-quantise + GPU terrain next, behind the `cp_vis_palette` / `cp4_pose_prims` contract |
| legacy browser shell | `web/` (`make wasm`) |

### The render budget, honestly

This is the project's one real technical risk, so it gets numbers. `terra` is
~8 s/frame at 640×360; interactive needs 33 ms. The path to ~240× is a stack
of knobs, each applied to the same renderer behind one quality struct:

- 320×180 internal resolution (4×) — the pixel-art aesthetic already forgives
  this; it's what `abyss` ships at.
- Terrain AO from 16 height samples to 4, or hash-cached per ground cell
  (~2–3× — AO and the marcher dominate the frame).
- Shorter view distance with a coarser far-field march, and the sky/mist/
  aerial-perspective stack at quarter res (~2–4×).
- Crepuscular rays, bloom and water reflection march off in the interactive
  tier (~1.5×).
- Temporal reuse: the terrain is static per seed and the camera moves slowly,
  so a reprojected terrain buffer refreshed a few columns per frame is the
  big lever if the knobs above fall short.

If the stack only reaches 100 ms, the fallback is 240×135 upscaled — still
respectable for this art style. The screenshot tier stays exactly as it is;
beauty is not being traded away, it's being tiered. Acceptance is a measured
number, in this repo's tradition: an FPS counter in the shell, ≥30 on a
mid-range laptop, and the 30-seed table byte-identical afterward because none
of this touches the sim.

## Part II — Making walking pay (the BOTW half)

What BOTW actually does, translated into mechanisms this codebase can carry:

**1. Discovery is the shrine.** The one thing that most needs building. A
**codex**: first sighting of a lineage fires an event — a card with the
four-view render, the biome and medium it was found in, its diet and
disposition, and a name. The player can rename it; the discovery counter
becomes a collection. Where: `src/codex.c`, a pure function of genomes
encountered, its records POD inside the world struct so snapshots stay a
`memcpy`; the card UI lives in `web/`. Crucially the codex count is already
in the observation (`discovered`), so the same mechanism is an exploration
reward for the lab.

**2. Rare species by construction.** "Strange and cool" must be guaranteed,
not hoped for. Nest seeding in `land.c` gains rarity tiers: some lineages
only site in a biome × medium × time-of-day intersection — a night-glowing
walker on the snowline, an eel in underground rivers, a giant that only
haunts one ruggedness province. Rarity is what converts a codex from a list
into a pursuit. Where: the resident-window seeding in `src/land.c`; no new
state.

**3. Traversal is a puzzle.** Media already gate swim/fly/dig; the ground
itself should resist too. Slope-gated climbing paid from a **stamina meter**
(derived from legs/claws/tail, the parts already there), gliding for wings
that can't sustain lift, sprint on the same meter. A peak you cannot climb
with your current build is the purest BOTW sentence this game can utter.
Where: the movement resolve in `src/land.c`; stamina is one float in the
player state and one slot in the observation.

**4. No markers — senses instead.** The world must never grow a quest arrow.
Distant species call (an ear/voice readout pointing off-screen), tracks hash
out of the ground cell like flora does, and the overhead `--map` becomes an
in-game map revealed by where you have actually walked (a coarse visited-cell
trail, ring-buffered, POD). Where: `src/land.c` for hearing and tracks,
`web/` for the map screen reusing the existing map render path.

**5. Weather and seasons** (roadmap 12, unchanged priority). Rain that cuts
sight the way night does, wind that pushes fliers along the gust field the
grass already reads, a snowline that breathes over an episode. All pure
functions of (seed, position, time) like the biome fields, so free-streaming
and deterministic. Where: next to temperature/moisture in `src/land.c`; read
by `src/render_land.c`.

**6. Landmarks with a promise.** Spires and slabs exist; the missing half is
*reward on arrival* — a landmark should be where rare nests site, where a
vantage reveals map cells in a radius, where ruins (stage-4 cities from a
past civilisation on the same seed — the planet already supports this) stand.
Where: landmark placement graduates from render-only hash to a shared
function both `land.c` and `render_land.c` call, so the sim finally knows
where the renderer put the monuments.

## Part III — The creator (the Spore half)

The genome already carries everything an editor needs: per-part type,
segment, yaw, pitch, scale, mirror, length and fold, spine profile, three
coats. What's missing is hands.

**The editor is a frontend over the genome, not a new system.** A screen in
`web/`: parts palette on the left with live DNA prices, the creature centre
via the existing SDF four-view path (creature-only rendering skips the
terrain marcher entirely, so per-edit refresh is cheap), stat block right —
the one `--creature` already prints, which is the editor's honesty: every
number shown is a number the sim reads. Drag a part onto a segment, drag
outward to lengthen, wheel to fold, one toggle for mirror, a paint tab for
the three coats. The DNA budget stays enforced in C — the UI physically
cannot sell a build the budget didn't buy, because `cp4_env_reset` already
trims overspend.

**The test pen.** Spore's most-loved feature was trying the body immediately.
A pocket world — flat ground, a pond, a bush, one rival — spun up for 200
steps with the candidate build, stat deltas printed. Where: an init flag in
`src/land_env.c` that builds the pocket world instead of a planet; it reuses
the whole sim unchanged.

**Share codes.** A genome is a small POD; encode it as a short base64 string
so a creature is a thing you paste to a friend, no server. Where:
`src/genome_codec.c`, exposed through the header and the web shell alike.
This is also how a human-designed body enters the lab: the same string is a
valid `genome=` argument in Python.

**Editor-first session flow.** The game opens in the editor with the gen-0
budget — creativity before walking, like Spore — and re-enters it at each
generation boundary, which is exactly when the sim already samples the design
head. One moment, two masters: the human gets the editor screen, the RL agent
gets the design head, same boundary, same budget.

## Part IV — The lab (pufferlib + an LLM benchmark)

**Puffer-native vectorisation.** Done: `src/vec.c` batches N worlds behind
one call writing flat obs/rew/done buffers, and `python/cpore/puffer.py`
implements native `PufferEnv`s with numpy buffers C writes into directly —
the old ctypes loop stays as the slow path. Thread sharding (one batch per
core) comes second; the API is already shaped for it. Acceptance the repo's
way: a steps/s table before and after (in the README), and next a PPO run
that beats the scripted baseline's 30-seed table — the experiment the whole
project was built to ask.

**Exploration reward as a first-class option.** The game's currency
(discoveries, distance, codex entries) becomes a selectable reward mix at
reset — the sim already counts all three. That makes "does curiosity help" a
flag, not a fork.

**The LLM benchmark.** Same env, text face, no C changes:
`python/cpore/textgym.py` renders the observation as a compact situation
report (position, vitals, medium, the N nearest entities with species names
from the codex, active memories like wariness) and accepts a small verb DSL —
`move NE`, `bite`, `sing`, `flee`, `buy leg seg=2 len=200 mirror` — with
frame-skip so an episode is ~200 decisions, not 9000. Score = codex entries +
DNA goal + survival, reported per seed; transcripts as JSONL. Deliverable: a
harness script and a leaderboard table — scripted baseline vs. PPO vs. each
LLM vs. a human over the same seeds. The census accessors mean the report
never picks structs apart from Python.

**The campaign** (roadmap 11c) stays the long-horizon flagship eval: cell →
creature → civ as one episode, legacy handed forward by the bridges that
already exist.

## Part V — Generative models in aid of procedural generation

The world stays procedural: deterministic, seed-stable, storing nothing —
that is what makes the unbounded world, the 22 KB snapshot and the RL
throughput possible, and nothing may compromise it. But a generative model
can *aid* that generation without ever entering the loop, and the test for
every proposed use is one question: **does the runtime remain a pure
function of (seed, position, time)?**

Uses that pass the test, in order of payoff:

- **A distilled neural residual on the heightfield.** The subtle thing fBm
  cannot fake is erosion — the way real valleys widen downstream and ridges
  sharpen. Train a model offline on real elevation data (or on expensively
  eroded terrain) to predict a detail residual from the coarse procedural
  height and its slope, then **distill it into a tiny MLP**. A small MLP is
  itself a pure deterministic function of its inputs — it can sit inside
  `cp4_height()` as one more octave, seed-conditioned, budgeted in
  multiply-adds like everything else there. This is the honest version of
  "generative AI makes the terrain beautiful": the model runs at training
  time, the game ships a function. (The literature calls the un-distilled
  version *terrain amplification* — Guérin et al.'s cGAN terrain authoring
  is the entry point.)
- **Fitting the procedural parameters against reality.** Cheaper than the
  above and worth doing first: use real DEM statistics (slope distributions,
  drainage density, ridge spacing per province type) as a target and let an
  optimiser — or a model — tune the constants already in `cp4_height`. The
  code doesn't change shape at all; the numbers stop being guesses.
- **Offline asset generation.** Palettes, sky gradients, landmark
  silhouettes, tree-shape parameter sets: generated during development,
  committed as the same kinds of constants the renderer already carries.
- **Naming and lore at discovery time.** When a codex card is created, an
  optional Python/web plugin asks an LLM for a species name and two lines of
  field notes from the body plan and biome. Cached per lineage, falls back to
  a procedural name generator offline. The C core never knows.

What still fails the test and stays out: any model invoked per-frame or
per-chunk at runtime, anything that makes two visits to the same coordinates
disagree, anything that needs the world stored.

## Part VI — Senses, and the umwelt principle

The deepest idea available to this game costs almost nothing given the
architecture: **every creature lives in the world its senses can afford, and
no two builds live in the same world.** (The biology word is *umwelt*.) The
sim already honours this — eyes buy sight that night takes away, ears buy
hearing that it doesn't, and unperceived entities are zeroed in the
observation rather than merely hidden by the UI. The direction is to widen
the roster until choosing senses is as expressive as choosing limbs:

| sense | part | it buys | it's taken away by |
|---|---|---|---|
| sight | eye | range, detail | night, rain, underground |
| hearing | ear | direction of callers, unaffected by dark | wind, distance |
| smell | nostril | *time* — a scent trail is the past of whoever left it, drifting downwind | rain washing it out, being upwind |
| vibration | whisker/pad | anything moving on or under the ground, 360°, no light needed | flying (no contact), water |
| electroreception | lateral organ | live bodies in water, even buried, even in the dark | air — it simply doesn't exist there |
| echolocation | voice + ear | active sight in total darkness | its own cost: it announces you to everything with ears |

Each row follows the standing law: a sense is a part, it buys information,
and some medium, weather or time of day takes it away — so senses fork the
build the same way media do, and a cave, a night, a storm each reshuffle
which body is the capable one. Echolocation is the designed jewel: the only
sense that *spends* something (energy, and stealth) to perceive, which makes
it a decision every time rather than a passive stat.

Where the code goes: scent is the one genuinely new mechanism — a field that
must remember who passed — and the trick is to keep it POD: a small ring
buffer of (position, species, timestamp) scent drops on the world struct,
sampled with wind offset and exponential decay, exactly parallel to how
species memory already works. Vibration and electroreception are new terms
in the existing perception resolve in `src/land.c`; echolocation is an
action bit plus a broadcast event, shaped like the song mechanic that
already exists. The observation grows one block per sense, and the HUD in
`web/` shows *only the senses the build owns* — the umwelt made literal: a
different creature's screen is a different world.

For the lab this roster is a gift: it makes partial observability a design
axis you can buy, which is a cleaner curiosity/exploration research setting
than bolting noise onto an observation vector.

## Milestones, in order, each with an acceptance test

The order optimises for *the game becoming fun as early as possible*, because
the lab is already proven at 25k steps/s and the render budget is the risk
that most wants early contact with reality.

| # | name | delivers | accept when |
|---|---|---|---|
| M1 | **Walk** | native game shell, GPU present, input → action vector | DONE: `cpore_game` at 60 presents/s, map tier for land/tribe/civ, photo stills via the beauty renderer; 30-seed table byte-identical |
| M2 | **Pay** | codex + rarity tiers + first-sighting card + senses | codex DONE (game banner + lab reward use it); rarity/senses open |
| M3 | **Build** | browser editor + test pen + share codes | share codes + live-redesign API DONE (C + Python round-trip; N key in game); drag-and-drop canvas + test pen open |
| M4 | **Traverse** | climbing/stamina/glide + weather | open |
| M5 | **Scale** | `src/vec.c` SoA + puffer binding | vec + puffer DONE with measured table; PPO-beats-baseline run open |
| M6 | **Judge** | text protocol + LLM harness | text face + transcript + seeds DONE; leaderboard rows beyond scripted open |
| M7 | **Live** | procedural audio (WebAudio), photo mode, APNG export | a session recording someone would post unprompted |

Dependencies are honest: M2–M4 all ride on M1's shell; M5–M6 need nothing
from M1 at all and can proceed in parallel if there are two streams of work.

## Deliberately not doing (additions to the standing list)

- **Multiplayer.** Share codes move creatures between worlds; nothing moves
  players into one world. Determinism and the snapshot story die first.
- **Generative AI in the world pipeline.** Above.
- **A quest system, NPCs with dialogue, a story.** The species *are* the
  content; the codex *is* the quest log. The moment the world needs authored
  content it stops being infinite.
- **Engine adoption (Unity/Godot/Bevy).** The entire lab face depends on the
  sim staying a dependency-free C library; the game face is a thin shell over
  it, and must stay thin.
- **Five-stage parity before fun.** Space (roadmap 11b) still waits. A
  brilliant creature stage beats five adequate stages.

## A reading list

The books behind the ideas above, grouped by the part they serve.

**Procedural worlds (Parts II, V):**
- *Texturing & Modeling: A Procedural Approach* — Ebert, Musgrave, Peachey,
  Perlin, Worley. The canon. Musgrave's fractal-terrain chapters are the
  lineage `cp4_height` descends from; Perlin explains noise from the source.
- *Procedural Content Generation in Games* — Shaker, Togelius, Nelson. Free
  online (pcgbook.com). The academic survey, including the ML-assisted PCG
  that Part V leans on.
- *Procedural Generation in Game Design* — ed. Short & Adams. The design
  side: when generated content is fun and when it's wallpaper, by the people
  who made Dwarf Fortress and Caves of Qud.
- *The Computational Beauty of Nature* — Flake. Fractals, chaos, adaptation
  and selection in one book; the closest thing to this project's worldview
  in print.

**Making it look good (Parts I, V):**
- *Real-Time Rendering* — Akenine-Möller, Haines, Hoffman. The frame-budget
  bible; everything M1's quality tier needs a name for is in here.
- *Physically Based Rendering* — Pharr, Jakob, Humphreys. Free online
  (pbr-book.org). Overkill on purpose: it's where the aerial perspective,
  scattering and tonemapping already in `render_land.c` come from.
- Inigo Quilez's articles (iquilezles.org) — not a book, but the SDF
  raymarching, smooth-minimum and fBm-derivative techniques this renderer is
  built on are documented nowhere better.
- *The Book of Shaders* — Gonzalez Vivo & Lowe. Free online. The gentlest
  on-ramp to thinking in fields and noise.

**Generative models (Part V):**
- *Understanding Deep Learning* — Prince. Free online (udlbook). Modern and
  rigorous, covers diffusion; the right foundation text today.
- *Generative Deep Learning*, 2nd ed. — Foster. The practical one: VAEs,
  GANs, diffusion, world models, with code. Enough to build the terrain
  residual and the distillation in Part V.
- Guérin et al., *Interactive Example-Based Terrain Authoring with
  Conditional Generative Adversarial Networks* (2017) — the paper Part V's
  amplification idea descends from.

**Senses and creatures (Part VI):**
- *An Immense World* — Yong. Animal senses and the umwelt, one chapter per
  modality. Not a technical book; it is the design document nature already
  wrote for Part VI, and the single best source of "strange and cool" here.
- *Vehicles: Experiments in Synthetic Psychology* — Braitenberg. Fifty
  pages: how sensors wired to motors produce fear, aggression and love.
  The spiritual ancestor of every scripted policy in this repo.
- *Sensory Ecology, Behaviour, and Evolution* — Stevens. The academic
  backing for the senses table: what each modality costs, what it detects,
  what defeats it.
- *AI for Games* — Millington. The perception-simulation chapters (sight
  cones, sound propagation, knowledge models) are the standard treatment of
  what `land.c`'s perception resolve does.
- *The Nature of Code* — Shiffman. Free online. Steering, flocking, and
  evolutionary systems; the friendliest path into agent behaviour.

**The lab (Part IV):**
- *Reinforcement Learning: An Introduction* — Sutton & Barto. Free online.
  Non-negotiable foundation for anyone training against this environment.

## Is this a lot to ask?

As a wishlist, yes. Against this codebase, no: the three expensive miracles —
a world worth looking at, bodies worth designing, a sim fast and pure enough
to train against — are already built and measured. What remains is four
subsystems (a shell, a codex, an editor UI, a vec layer), each of which has a
named home above and none of which touches the others' correctness. The
riskiest single item is the interactive frame budget, which is why it is M1.
