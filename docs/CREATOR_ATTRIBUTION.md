# Attribution — Creature Creator algorithms

The procedural body mesh in `src/bodymesh.c` ports algorithms from
**Daniel Lochner's Creature Creator** (MIT License, Copyright 2020 Daniel Lochner):

- Skinned cylinder + hemisphere topology (`CreatureController.Setup`)
- Cosine blend-shape inflate along bones
- Neighbor-bleed weight add/remove
- Build / Paint / Test mode loop (studio UX)

Original: https://github.com/daniellochner/SPORE-Creature-Creator
MIT fork used for reference: https://github.com/Dev-Sona/Creature-Creator

Not affiliated with EA/Maxis Spore.
