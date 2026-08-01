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
from ctypes import POINTER, c_float, c_int32, c_uint32, c_void_p, c_size_t, c_char_p

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
VIS_STYLES = ("abyss", "dmg", "neon", "petri", "c64")
VIS = {n: i for i, n in enumerate(VIS_STYLES)}
DEFAULT_VIS = "petri"

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
