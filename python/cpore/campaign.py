"""The campaign: cell -> aqua -> creature -> tribe -> civ -> space as one
episode.

Spore's fantasy is the arc, not any single stage. Each stage hands its
legacy forward through the bridges that already exist in C:

- cell EVOLVED -> aqua starts (same seed, same planet logic)
- aqua EVOLVED -> creature starts
- creature -> tribe founds from the survivor's live genome
- creature -> civ inherits military/economic/religious multipliers
- tribe -> civ: an allied continent is a richer inheritance
- civ -> space: the nation's doctrine becomes the empire's strengths

Gymnasium-shaped (reset/step) with a dict observation ``{"stage": name,
"obs": [...], "t": total_steps}`` so the stage's own dims are preserved -
padding every stage to one width would lie to the policy about what it is
driving. A fixed-shape padded variant for PPO is tracked work, not done
work. Today this is the eval/play path: ``run_baseline()`` plays the whole
arc with the scripted policies and returns the report card.
"""

from __future__ import annotations

from .env import CporeEnv, AquaEnv, LandEnv, TribeEnv, CivEnv, SpaceEnv

STAGES = ("cell", "aqua", "land", "tribe", "civ", "space")


class Campaign:
    def __init__(self, seed: int = 0):
        self.seed = seed
        self.cell = CporeEnv(seed=seed)
        self.aqua = AquaEnv(seed=seed)
        self.land = LandEnv(seed=seed)
        self.tribe = TribeEnv(seed=seed)
        self.civ = CivEnv(seed=seed)
        self.space = SpaceEnv(seed=seed)
        self.stage = "cell"
        self.t = 0
        self.report = {}

    def close(self):
        for e in (self.cell, self.aqua, self.land, self.tribe, self.civ,
                  self.space):
            e.close()

    # -- chaining ------------------------------------------------------
    def reset(self, seed=None):
        if seed is None:
            seed = self.seed
        self.seed = seed
        self.t = 0
        self.report = {}
        self.stage = "cell"
        return {"stage": "cell", "obs": self.cell.reset(seed=seed), "t": 0}

    def _advance(self):
        if self.stage == "cell":
            self.stage = "aqua"
            return {"stage": "aqua", "obs": self.aqua.reset(seed=self.seed)}
        if self.stage == "aqua":
            self.stage = "land"
            return {"stage": "land", "obs": self.land.reset(seed=self.seed)}
        if self.stage == "land":
            code = self.land.share_code()
            self.report["land_code"] = code
            self.report["land_legacy"] = self.land.legacy()
            self.report["land_census"] = self.land.census()
            # settle the survivor: its share code becomes the tribe founder
            from .env import land_genome_from_code
            founder = land_genome_from_code(code)
            self.stage = "tribe"
            return {"stage": "tribe",
                    "obs": self.tribe.reset(seed=self.seed, genome=founder),
                    "founder": code}
        if self.stage == "tribe":
            self.report["tribe_census"] = self.tribe.census()
            self.stage = "civ"
            return {"stage": "civ",
                    "obs": self.civ.reset(seed=self.seed,
                                          legacy=self.report.get("land_legacy")),
                    "legacy": self.report.get("land_legacy")}
        if self.stage == "civ":
            self.report["civ_census"] = self.civ.census()
            # the nation's own multipliers become the empire's strengths
            bonus = self.civ.census()["bonus"]
            legacy = [bonus["religious"], bonus["economic"], bonus["military"]]
            self.report["space_legacy"] = bonus
            self.stage = "space"
            return {"stage": "space",
                    "obs": self.space.reset(seed=self.seed, legacy=legacy),
                    "legacy": legacy}
        self.stage = "done"
        return {"stage": "done", "obs": []}

    def step(self, action):
        self.t += 1
        if self.stage == "cell":
            o, r, te, tr, i = self.cell.step(action)
            if te or tr:
                if self.cell.status == "evolved":
                    nxt = self._advance()
                    return nxt, r, False, False, {"advanced": True, **i}
                return {"stage": "done", "obs": o}, r, True, False, \
                    {"died": self.cell.status, **i}
            return {"stage": "cell", "obs": o, "t": self.t}, r, te, tr, i
        if self.stage == "aqua":
            o, r, te, tr, i = self.aqua.step(action)
            if te or tr:
                if self.aqua.status == "evolved":
                    nxt = self._advance()
                    return nxt, r, False, False, {"advanced": True}
                return {"stage": "done", "obs": o}, r, True, False, \
                    {"died": self.aqua.status}
            return {"stage": "aqua", "obs": o, "t": self.t}, r, te, tr, i
        if self.stage == "land":
            o, r, te, tr, i = self.land.step(action)
            if te or tr:
                if self.land.status == "evolved":
                    nxt = self._advance()
                    return nxt, r, False, False, {"advanced": True}
                return {"stage": "done", "obs": o}, r, True, False, \
                    {"died": self.land.status}
            return {"stage": "land", "obs": o, "t": self.t}, r, te, tr, i
        if self.stage == "tribe":
            o, r, te, tr, i = self.tribe.step(action)
            if te or tr:
                self.report["tribe_census"] = self.tribe.census()
                if self.tribe.status == "won":
                    nxt = self._advance()
                    return nxt, r, False, False, {"advanced": True}
                return {"stage": "done", "obs": o}, r, True, False, \
                    {"died": self.tribe.status}
            return {"stage": "tribe", "obs": o, "t": self.t}, r, te, tr, i
        if self.stage == "civ":
            o, r, te, tr, i = self.civ.step(action)
            if te or tr:
                self.report["civ_census"] = self.civ.census()
                if self.civ.status == "won":
                    nxt = self._advance()
                    return nxt, r, False, False, {"advanced": True}
                return {"stage": "done", "obs": o}, r, True, False, \
                    {"died": self.civ.status}
            return {"stage": "civ", "obs": o, "t": self.t}, r, False, tr, i
        if self.stage == "space":
            o, r, te, tr, i = self.space.step(action)
            if te or tr:
                self.report["space_census"] = self.space.census()
                return {"stage": "done", "obs": o}, r, True, False, {}
            return {"stage": "space", "obs": o, "t": self.t}, r, False, tr, i
        return {"stage": "done", "obs": []}, 0.0, True, False, {}

    # -- scripted full-arc baseline -------------------------------------
    def run_baseline(self, seed=None, verbose=False):
        """Play the arc with the scripted policies. Returns the report card."""
        out = self.reset(seed=seed)
        total = 0.0
        steps = 0
        while out["stage"] != "done" and steps < 40000:
            st = out["stage"]
            env = getattr(self, st)
            act = env.greedy_action()
            out, r, te, tr, _ = self.step(act)
            total += r
            steps += 1
            if te or tr:
                break
        self.report["total_reward"] = total
        self.report["total_steps"] = steps
        self.report["final_stage"] = self.stage
        if verbose:
            for k, v in self.report.items():
                print(f"  {k}: {v}")
        return dict(self.report)


if __name__ == "__main__":  # pragma: no cover
    c = Campaign(seed=7)
    rep = c.run_baseline(verbose=True)
    print("arc finished:", rep.get("final_stage"), "reward", round(rep.get("total_reward", 0), 1))
    c.close()
