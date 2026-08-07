# cpore: the plan

The staged path from what exists today to the game people play — MVP first,
then depth, then the feature this is all secretly for: **creatures made by
different people competing in the same world, and carrying that rivalry up
through tribe and civilisation.**

Design rationale lives in [GAME_DESIGN.md](GAME_DESIGN.md); this file is the
order of battle. Each stage lists what it delivers, where the code goes, and
the test that decides it is done. A stage is not done because its code
merged; it is done when its acceptance line passes.

One standing rule shapes everything below: **no servers, no live
multiplayer.** Spore's own "multiplayer" was asynchronous pollination —
other players' creatures arrived in your world as data, not as connections —
and that is exactly the shape this plan uses. Determinism, the 22 KB
snapshot and free hosting all survive because of it.

---

## Phase 0 — already built

The sim (cell, aquatic, creature, civ), the unbounded procedural planet with
biomes, rivers and day/night, the 19-part genome, evolving rival species,
the SDF renderers, the ctypes binding, 25.5k steps/s on the land stage.
Phase 0 is why every stage below is a bounded amount of work.

---

## Phase 1 — MVP: a stranger can play it

The MVP question: can a person with a link and no manual have a good ten
minutes? Nothing that does not serve that question belongs in this phase.

### 1.1 Walk

Real-time in the browser. `make wasm` (emscripten, single-threaded — no
COOP/COEP header trouble), a shell in `web/` that maps keyboard + mouse onto
the existing land-stage action vector and blits the frame to a canvas at an
integer pixel scale. An interactive quality tier inside `src/render_land.c`
— one quality struct, not a fork — targeting 320×180 at ≥30 fps. Sim ticks
fixed at ~15 steps/s, so an episode is a ten-minute session and a day/night
cycle passes every ~2¼ minutes.

*Code:* `Makefile`, `web/index.html`, `web/main.js`, `src/render_land.c`.
*Done when:* ≥30 fps on a mid-range laptop; you can walk to a river and
watch a live sunset; the 30-seed balance table is byte-identical.

### 1.2 Pay

A reason to walk. The codex: first sighting of a lineage fires an event and
a card — four-view render, biome, diet, a name you can edit. Rarity tiers in
nest seeding so some species only exist at biome × medium × hour
intersections. Discovery count already sits in the observation, so the same
work is an exploration reward for the lab.

*Code:* `src/codex.c` (POD records inside the world struct), seeding in
`src/land.c`, card UI in `web/`.
*Done when:* a cold ten-minute session yields ≥5 codex entries and ≥1 rare;
sightings feel like events, not counter increments.

### 1.3 Build

The creature creator, playable. Parts palette with live DNA prices, drag to
place/lengthen, wheel to fold, mirror toggle, paint tab for the three coats
— all a frontend over the genome that already exists, with the budget still
enforced in C. The test pen: a pocket world (flat ground, pond, bush, one
rival) to try the build for 200 steps. Share codes: the genome as a short
base64 string, so a creature is a thing you paste.

*Code:* editor UI in `web/`, pocket-world flag in `src/land_env.c`,
`src/genome_codec.c`.
*Done when:* each of the six archetypes is hand-buildable in under three
minutes; a share string round-trips through the Python binding.

### 1.4 Ship

itch.io page (browser-playable zip, uploaded via butler from the Makefile)
as the front door; a GitHub Actions workflow publishing every push to
GitHub Pages as the dev build; both linked from the README. Then the real
deliverable of this stage: **five strangers play it and are watched.**

*Code:* `.github/workflows/pages.yml`, a `make itch` target, README.
*Done when:* the game is a public link, and there exists a written list of
what the first five players did, got stuck on, and quit at. Phase 2's order
gets re-sorted against that list.

---

## Phase 2 — depth: the world pushes back

Order within this phase is provisional until 1.4's playtest notes exist.

### 2.1 Traverse

Slope-gated climbing paid from a stamina meter derived from parts, gliding
for wings that cannot sustain lift, sprint on the same meter. A peak you
cannot climb with your current build is the game's clearest promise.

*Code:* movement resolve in `src/land.c`; one stamina float in player state
and observation.
*Done when:* a named peak needs the right build to summit; all six
archetypes still viable on the re-run table.

### 2.2 Weather

Rain that cuts sight the way night does, wind that pushes fliers along the
gust field the grass already reads, a snowline that breathes. Pure functions
of (seed, position, time), like everything else.

*Code:* next to the biome fields in `src/land.c`; read by `render_land.c`.
*Done when:* a storm changes which build is the capable one, measurably.

### 2.3 Senses

The umwelt roster from GAME_DESIGN.md Part VI: smell (scent drops in a POD
ring buffer, sampled with wind offset and decay), ground vibration,
electroreception in water, echolocation as the one sense that spends energy
and stealth to perceive. HUD shows only the senses the build owns.

*Code:* perception resolve in `src/land.c`, scent ring buffer on the world
struct, HUD in `web/`.
*Done when:* a blind build with good ears and nose is genuinely playable at
night, and the table proves it.

### 2.4 Juice

Procedural audio (WebAudio: wind, calls, footsteps by medium), photo mode,
APNG export for sharing clips. The stage that makes people *post* it.

*Code:* `web/audio.js`, APNG in `src/png.c` (a short step from the existing
DEFLATE encoder).
*Done when:* a session recording gets shared unprompted.

---

