"""ctypes binding for the cpore cell-stage environment.

The C side owns all state; this module is a thin marshalling layer. numpy and
gymnasium are optional - the raw API works with plain Python lists so the
environment can be smoke-tested anywhere.

    from cpore import CporeEnv
    env = CporeEnv()
    obs = env.reset(seed=23, morph=(2, 1, 2, 3, 0, 0))
    obs, reward, terminated, truncated, info = env.step([1.0, 0.0, 0.0])
"""

from __future__ import annotations

import ctypes
import os
from ctypes import (POINTER, c_float, c_int, c_int32, c_uint8, c_uint32,
                    c_void_p, c_size_t, c_char_p)

try:
    import numpy as _np
except ImportError:                                    # pragma: no cover
    _np = None

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_LIB = os.path.normpath(os.path.join(_HERE, "..", "..", "build", "libcpore.so"))

# index == the CP_PART_* enum value; slot 0 is "empty"
PART_NAMES = ("none", "filter", "jaw", "proboscis", "cilia", "flagella",
              "jet", "spike", "electric", "poison", "eye")
PART = {n: i for i, n in enumerate(PART_NAMES)}
PART_COST = (0, 5, 10, 16, 5, 10, 16, 9, 20, 14, 6)
GEN_BUDGET = (30, 55, 85, 125)
STATUS = ("running", "dead", "evolved", "timeout")

# render styles; index == the CP_VIS_* enum value
VIS_STYLES = ("abyss", "dmg", "neon", "petri", "c64", "terra", "drop", "vista")
VIS = {n: i for i, n in enumerate(VIS_STYLES)}
DEFAULT_VIS = "drop"
# The land stage renders through its own palette at twice the resolution;
# the water palette has no sky in it.
LAND_VIS = "vista"

# angle units: 0..255 clockwise from the front of the cell
FRONT, RIGHT, BACK, LEFT = 0, 64, 128, 192


def genome(*parts):
    """Build a genome from (name_or_index, angle) pairs.

        genome(("jaw", FRONT), ("cilia", 112), ("cilia", 144), ("spike", 16))

    Placement is not decoration - the simulation reads these angles when it
    resolves contact, thrust and armour."""
    out = []
    for p in parts:
        t, a = p
        if isinstance(t, str):
            t = PART[t]
        out.append((int(t), int(a) & 0xFF))
    return out


def _load(path=None):
    path = path or os.environ.get("CPORE_LIB") or _DEFAULT_LIB
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"libcpore.so not found at {path!r}. Build it first:  make lib"
        )
    lib = ctypes.CDLL(path)

    lib.cp_env_create.argtypes = [c_uint32]
    lib.cp_env_create.restype = c_void_p
    lib.cp_env_free.argtypes = [c_void_p]
    lib.cp_env_reset.argtypes = [c_void_p, c_uint32, POINTER(c_int32), POINTER(c_float)]
    lib.cp_env_max_parts.restype = c_int32
    lib.cp_env_part_count.restype = c_int32
    lib.cp_env_genome.argtypes = [c_void_p, POINTER(c_int32)]
    lib.cp_env_generation.argtypes = [c_void_p]
    lib.cp_env_generation.restype = c_int32
    lib.cp_part_name.argtypes = [c_int32]
    lib.cp_part_name.restype = c_char_p
    lib.cp_part_cost.argtypes = [c_int32]
    lib.cp_part_cost.restype = c_int32
    lib.cp_env_step.argtypes = [c_void_p, POINTER(c_float), POINTER(c_float),
                                POINTER(c_float), POINTER(c_int32), POINTER(c_int32)]
    lib.cp_env_obs_dim.restype = c_int32
    lib.cp_env_act_dim.restype = c_int32
    lib.cp_env_state_size.restype = c_size_t
    lib.cp_env_save.argtypes = [c_void_p, c_void_p]
    lib.cp_env_load.argtypes = [c_void_p, c_void_p]
    lib.cp_env_world.argtypes = [c_void_p]
    lib.cp_env_world.restype = c_void_p
    lib.cp_render.argtypes = [c_void_p, c_void_p, c_int32, c_int32]
    lib.cp_render_styled.argtypes = [c_void_p, c_void_p, c_int32, c_int32, c_int32]
    lib.cp_vis_name.argtypes = [c_int32]
    lib.cp_vis_name.restype = c_char_p
    lib.cp_png_write.argtypes = [c_char_p, c_void_p, c_int32, c_int32]
    lib.cp_png_write.restype = c_int32
    lib.cp_policy_greedy.argtypes = [c_void_p, POINTER(c_float)]
    return lib


_LIB = None


def lib():
    global _LIB
    if _LIB is None:
        _LIB = _load()
    return _LIB


