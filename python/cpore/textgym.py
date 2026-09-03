"""The LLM benchmark face: same env, text in, verbs out, no C changes.

An LLM cannot drive a 170-float observation at 15 Hz. This wraps the land
stage as a compact situation report plus a small verb DSL with frame-skip,
so an episode is ~200 decisions, not 9000::

    from cpore.textgym import TextLand
    game = TextLand(seed=7)
    print(game.describe())
    report = game.act("move NE")
    report = game.act("sing")

Score = codex entries + DNA goal + survival, reported per seed; transcripts
as JSONL. The leaderboard this enables - scripted baseline vs PPO vs LLMs
vs a human over fixed seeds - is GAME_DESIGN.md M6.
"""

from __future__ import annotations

import json
import time

from .env import LandEnv, MEDIA, LAND_STYLES

# control dims of the land action vector
_TURN, _PITCH, _MOVE, _UP, _BITE, _SING, _DIG, _NEST = range(8)

_DIRS = {
    "N": (0.0, 1.0), "S": (0.0, -1.0), "E": (1.0, 0.0), "W": (-1.0, 0.0),
    "NE": (0.7, 0.7), "NW": (-0.7, 0.7), "SE": (0.7, -0.7), "SW": (-0.7, -0.7),
}

_VERBS = ("move", "bite", "sing", "flee", "dig", "nest", "wait", "redesign",
          "buy", "help")


class TextLand:
    """Land stage as a text game for LLM (and human) benchmarking."""

    def __init__(self, seed: int = 0, genome=None, frame_skip: int = 45):
        self.env = LandEnv(seed=seed, genome=genome)
        self.seed = seed
        self.frame_skip = frame_skip
        self.obs = self.env.reset(seed=seed)
        self.decisions = 0
        self.transcript = []
        self._known_species = set()

    def close(self):
        self.env.close()

    # -- situation report -------------------------------------------
    def describe(self) -> str:
        c = self.env.census()
        med = c["medium"]
        lines = [
            f"seed {self.seed} decision {self.decisions} | "
            f"medium: {med} | dna {c['dna']:.0f}/100 | "
            f"travelled {c['travelled']:.0f} | discovered {c['discovered']}",
            f"ate bush {c['ate']['bush']} kelp {c['ate']['kelp']} "
            f"tuber {c['ate']['tuber']} meat {c['ate']['meat']} | "
            f"songs {c.get('befriended', 0)} kills {c['kills']} | "
            f"nest {'yes' if c['has_nest'] else 'no'} "
            f"store {c['nest_store']:.0f}",
            f"pop {c['pop']} allies {c['allies']} enemies {c['enemies']} | "
            f"steps {c['steps']}",
        ]
        return "\n".join(lines)

    @staticmethod
    def help() -> str:
        return ("verbs: move <dir> [n] | bite | sing | flee | dig | nest | "
                "wait [n] | redesign <style> | buy <part> | help\n"
                "dirs: N S E W NE NW SE SW. n repeats the verb (frame-skip "
                "x n). styles: " + ",".join(LAND_STYLES))

    # -- verb DSL -----------------------------------------------------
    def _base(self):
        return [0.0] * self.env.act_dim

    def act(self, command: str):
        """Run one verb, return the new situation report string."""
        t0 = time.time()
        parts = command.strip().split()
        verb = parts[0].lower() if parts else "wait"
        arg = parts[1].lower() if len(parts) > 1 else ""
        rep = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else 1
        if len(parts) > 1 and parts[1].isdigit() and verb in ("move", "wait"):
            rep = int(parts[1])
            arg = ""
        rep = max(1, min(rep, 12))

        reward = 0.0
        done = False
        note = ""
        for _ in range(rep):
            a = self._base()
            if verb == "move" and arg.upper() in _DIRS:
                dx, dz = _DIRS[arg.upper()]
                a[_TURN] = dx
                a[_MOVE] = 1.0
            elif verb == "bite":
                a[_BITE] = 1.0
                a[_MOVE] = 0.4
            elif verb == "sing":
                a[_SING] = 1.0
            elif verb == "flee":
                a[_TURN] = 0.0
                a[_MOVE] = 1.0
            elif verb == "dig":
                a[_DIG] = 1.0
            elif verb == "nest":
                a[_NEST] = 1.0
            elif verb == "redesign" and arg in LAND_STYLES:
                self.env.redesign(arg)
                note = f"redesigned toward {arg}"
            elif verb == "buy":
                note = "buy is flavour for redesign <style> in this build"
            elif verb == "help":
                return self.help()
            for _ in range(self.frame_skip):
                self.obs, r, te, tr, _ = self.env.step(a)
                reward += r
                if te or tr:
                    done = True
                    break
            if done:
                break
        self.decisions += 1
        c = self.env.census()
        score = c["discovered"] * 10.0 + c["dna"] + self.decisions * 0.1
        report = self.describe() + f"\nreward {reward:+.1f} score {score:.1f}"
        if note:
            report += f"\n{note}"
        if done:
            report += f"\nEPISODE OVER ({c['kills']} kills, {c['discovered']} species)"
        self.transcript.append({"d": self.decisions, "cmd": command,
                                "report": report, "dt": time.time() - t0})
        return report

    def score(self) -> float:
        c = self.env.census()
        return c["discovered"] * 10.0 + c["dna"] + self.decisions * 0.1

    def save_transcript(self, path: str):
        with open(path, "w") as f:
            for row in self.transcript:
                f.write(json.dumps(row) + "\n")
        return path


def leaderboard(seeds=(7, 21, 42), max_decisions=200):
    """Scripted-baseline leaderboard over fixed seeds. PPO/LLM/human rows
    plug in by replacing the policy loop; the seeds and scoring stay fixed."""
    rows = []
    for seed in seeds:
        g = TextLand(seed=seed)
        for _ in range(max_decisions):
            c = g.env.census()
            cmd = "sing" if c["allies"] < c["enemies"] else "move NE"
            rep = g.act(cmd)
            if "EPISODE OVER" in rep:
                break
        rows.append({"seed": seed, "score": round(g.score(), 1),
                     "census": g.env.census(), "policy": "scripted"})
        g.close()
    return rows


if __name__ == "__main__":  # pragma: no cover
    g = TextLand(seed=7)
    print(g.describe())
    print(g.act("move NE 2"))
    print(g.act("sing"))
    print("score:", round(g.score(), 1))
    g.close()