## Phase 3 — pollination: creatures compete

The wish this phase serves: **creatures created by different people meet in
one world and compete.** Asynchronously, by data, with no server — share
codes are the transport, determinism is the referee.

### 3.1 Invasions

Paste a friend's share code before starting a world and their creature
enters it as a founding rival lineage — nested, breeding, mutating, subject
to the same selection as everything else. Ten minutes later the codex tells
you whether their design is thriving in your biomes or already extinct.
Multiple codes seed multiple rival dynasties.

*Code:* `cp4_env_reset` already accepts genomes; this is plumbing imported
genomes into rival-nest founding in `src/land.c` plus a paste box in `web/`.
*Done when:* two people can trade codes and truthfully argue about whose
creature outlasted whose.

### 3.2 Tournaments

The competitive face, run headless: N submitted genomes, fixed seed set,
every genome founds a lineage in every world, ranked by survival, spread,
kills, codex presence at episode end. One command, one table — the same
30-seed methodology the repo already trusts. Results are a markdown table
anyone can regenerate, so a "leaderboard" is a file in a repo, not a
service. Tournaments double as lab benchmarks: an RL-designed genome enters
by the same share code a human uses.

*Code:* `apps/tourney.c` (headless runner), `python/cpore/tourney.py`
(orchestration + table), results under `docs/tourneys/`.
*Done when:* a tournament of ≥8 community genomes has run and published;
verifying a result by re-running it locally works.

### 3.3 Ecosystem packs

Curated sets of community creatures shipped as seed-world presets — "world
of the week" — so a fresh player's planet is already populated by other
people's imaginations. This is Spore's pollination feeling, delivered as a
JSON file of share codes.

*Code:* `web/packs/`, loaded by the shell at world creation.
*Done when:* a new player meets a stranger's creature in their first
session without pasting anything.

---

## Phase 4 — the long arc: creature → tribe → civilisation

The full Spore fantasy: what you evolve goes on to build. Cell → aquatic →
creature already chain, and creature → civ already hands forward a legacy.
This phase fills the gap Spore filled with its tribal stage, then makes the
whole arc one episode — with imported creatures riding along, so Phase 3's
rivalries escalate from ecology to war.

### 4.1 The tribe stage

A new rule set over the same planet, between creature and civ: your species
settles. A handful of tribe members (the follower/nest machinery grown up),
fire, tools, food stores, domestication of species from *your own codex*,
and neighbouring tribes founded from the world's other successful lineages —
including invaded ones. Charm and violence stay the two currencies, so the
stage-3 fork (impress or eat) matures into diplomacy or raiding. The body
still matters: what your species' parts are decides what its tools extend.

*Code:* `src/tribe.c`, `src/tribe_env.c`, `include/cpore/tribe.h`, a
map-scale renderer reusing `render_civ.c`'s approach; legacy bridge in the
same style as `cp5_legacy_from_world`.
*Done when:* three doctrines (raid, trade, charm) win over a seed table by
genuinely different mechanics, like civ's force/trade/faith already do.

### 4.2 The campaign

One environment that runs the arc as a single episode: cell → aquatic →
creature → tribe → civ, each stage handing its legacy to the next through
the bridges that exist. For the game face this is "campaign mode"; for the
lab it is the long-horizon credit-assignment benchmark the roadmap has
wanted all along.

*Code:* `src/campaign.c` + `campaign_env.c` wrapping the per-stage envs;
`python/cpore/campaign.py`.
*Done when:* a full arc plays start to finish in one sitting, and a
scripted baseline's campaign score exists for others to beat.

### 4.3 Rivalry all the way up

The payoff that ties Phases 3 and 4 together: invaded creatures persist
across the arc. The lineage your friend's share code founded in your
creature stage becomes a rival tribe in 4.1 and a rival nation in civ —
*whose doctrine multipliers come from their body, via the same legacy
bridge yours do.* Your friend designed a horned pack hunter; three stages
later you are besieged by its descendants' war parties, and that is nobody's
scripted content — it is their design meeting your world.

*Code:* carrying rival-lineage genomes through the stage bridges — an
extension of the legacy structs, no new machinery.
*Done when:* a civ-stage war can be traced, in the codex, back to a share
code pasted an hour earlier.

---

## The lab track (parallel, independent)

Runs alongside all phases; nothing above depends on it and it depends on
nothing above. (1) `src/vec.c` — SoA batch stepper, then the pufferlib
binding in `python/cpore/puffer.py`, accepted by a measured steps/s table
and a PPO run that beats the scripted 30-seed table. (2) The text protocol
in `python/cpore/textgym.py` and the LLM harness, accepted by a leaderboard
over fixed seeds: baseline / PPO / ≥2 LLMs / one human. Tournament entry by
share code (3.2) is where the two tracks meet: humans, PPO policies and
LLMs compete in the same table.

---

## What is deliberately deferred

- **Live multiplayer, accounts, any server** — pollination by data gets the
  fantasy at zero cost; a tiny serverless endpoint for code-sharing can be
  reconsidered only if 3.x demand proves it.
- **The space stage** — after the arc through civ is fun, not before.
- **Engine or fidelity pivots** — the pixel-art identity and the WASM
  frame budget are settled decisions (see GAME_DESIGN.md).
- **Generative-model terrain amplification** — Phase 2-adjacent polish, only
  after MVP feedback says the terrain, of all things, is what needs depth.