class CporeEnv:
    """Single cell-stage environment. Gymnasium-shaped, but not dependent on it."""

    metadata = {"render_modes": ["rgb_array"]}

    def __init__(self, seed: int = 0, parts=None, render_size=(1280, 720),
                 vis=DEFAULT_VIS):
        self._lib = lib()
        self.obs_dim = int(self._lib.cp_env_obs_dim())
        self.act_dim = int(self._lib.cp_env_act_dim())
        self.max_parts = int(self._lib.cp_env_max_parts())
        self.part_count = int(self._lib.cp_env_part_count())
        self._h = self._lib.cp_env_create(c_uint32(seed & 0xFFFFFFFF))
        if not self._h:
            raise MemoryError("cp_env_create failed")

        self._obs = (c_float * self.obs_dim)()
        self._act = (c_float * self.act_dim)()
        self._rew = c_float()
        self._term = c_int32()
        self._trunc = c_int32()
        self._state_size = int(self._lib.cp_env_state_size())

        self.default_parts = parts
        self._gbuf = (c_int32 * (self.max_parts * 2))()
        self.render_size = render_size
        self.vis = vis
        self._fb = None
        self._seed = seed

    # -- lifecycle ---------------------------------------------------------

    def close(self):
        if getattr(self, "_h", None):
            self._lib.cp_env_free(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # -- core api ----------------------------------------------------------

    def reset(self, seed=None, parts=None, options=None):
        """parts: list of (type, angle) pairs, or None for the starter cell.

        Anything over the generation-0 DNA budget is trimmed by the C side,
        so a caller cannot smuggle in a build it has not paid for."""
        if seed is None:
            seed = self._seed
        self._seed = seed
        parts = parts if parts is not None else self.default_parts

        if parts is None:
            pp = None
        else:
            flat = []
            for t, a in parts[:self.max_parts]:
                if isinstance(t, str):
                    t = PART[t]
                flat += [int(t), int(a) & 0xFF]
            flat += [0, 0] * (self.max_parts - len(flat) // 2)
            pp = (c_int32 * (self.max_parts * 2))(*flat)

        self._lib.cp_env_reset(self._h, c_uint32(seed & 0xFFFFFFFF), pp, self._obs)
        return self._out_obs()

    def step(self, action):
        for i in range(self.act_dim):
            self._act[i] = float(action[i])
        self._lib.cp_env_step(self._h, self._act, self._obs,
                              ctypes.byref(self._rew),
                              ctypes.byref(self._term), ctypes.byref(self._trunc))
        return (self._out_obs(), float(self._rew.value),
                bool(self._term.value), bool(self._trunc.value),
                {"status": self.status})

    def _out_obs(self):
        if _np is not None:
            return _np.frombuffer(self._obs, dtype=_np.float32).copy()
        return list(self._obs)

    # -- baseline ----------------------------------------------------------

    def greedy_action(self):
        """The scripted C baseline, exposed so python can benchmark against it."""
        self._lib.cp_policy_greedy(self._lib.cp_env_world(self._h), self._act)
        return list(self._act)

    # -- the live build ----------------------------------------------------

    def genome(self):
        """Current genome as [(part_name, angle), ...] - empty slots dropped."""
        self._lib.cp_env_genome(self._h, self._gbuf)
        return [(PART_NAMES[self._gbuf[i * 2]], self._gbuf[i * 2 + 1])
                for i in range(self.max_parts) if self._gbuf[i * 2] != 0]

    def genome_cost(self):
        return sum(PART_COST[PART[n]] for n, _ in self.genome())

    @property
    def generation(self) -> int:
        return int(self._lib.cp_env_generation(self._h))

    # -- snapshot / restore ------------------------------------------------

    def save_state(self) -> bytes:
        buf = ctypes.create_string_buffer(self._state_size)
        self._lib.cp_env_save(self._h, buf)
        return buf.raw

    def load_state(self, blob: bytes):
        if len(blob) != self._state_size:
            raise ValueError(f"state blob must be {self._state_size} bytes")
        self._lib.cp_env_load(self._h, ctypes.create_string_buffer(blob, len(blob)))

    # -- introspection / rendering -----------------------------------------

    @property
    def status(self) -> str:
        # status lives in CpWorld; read it back through the observation instead
        # of duplicating the struct layout here
        if self._term.value:
            return "evolved" if self._obs[3] >= 1.0 else "dead"
        return "truncated" if self._trunc.value else "running"

    def _draw(self, vis=None):
        w, h = self.render_size
        if self._fb is None:
            self._fb = ctypes.create_string_buffer(w * h * 4)
        v = vis if vis is not None else self.vis
        if isinstance(v, str):
            v = VIS[v]
        self._lib.cp_render_styled(self._lib.cp_env_world(self._h), self._fb, w, h, v)
        return w, h

    def render(self, vis=None):
        w, h = self._draw(vis)
        if _np is not None:
            return _np.frombuffer(self._fb, dtype=_np.uint8).reshape(h, w, 4).copy()
        return self._fb.raw

    def save_png(self, path: str, vis=None):
        w, h = self._draw(vis)
        if self._lib.cp_png_write(path.encode(), self._fb, w, h) != 0:
            raise IOError(f"failed to write {path}")
        return path


class CporeVecEnv:
    """Naive synchronous vectorisation.

    Real training should vectorise on the C side (one thread per shard) - this
    exists so python-side algorithms have something to talk to today.
    """

    def __init__(self, num_envs: int, seed: int = 0, parts=None, **kw):
        self.envs = [CporeEnv(seed=seed + i, parts=parts, **kw) for i in range(num_envs)]
        self.num_envs = num_envs
        self.obs_dim = self.envs[0].obs_dim
        self.act_dim = self.envs[0].act_dim
        self._ep_seed = seed + num_envs

    def reset(self, seed=None):
        return [e.reset(seed=None if seed is None else seed + i)
                for i, e in enumerate(self.envs)]

    def step(self, actions):
        obs, rews, terms, truncs, infos = [], [], [], [], []
        for e, a in zip(self.envs, actions):
            o, r, te, tr, info = e.step(a)
            if te or tr:
                info = dict(info, final_observation=o)
                o = e.reset(seed=self._ep_seed)
                self._ep_seed += 1
            obs.append(o); rews.append(r); terms.append(te)
            truncs.append(tr); infos.append(info)
        return obs, rews, terms, truncs, infos

    def close(self):
        for e in self.envs:
            e.close()


def make_gym_env(**kw):
    """Wrap CporeEnv as a gymnasium.Env. Requires gymnasium + numpy."""
    import gymnasium as gym
    import numpy as np

    class CporeGym(gym.Env):
        metadata = {"render_modes": ["rgb_array"], "render_fps": 60}

        def __init__(self, **kw):
            self._env = CporeEnv(**kw)
            self.observation_space = gym.spaces.Box(
                -4.0, 4.0, (self._env.obs_dim,), dtype=np.float32)
            # 4 control dims (steer x/y, burst, discharge) followed by a design
            # head of (part type, mount angle) per slot. The design head is
            # read only at a generation boundary, so this stays one Box.
            self.action_space = gym.spaces.Box(
                -1.0, 1.0, (self._env.act_dim,), dtype=np.float32)

        def reset(self, seed=None, options=None):
            super().reset(seed=seed)
            parts = (options or {}).get("parts")
            obs = self._env.reset(seed=seed if seed is not None else 0, parts=parts)
            return obs, {}

        def step(self, action):
            return self._env.step(action)

        def render(self):
            return self._env.render()

        def close(self):
            self._env.close()

    return CporeGym(**kw)


# ---------------------------------------------------------------------------
# Stage 2: aquatic
# ---------------------------------------------------------------------------

AQ_PART_NAMES = ("none", "filter", "jaw", "fin", "tail",
                 "spike", "eye", "lung", "plate", "light")
AQ_PART = {n: i for i, n in enumerate(AQ_PART_NAMES)}
AQ_PART_COST = (0, 5, 12, 6, 8, 10, 6, 10, 12, 14)
AQ_GEN_BUDGET = (40, 70, 105, 150)


def aqua_genome(parts, nseg=3, girth=120):
    """Build a 3D body plan from (name, segment, yaw, pitch) tuples.

        aqua_genome([("jaw", 0, 0, 0), ("tail", 2, 128, 0)], nseg=3)

    yaw is 0..255 around the body axis, pitch -64..63 up/down. The simulation
    reads both when it resolves bites, thrust and armour."""
    out = []
    for p in parts:
        t, seg, yaw, pitch = p
        if isinstance(t, str):
            t = AQ_PART[t]
        out.append((int(t), int(seg), int(yaw) & 0xFF, int(pitch)))
    return {"parts": out, "nseg": int(nseg), "girth": int(girth)}


def _bind_aqua(lib):
    lib.cp3_env_create.argtypes = [c_uint32]
    lib.cp3_env_create.restype = c_void_p
    lib.cp3_env_free.argtypes = [c_void_p]
    lib.cp3_env_reset.argtypes = [c_void_p, c_uint32, POINTER(c_int32), POINTER(c_float)]
    lib.cp3_env_step.argtypes = [c_void_p, POINTER(c_float), POINTER(c_float),
                                 POINTER(c_float), POINTER(c_int32), POINTER(c_int32)]
    lib.cp3_env_obs_dim.restype = c_int32
    lib.cp3_env_act_dim.restype = c_int32
    lib.cp3_env_state_size.restype = c_size_t
    lib.cp3_env_save.argtypes = [c_void_p, c_void_p]
    lib.cp3_env_load.argtypes = [c_void_p, c_void_p]
    lib.cp3_env_world.argtypes = [c_void_p]
    lib.cp3_env_world.restype = c_void_p
    lib.cp3_policy_greedy.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp3_render_styled.argtypes = [c_void_p, c_void_p, c_int32, c_int32, c_int32]
    lib.cp3_part_name.argtypes = [c_int32]
    lib.cp3_part_name.restype = c_char_p
    lib.cp3_env_census.argtypes = [c_void_p, POINTER(c_int32), POINTER(c_float)]
    return lib


class AquaEnv:
    """Stage 2. Same shape as CporeEnv; the NPC population evolves on its own."""

    metadata = {"render_modes": ["rgb_array"]}

    def __init__(self, seed: int = 0, genome=None, render_size=(1280, 720),
                 vis=DEFAULT_VIS):
        self._lib = _bind_aqua(lib())
        self.obs_dim = int(self._lib.cp3_env_obs_dim())
        self.act_dim = int(self._lib.cp3_env_act_dim())
        self._h = self._lib.cp3_env_create(c_uint32(seed & 0xFFFFFFFF))
        if not self._h:
            raise MemoryError("cp3_env_create failed")
        self._obs = (c_float * self.obs_dim)()
        self._act = (c_float * self.act_dim)()
        self._rew = c_float()
        self._term = c_int32()
        self._trunc = c_int32()
        self._state_size = int(self._lib.cp3_env_state_size())
        self._cnt = (c_int32 * 3)()
        self._mean = (c_float * 6)()
        self.default_genome = genome
        self.render_size = render_size
        self.vis = vis
        self._fb = None
        self._seed = seed

    def close(self):
        if getattr(self, "_h", None):
            self._lib.cp3_env_free(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def reset(self, seed=None, genome=None):
        if seed is None:
            seed = self._seed
        self._seed = seed
        genome = genome if genome is not None else self.default_genome
        if genome is None:
            pp = None
        else:
            flat = []
            for t, sg, yw, pt in genome["parts"][:10]:
                if isinstance(t, str):
                    t = AQ_PART[t]
                flat += [int(t), int(sg), int(yw) & 0xFF, int(pt)]
            flat += [0, 0, 0, 0] * (10 - len(flat) // 4)
            flat += [genome.get("nseg", 3), genome.get("girth", 120)]
            pp = (c_int32 * len(flat))(*flat)
        self._lib.cp3_env_reset(self._h, c_uint32(seed & 0xFFFFFFFF), pp, self._obs)
        return self._out()

    def step(self, action):
        for i in range(self.act_dim):
            self._act[i] = float(action[i])
        self._lib.cp3_env_step(self._h, self._act, self._obs,
                               ctypes.byref(self._rew),
                               ctypes.byref(self._term), ctypes.byref(self._trunc))
        return (self._out(), float(self._rew.value),
                bool(self._term.value), bool(self._trunc.value), {})

    def _out(self):
        if _np is not None:
            return _np.frombuffer(self._obs, dtype=_np.float32).copy()
        return list(self._obs)

    def greedy_action(self):
        self._lib.cp3_policy_greedy(self._lib.cp3_env_world(self._h), self._act)
        return list(self._act)

    def census(self):
        """Live population statistics - what the ocean has evolved into.

        Read through a C accessor rather than by picking the struct apart from
        python, so a field added to the sim cannot silently shift what this
        returns."""
        self._lib.cp3_env_census(self._h, self._cnt, self._mean)
        return {"births": self._cnt[0], "deaths": self._cnt[1], "pop": self._cnt[2],
                "mean_gen": self._mean[0], "mean_parts": self._mean[1],
                "mean_mouth": self._mean[2], "mean_tail": self._mean[3],
                "mean_light": self._mean[4], "mean_depth": self._mean[5]}

    def save_state(self) -> bytes:
        buf = ctypes.create_string_buffer(self._state_size)
        self._lib.cp3_env_save(self._h, buf)
        return buf.raw

    def load_state(self, blob: bytes):
        self._lib.cp3_env_load(self._h, ctypes.create_string_buffer(blob, len(blob)))

    def _draw(self, vis=None):
        w, h = self.render_size
        if self._fb is None:
            self._fb = ctypes.create_string_buffer(w * h * 4)
        v = vis if vis is not None else self.vis
        if isinstance(v, str):
            v = VIS[v]
        self._lib.cp3_render_styled(self._lib.cp3_env_world(self._h), self._fb, w, h, v)
        return w, h

    def render(self, vis=None):
        w, h = self._draw(vis)
        if _np is not None:
            return _np.frombuffer(self._fb, dtype=_np.uint8).reshape(h, w, 4).copy()
        return self._fb.raw

    def save_png(self, path: str, vis=None):
        w, h = self._draw(vis)
        if self._lib.cp_png_write(path.encode(), self._fb, w, h) != 0:
            raise IOError(f"failed to write {path}")
        return path


# ---------------------------------------------------------------------------
# Stage 3: creature (land)
# ---------------------------------------------------------------------------

LAND_PART_NAMES = ("none", "graze", "jaw", "beak", "leg", "foot", "claw",
                   "horn", "plate", "eye", "ear", "voice", "plume", "wing",
                   "fin", "gill", "digger", "arm", "tail")
LAND_PART = {n: i for i, n in enumerate(LAND_PART_NAMES)}
LAND_PART_COST = (0, 6, 14, 18, 9, 7, 12, 13, 13, 7, 8, 12, 11, 16, 9, 11, 12,
                  13, 10)
LAND_GEN_BUDGET = (82, 132, 186, 248)
LAND_STYLES = ("grazer", "predator", "charmer", "swimmer", "flyer", "burrower")
MEDIA = ("ground", "water", "air", "under")
LAND_MAX_PARTS = 16
LAND_PART_FIELDS = 8
LAND_MAX_SEG = 16


def land_genome(parts, nseg=3, girth=130):
    """Build a land body plan from
    (name, segment, yaw, pitch, scale, mirror, len, bend).

        land_genome([("graze", 0, 0, 0), ("leg", 0, 60, -40, 128, 1, 200, 50)])

    Trailing fields may be omitted: scale defaults to 128, mirror to 0, len to
    128 (a mid-length limb) and bend to 0. Mirrored parts are placed on both
    flanks and cost twice, which is the trade the whole bilateral-symmetry gene
    exists to express; len and bend are how long the limb is and how far its
    joint folds, which is what makes a leg a leg you designed rather than the
    one leg everything has."""
    defaults = (0, 0, 0, 0, 128, 0, 128, 0)
    out = []
    for p in parts:
        p = tuple(p)
        p = p + defaults[len(p):]
        t, seg, yaw, pitch, scale, mirror, ln, bend = p[:8]
        if isinstance(t, str):
            t = LAND_PART[t]
        out.append((int(t), int(seg), int(yaw) & 0xFF, int(pitch),
                    int(scale), 1 if mirror else 0, int(ln), int(bend)))
    return {"parts": out, "nseg": int(nseg), "girth": int(girth)}



# ---------------------------------------------------------------- creature
# editor
#
# The C side deliberately does not expose Cp4Genome across the boundary: the
# struct has grown a field twice, and every layout ctypes mirrors is a promise
# to keep it. What crosses instead is a handle, integers and flat arrays, so
# this binding never has to know what a body plan looks like in memory.

_EDIT_BOUND = [False]


def _bind_edit(lib):
    if _EDIT_BOUND[0]:
        return
    lib.cp4_edit_create.argtypes = [c_int32, c_int32, c_int32]
    lib.cp4_edit_create.restype = c_void_p
    lib.cp4_edit_free.argtypes = [c_void_p]
    lib.cp4_edit_budget.argtypes = [c_void_p, c_int32]
    lib.cp4_edit_load.argtypes = [c_void_p, POINTER(c_int32), c_int32, c_int32]
    lib.cp4_edit_random.argtypes = [c_void_p, c_uint32]
    lib.cp4_edit_style.argtypes = [c_void_p, c_int32]
    lib.cp4_edit_mutate.argtypes = [c_void_p, c_uint32, c_float]
    lib.cp4_edit_view.argtypes = [c_void_p, c_float, c_float, c_float, c_float]
    lib.cp4_edit_orbit.argtypes = [c_void_p, c_float, c_float]
    lib.cp4_edit_get_view.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp4_edit_render.argtypes = [c_void_p, POINTER(c_uint8), c_int32]
    lib.cp4_edit_surface.argtypes = [c_void_p, c_int32, c_int32, POINTER(c_int32)]
    lib.cp4_edit_surface.restype = c_int32
    lib.cp4_edit_pick.argtypes = [c_void_p, c_int32, c_int32]
    lib.cp4_edit_pick.restype = c_int32
    lib.cp4_edit_extent.argtypes = [c_void_p, c_int32, POINTER(c_int32)]
    lib.cp4_edit_extent.restype = c_int32
    lib.cp4_edit_remove.argtypes = [c_void_p, c_int32]
    lib.cp4_edit_remove.restype = c_int32
    lib.cp4_edit_drop.argtypes = [c_void_p, c_int32, c_int32, c_int32, c_int32]
    lib.cp4_edit_drop.restype = c_int32
    lib.cp4_edit_move.argtypes = [c_void_p, c_int32, c_int32, c_int32]
    lib.cp4_edit_move.restype = c_int32
    lib.cp4_edit_shape.argtypes = [c_void_p, c_int32, c_int32, c_int32, c_int32]
    lib.cp4_edit_shape.restype = c_int32
    lib.cp4_edit_mirror.argtypes = [c_void_p, c_int32, c_int32]
    lib.cp4_edit_mirror.restype = c_int32
    lib.cp4_edit_spine_pick.argtypes = [c_void_p, c_int32, c_int32, c_float]
    lib.cp4_edit_spine_pick.restype = c_int32
    lib.cp4_edit_spine_move.argtypes = [c_void_p, c_int32, c_int32, c_int32]
    lib.cp4_edit_spine_move.restype = c_int32
    lib.cp4_edit_spine_girth.argtypes = [c_void_p, c_int32, c_float]
    lib.cp4_edit_frame_hold.argtypes = [c_void_p, c_int32]
    lib.cp4_edit_spine_points.argtypes = [c_void_p, POINTER(c_int32)]
    lib.cp4_edit_spine_points.restype = c_int32
    lib.cp4_edit_spine_add.argtypes = [c_void_p, c_int32]
    lib.cp4_edit_spine_add.restype = c_int32
    lib.cp4_edit_spine_remove.argtypes = [c_void_p, c_int32]
    lib.cp4_edit_spine_remove.restype = c_int32
    lib.cp4_edit_spine_set.argtypes = [c_void_p, c_int32, c_int32]
    lib.cp4_edit_paint.argtypes = [c_void_p, c_int32, c_int32, c_int32, c_int32, c_int32]
    lib.cp4_edit_coats.argtypes = [c_void_p, c_int32, c_int32, c_int32, c_int32]
    lib.cp4_edit_cost.argtypes = [c_void_p]
    lib.cp4_edit_cost.restype = c_int32
    lib.cp4_edit_budget_get.argtypes = [c_void_p]
    lib.cp4_edit_budget_get.restype = c_int32
    lib.cp4_edit_can_afford.argtypes = [c_void_p, c_int32, c_int32]
    lib.cp4_edit_can_afford.restype = c_int32
    lib.cp4_edit_genome.argtypes = [c_void_p, POINTER(c_int32)]
    lib.cp4_edit_body.argtypes = [c_void_p, POINTER(c_int32)]
    lib.cp4_edit_stat_count.restype = c_int32
    lib.cp4_edit_stats.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp4_edit_finish.argtypes = [c_void_p, POINTER(c_int32)]
    _EDIT_BOUND[0] = True


EDIT_STATS = ("speed", "accel", "turn", "jump", "grip", "hp", "armor",
              "bite", "claw", "graze", "carn", "sight", "hearing", "charm",
              "reach", "carry", "stamina", "swim", "fly", "dig")

PATTERNS = ("plain", "bands", "spots", "counter", "stripes", "mottle",
            "gradient", "rings")
PATTERN = {n: i for i, n in enumerate(PATTERNS)}


class CreatureEditor:
    """An open editing session on one body plan.

    Everything that takes a position takes it in pixels, where a mouse would
    be, because the renderer and the picker are the same march - so a part
    dropped on a pixel lands on the surface that pixel showed.

        ed = CreatureEditor(480, 480)
        ed.style("predator")
        slot = ed.pick(240, 240)          # what is under the pointer
        ed.drop(260, 200, "horn")         # put a horn there
        ed.paint(hue=150, sat=220)
        ed.save_png("creature.png")
    """

    def __init__(self, width=480, height=480, budget=None, lib_path=None):
        self._lib = _load(lib_path)
        _bind_edit(self._lib)
        if budget is None:
            budget = LAND_GEN_BUDGET[-1]
        self._h = self._lib.cp4_edit_create(int(width), int(height), int(budget))
        if not self._h:
            raise RuntimeError("could not open a creature editor")
        self.width, self.height = int(width), int(height)
        self._fb = (c_uint8 * (self.width * self.height * 4))()
        self._parts = (c_int32 * (LAND_MAX_PARTS * LAND_PART_FIELDS))()
        self._body = (c_int32 * 11)()
        self._spine = (c_int32 * (LAND_MAX_SEG * 2))()
        self._stats = (c_float * len(EDIT_STATS))()

    def close(self):
        if getattr(self, "_h", None):
            self._lib.cp4_edit_free(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    # ---- loading ----
    def load(self, plan):
        """Take a land_genome() dict, or a list of part tuples."""
        if isinstance(plan, dict):
            parts, nseg, girth = plan["parts"], plan["nseg"], plan["girth"]
        else:
            d = land_genome(plan)
            parts, nseg, girth = d["parts"], d["nseg"], d["girth"]
        buf = (c_int32 * (LAND_MAX_PARTS * LAND_PART_FIELDS))()
        for i, p in enumerate(parts[:LAND_MAX_PARTS]):
            for j, val in enumerate(p[:LAND_PART_FIELDS]):
                buf[i * LAND_PART_FIELDS + j] = int(val)
        self._lib.cp4_edit_load(self._h, buf, int(nseg), int(girth))
        return self

    def style(self, name_or_index):
        i = LAND_STYLES.index(name_or_index) if isinstance(name_or_index, str) \
            else int(name_or_index)
        self._lib.cp4_edit_style(self._h, i)
        return self

    def random(self, seed=0):
        self._lib.cp4_edit_random(self._h, int(seed) & 0xFFFFFFFF)
        return self

    def mutate(self, seed=0, rate=0.25):
        self._lib.cp4_edit_mutate(self._h, int(seed) & 0xFFFFFFFF, float(rate))
        return self

    # ---- camera ----
    def view(self, azimuth=None, elev=None, zoom=None, phase=None):
        cur = (c_float * 4)()
        self._lib.cp4_edit_get_view(self._h, cur)
        self._lib.cp4_edit_view(
            self._h,
            float(cur[0] if azimuth is None else azimuth),
            float(cur[1] if elev is None else elev),
            float(cur[2] if zoom is None else zoom),
            float(cur[3] if phase is None else phase))
        return self

    def orbit(self, dazimuth=0.0, delev=0.0):
        self._lib.cp4_edit_orbit(self._h, float(dazimuth), float(delev))
        return self

    # ---- interaction, in pixels ----
    def surface(self, x, y):
        """Where on the body a pixel lands, as (seg, yaw, pitch), or None if
        the pointer is off the animal. This is the hover question; pick() is
        the "what did I grab" question and says None over bare trunk."""
        buf = (c_int32 * 3)()
        if not self._lib.cp4_edit_surface(self._h, int(x), int(y), buf):
            return None
        return (int(buf[0]), int(buf[1]), int(buf[2]))

    def pick(self, x, y):
        """Which part slot is under a pixel, or None."""
        s = self._lib.cp4_edit_pick(self._h, int(x), int(y))
        return None if s < 0 else s

    def extent(self, slot):
        """Where a part sits on screen: (cx, cy, tip_x, tip_y, radius) in
        pixels, or None if the slot is empty. Projected from the geometry, so
        it costs nothing next to a pick - which is what makes it usable for
        drag handles that follow the part every frame."""
        if not self._lib.cp4_edit_extent(self._h, int(slot), self._parts):
            return None
        return tuple(self._parts[i] for i in range(5))

    def drop(self, x, y, part, mirror=-1):
        """Place a part where the pointer is. Returns the slot, or None if it
        missed the body, there was no slot free, or the budget said no - and
        in every one of those cases nothing changed."""
        t = LAND_PART[part] if isinstance(part, str) else int(part)
        s = self._lib.cp4_edit_drop(self._h, int(x), int(y), t, int(mirror))
        return None if s < 0 else s

    def move(self, slot, x, y):
        return bool(self._lib.cp4_edit_move(self._h, int(slot), int(x), int(y)))

    def remove(self, slot):
        return bool(self._lib.cp4_edit_remove(self._h, int(slot)))

    def shape(self, slot, scale=-1, length=-1, bend=-1000):
        return bool(self._lib.cp4_edit_shape(self._h, int(slot), int(scale),
                                             int(length), int(bend)))

    def mirror(self, slot, on=True):
        return bool(self._lib.cp4_edit_mirror(self._h, int(slot), 1 if on else 0))

    def spine_pick(self, x, y, grab=14.0):
        v = self._lib.cp4_edit_spine_pick(self._h, int(x), int(y), float(grab))
        return None if v < 0 else v

    def spine_move(self, vert, x, y):
        """Drag one control point to a pixel, on the plane facing the camera."""
        return bool(self._lib.cp4_edit_spine_move(self._h, int(vert), int(x), int(y)))

    def spine_girth(self, vert, amount):
        return bool(self._lib.cp4_edit_spine_girth(self._h, int(vert), float(amount)))

    def frame_hold(self, on=True):
        """Freeze the viewport's automatic framing for the length of a drag."""
        self._lib.cp4_edit_frame_hold(self._h, 1 if on else 0)
        return self

    def spine_points(self):
        """[(x, y), ...] for every control point, in output pixels."""
        n = self._lib.cp4_edit_spine_points(self._h, self._spine)
        return [(int(self._spine[2 * i]), int(self._spine[2 * i + 1]))
                for i in range(n)]

    def spine_add(self, front=False):
        """Grow the spine at one end. Returns the new count, or None if the
        DNA budget or CP4_MAX_SEG said no."""
        n = self._lib.cp4_edit_spine_add(self._h, 1 if front else 0)
        return None if n < 0 else int(n)

    def spine_remove(self, front=False):
        n = self._lib.cp4_edit_spine_remove(self._h, 1 if front else 0)
        return None if n < 0 else int(n)

    def spine(self, nseg=-1, girth=-1):
        self._lib.cp4_edit_spine_set(self._h, int(nseg), int(girth))
        return self

    # ---- paint ----
    def paint(self, hue=-1, hue2=-1, hue3=-1, sat=-1, val=-1):
        self._lib.cp4_edit_paint(self._h, int(hue), int(hue2), int(hue3),
                                 int(sat), int(val))
        return self

    def coats(self, pattern=-1, scale=-1, pattern2=-1, scale2=-1):
        p = PATTERN[pattern] if isinstance(pattern, str) else int(pattern)
        q = PATTERN[pattern2] if isinstance(pattern2, str) else int(pattern2)
        self._lib.cp4_edit_coats(self._h, p, int(scale), q, int(scale2))
        return self

    # ---- readback ----
    @property
    def cost(self):
        return int(self._lib.cp4_edit_cost(self._h))

    @property
    def budget(self):
        return int(self._lib.cp4_edit_budget_get(self._h))

    def can_afford(self, part, mirror=0):
        t = LAND_PART[part] if isinstance(part, str) else int(part)
        return bool(self._lib.cp4_edit_can_afford(self._h, t, int(mirror)))

    def parts(self):
        """[(slot, name, seg, yaw, pitch, scale, mirror, len, bend), ...]"""
        self._lib.cp4_edit_genome(self._h, self._parts)
        out = []
        for i in range(LAND_MAX_PARTS):
            f = self._parts[i * LAND_PART_FIELDS:(i + 1) * LAND_PART_FIELDS]
            if f[0] == 0:
                continue
            out.append((i, LAND_PART_NAMES[f[0]]) + tuple(int(x) for x in f[1:]))
        return out

    def body(self):
        self._lib.cp4_edit_body(self._h, self._body)
        k = ("nseg", "girth", "hue", "hue2", "hue3",
             "sat", "val", "pattern", "pscale", "pattern2", "pscale2")
        return dict(zip(k, (int(x) for x in self._body)))

    def stats(self):
        self._lib.cp4_edit_stats(self._h, self._stats)
        return dict(zip(EDIT_STATS, (float(x) for x in self._stats)))

    def finish(self):
        """Compact the slots and hand back a plan the environment accepts."""
        buf = (c_int32 * (LAND_MAX_PARTS * LAND_PART_FIELDS))()
        self._lib.cp4_edit_finish(self._h, buf)
        parts = []
        for i in range(LAND_MAX_PARTS):
            f = buf[i * LAND_PART_FIELDS:(i + 1) * LAND_PART_FIELDS]
            if f[0] == 0:
                continue
            parts.append(tuple(int(x) for x in f))
        b = self.body()
        return {"parts": parts, "nseg": b["nseg"], "girth": b["girth"]}

    # ---- pixels ----
    def render(self, quality=2):
        """RGBA bytes, width*height*4. quality 0 is what you draw while the
        mouse is moving; 3 is supersampled and slow."""
        self._lib.cp4_edit_render(self._h, self._fb, int(quality))
        return bytes(self._fb)

    def save_png(self, path, quality=3):
        self._lib.cp4_edit_render(self._h, self._fb, int(quality))
        self._lib.cp_png_write(path.encode(), self._fb,
                               c_int(self.width), c_int(self.height))
        return path


def _bind_land(lib):
    lib.cp4_env_create.argtypes = [c_uint32]
    lib.cp4_env_create.restype = c_void_p
    lib.cp4_env_free.argtypes = [c_void_p]
    lib.cp4_env_reset.argtypes = [c_void_p, c_uint32, POINTER(c_int32), POINTER(c_float)]
    lib.cp4_env_step.argtypes = [c_void_p, POINTER(c_float), POINTER(c_float),
                                 POINTER(c_float), POINTER(c_int32), POINTER(c_int32)]
    lib.cp4_env_obs_dim.restype = c_int32
    lib.cp4_env_act_dim.restype = c_int32
    lib.cp4_env_state_size.restype = c_size_t
    lib.cp4_env_save.argtypes = [c_void_p, c_void_p]
    lib.cp4_env_load.argtypes = [c_void_p, c_void_p]
    lib.cp4_env_world.argtypes = [c_void_p]
    lib.cp4_env_world.restype = c_void_p
    lib.cp4_policy_greedy.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp4_render_styled.argtypes = [c_void_p, c_void_p, c_int32, c_int32, c_int32]
    lib.cp4_part_name.argtypes = [c_int32]
    lib.cp4_part_name.restype = c_char_p
    lib.cp4_env_census.argtypes = [c_void_p, POINTER(c_int32), POINTER(c_float)]
    lib.cp4_height.argtypes = [c_uint32, c_float, c_float]
    lib.cp4_height.restype = c_float
    lib.cp5_legacy_from_world.argtypes = [c_void_p, POINTER(c_float)]
    return lib


class LandEnv:
    """Stage 3. Terrain, rival species, and one budget to buy charm or violence."""

    metadata = {"render_modes": ["rgb_array"]}

    def __init__(self, seed: int = 0, genome=None, render_size=(1280, 720),
                 vis=LAND_VIS):
        self._lib = _bind_land(lib())
        self.obs_dim = int(self._lib.cp4_env_obs_dim())
        self.act_dim = int(self._lib.cp4_env_act_dim())
        self._h = self._lib.cp4_env_create(c_uint32(seed & 0xFFFFFFFF))
        if not self._h:
            raise MemoryError("cp4_env_create failed")
        self._obs = (c_float * self.obs_dim)()
        self._act = (c_float * self.act_dim)()
        self._rew = c_float()
        self._term = c_int32()
        self._trunc = c_int32()
        self._state_size = int(self._lib.cp4_env_state_size())
        self._cnt = (c_int32 * 19)()
        self._mean = (c_float * 8)()
        self.default_genome = genome
        self.render_size = render_size
        self.vis = vis
        self._fb = None
        self._seed = seed

    def close(self):
        if getattr(self, "_h", None):
            self._lib.cp4_env_free(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def reset(self, seed=None, genome=None):
        if seed is None:
            seed = self._seed
        self._seed = seed
        genome = genome if genome is not None else self.default_genome
        if genome is None:
            pp = None
        else:
            flat = []
            for p in genome["parts"][:LAND_MAX_PARTS]:
                p = tuple(p)
                p = p + (0, 0, 0, 0, 128, 0, 128, 0)[len(p):]
                t, sg, yw, pt, sc, mr, ln, bd = p[:8]
                if isinstance(t, str):
                    t = LAND_PART[t]
                flat += [int(t), int(sg), int(yw) & 0xFF, int(pt), int(sc),
                         int(mr), int(ln), int(bd)]
            flat += [0, 0, 0, 0, 0, 0, 128, 0] * (
                LAND_MAX_PARTS - len(flat) // LAND_PART_FIELDS)
            flat += [genome.get("nseg", 3), genome.get("girth", 130)]
            pp = (c_int32 * len(flat))(*flat)
        self._lib.cp4_env_reset(self._h, c_uint32(seed & 0xFFFFFFFF), pp, self._obs)
        return self._out()

    def step(self, action):
        for i in range(self.act_dim):
            self._act[i] = float(action[i])
        self._lib.cp4_env_step(self._h, self._act, self._obs,
                               ctypes.byref(self._rew),
                               ctypes.byref(self._term), ctypes.byref(self._trunc))
        return (self._out(), float(self._rew.value),
                bool(self._term.value), bool(self._trunc.value), {})

    def _out(self):
        if _np is not None:
            return _np.frombuffer(self._obs, dtype=_np.float32).copy()
        return list(self._obs)

    def greedy_action(self):
        self._lib.cp4_policy_greedy(self._lib.cp4_env_world(self._h), self._act)
        return list(self._act)

    def height(self, x, y=None):
        """Ground height at a world position. y grows downward, so a smaller
        number is higher ground - the same axis the aquatic stage used for
        depth, kept pointing the same way on purpose."""
        if y is None:
            x, y = x
        return float(self._lib.cp4_height(c_uint32(self._seed & 0xFFFFFFFF),
                                          c_float(x), c_float(y)))

    def legacy(self):
        """What this creature hands forward to the civilisation stage.

        Three multipliers - military, economic, religious - derived from the
        body that finished the run. Feed the result straight to CivEnv:

            land = LandEnv(seed=7)   # ... play the creature stage ...
            civ  = CivEnv(seed=7, legacy=land.legacy())

        Same seed means the same planet, so the cities go up on the hills the
        creature walked over."""
        buf = (c_float * 3)()
        self._lib.cp5_legacy_from_world(self._lib.cp4_env_world(self._h), buf)
        return {"military": buf[0], "economic": buf[1], "religious": buf[2]}

    def census(self):
        """Live population and diplomacy, read through a C accessor rather than
        by picking the struct apart from python."""
        self._lib.cp4_env_census(self._h, self._cnt, self._mean)
        return {"births": self._cnt[0], "deaths": self._cnt[1], "pop": self._cnt[2],
                "allies": self._cnt[3], "enemies": self._cnt[4],
                "befriended": self._cnt[5], "kills": self._cnt[6],
                "discovered": self._cnt[7], "hatchlings": self._cnt[8],
                "has_nest": bool(self._cnt[9]), "medium": MEDIA[self._cnt[10] % 4],
                "steps": {m: self._cnt[11 + i] for i, m in enumerate(MEDIA)},
                "ate": {"bush": self._cnt[15], "kelp": self._cnt[16],
                        "tuber": self._cnt[17], "meat": self._cnt[18]},
                "mean_gen": self._mean[0], "mean_parts": self._mean[1],
                "mean_legs": self._mean[2], "mean_charm": self._mean[3],
                "dna": self._mean[4], "travelled": self._mean[5],
                "furthest": self._mean[6], "nest_store": self._mean[7]}

    def save_state(self) -> bytes:
        buf = ctypes.create_string_buffer(self._state_size)
        self._lib.cp4_env_save(self._h, buf)
        return buf.raw

    def load_state(self, blob: bytes):
        self._lib.cp4_env_load(self._h, ctypes.create_string_buffer(blob, len(blob)))

    def _draw(self, vis=None):
        w, h = self.render_size
        if self._fb is None:
            self._fb = ctypes.create_string_buffer(w * h * 4)
        v = vis if vis is not None else self.vis
        if isinstance(v, str):
            v = VIS[v]
        self._lib.cp4_render_styled(self._lib.cp4_env_world(self._h), self._fb, w, h, v)
        return w, h

    def render(self, vis=None):
        w, h = self._draw(vis)
        if _np is not None:
            return _np.frombuffer(self._fb, dtype=_np.uint8).reshape(h, w, 4).copy()
        return self._fb.raw

    def save_png(self, path: str, vis=None):
        w, h = self._draw(vis)
        if self._lib.cp_png_write(path.encode(), self._fb, w, h) != 0:
            raise IOError(f"failed to write {path}")
        return path


# ---------------------------------------------------------------------------
# Stage 4: civilisation
# ---------------------------------------------------------------------------

APPROACH_NAMES = ("military", "economic", "religious")
APPROACH = {n: i for i, n in enumerate(APPROACH_NAMES)}
CIV_MAX_CITIES = 12


def _bind_civ(lib):
    lib.cp5_env_create.argtypes = [c_uint32]
    lib.cp5_env_create.restype = c_void_p
    lib.cp5_env_free.argtypes = [c_void_p]
    lib.cp5_env_reset.argtypes = [c_void_p, c_uint32, POINTER(c_float), POINTER(c_float)]
    lib.cp5_env_step.argtypes = [c_void_p, POINTER(c_float), POINTER(c_float),
                                 POINTER(c_float), POINTER(c_int32), POINTER(c_int32)]
    lib.cp5_env_obs_dim.restype = c_int32
    lib.cp5_env_act_dim.restype = c_int32
    lib.cp5_env_state_size.restype = c_size_t
    lib.cp5_env_save.argtypes = [c_void_p, c_void_p]
    lib.cp5_env_load.argtypes = [c_void_p, c_void_p]
    lib.cp5_env_world.argtypes = [c_void_p]
    lib.cp5_env_world.restype = c_void_p
    lib.cp5_policy_greedy.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp5_render_styled.argtypes = [c_void_p, c_void_p, c_int32, c_int32, c_int32]
    lib.cp5_env_census.argtypes = [c_void_p, POINTER(c_int32), POINTER(c_float)]
    lib.cp5_approach_name.argtypes = [c_int32]
    lib.cp5_approach_name.restype = c_char_p
    return lib


class CivEnv:
    """Stage 4. One nation on the same planet stage 3 was played on.

    `legacy` is the three multipliers a finished creature hands forward -
    military, economic, religious - or None for a civilisation that inherited
    nothing. LandEnv does not produce it automatically on purpose: chaining the
    stages is the caller's decision, not a hidden coupling in the library."""

    metadata = {"render_modes": ["rgb_array"]}

    def __init__(self, seed: int = 0, legacy=None, render_size=(1280, 720),
                 vis=DEFAULT_VIS):
        self._lib = _bind_civ(lib())
        self.obs_dim = int(self._lib.cp5_env_obs_dim())
        self.act_dim = int(self._lib.cp5_env_act_dim())
        self._h = self._lib.cp5_env_create(c_uint32(seed & 0xFFFFFFFF))
        if not self._h:
            raise MemoryError("cp5_env_create failed")
        self._obs = (c_float * self.obs_dim)()
        self._act = (c_float * self.act_dim)()
        self._rew = c_float()
        self._term = c_int32()
        self._trunc = c_int32()
        self._state_size = int(self._lib.cp5_env_state_size())
        self._cnt = (c_int32 * 8)()
        self._val = (c_float * 5)()
        self.default_legacy = legacy
        self.render_size = render_size
        self.vis = vis
        self._fb = None
        self._seed = seed

    def close(self):
        if getattr(self, "_h", None):
            self._lib.cp5_env_free(self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def reset(self, seed=None, legacy=None):
        if seed is None:
            seed = self._seed
        self._seed = seed
        legacy = legacy if legacy is not None else self.default_legacy
        if legacy is None:
            lp = None
        else:
            if isinstance(legacy, dict):
                legacy = [legacy.get(n, 1.0) for n in APPROACH_NAMES]
            lp = (c_float * 3)(*[float(v) for v in legacy])
        self._lib.cp5_env_reset(self._h, c_uint32(seed & 0xFFFFFFFF), lp, self._obs)
        return self._out()

    def step(self, action):
        for i in range(self.act_dim):
            self._act[i] = float(action[i])
        self._lib.cp5_env_step(self._h, self._act, self._obs,
                               ctypes.byref(self._rew),
                               ctypes.byref(self._term), ctypes.byref(self._trunc))
        return (self._out(), float(self._rew.value),
                bool(self._term.value), bool(self._trunc.value), {})

    def _out(self):
        if _np is not None:
            return _np.frombuffer(self._obs, dtype=_np.float32).copy()
        return list(self._obs)

    def greedy_action(self):
        self._lib.cp5_policy_greedy(self._lib.cp5_env_world(self._h), self._act)
        return list(self._act)

    def census(self):
        self._lib.cp5_env_census(self._h, self._cnt, self._val)
        return {"cities": self._cnt[0], "captured": self._cnt[1],
                "converted": self._cnt[2], "bought": self._cnt[3],
                "lost": self._cnt[4],
                "units": {"military": self._cnt[5], "economic": self._cnt[6],
                          "religious": self._cnt[7]},
                "money": self._val[0], "income": self._val[1],
                "bonus": {"military": self._val[2], "economic": self._val[3],
                          "religious": self._val[4]}}

    def save_state(self) -> bytes:
        buf = ctypes.create_string_buffer(self._state_size)
        self._lib.cp5_env_save(self._h, buf)
        return buf.raw

    def load_state(self, blob: bytes):
        self._lib.cp5_env_load(self._h, ctypes.create_string_buffer(blob, len(blob)))

    def _draw(self, vis=None):
        w, h = self.render_size
        if self._fb is None:
            self._fb = ctypes.create_string_buffer(w * h * 4)
        v = vis if vis is not None else self.vis
        if isinstance(v, str):
            v = VIS[v]
        self._lib.cp5_render_styled(self._lib.cp5_env_world(self._h), self._fb, w, h, v)
        return w, h

    def render(self, vis=None):
        w, h = self._draw(vis)
        if _np is not None:
            return _np.frombuffer(self._fb, dtype=_np.uint8).reshape(h, w, 4).copy()
        return self._fb.raw

    def save_png(self, path: str, vis=None):
        w, h = self._draw(vis)
        if self._lib.cp_png_write(path.encode(), self._fb, w, h) != 0:
            raise IOError(f"failed to write {path}")
        return path
