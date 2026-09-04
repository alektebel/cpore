"""PufferLib-native vector environments for cpore.

The old ``CporeVecEnv`` loops over envs in Python: one ctypes call per env
per step. These envs make ONE call per step for all N envs: C owns N worlds
laid out contiguously (``src/vec.c``) and writes straight into numpy buffers
the caller owns - observations, rewards, terminals. No per-env Python.

Works with pufferlib installed (native ``PufferEnv`` with buffers, ready for
``pufferlib.vector`` / PPO) and without it (same numpy reset/step API, so
training code never branches on the import)::

    from cpore.puffer import LandPuffer, make_puffer_env
    env = make_puffer_env("land", num_envs=64, seed=0)
    obs, infos = env.reset()
    obs, rewards, terms, truncs, infos = env.step(actions)

Done lanes auto-reset inside the C step with an incrementing seed,
puffer-style: ``terminals[i]`` reports the ending while ``observations[i]``
already holds the fresh lane.
"""

from __future__ import annotations

import ctypes
import os
from ctypes import POINTER, c_float, c_int32, c_uint32, c_void_p

try:
    import pufferlib
    from pufferlib import PufferEnv as _PufferBase
    _HAVE_PUFFER = True
except ImportError:                                    # pragma: no cover
    pufferlib = None                                   # type: ignore
    _PufferBase = object                               # type: ignore
    _HAVE_PUFFER = False

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_LIB = os.path.normpath(os.path.join(_HERE, "..", "..", "build", "libcpore.so"))


def _load(path=None):
    path = path or os.environ.get("CPORE_LIB") or _DEFAULT_LIB
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"libcpore.so not found at {path!r}. Build it first:  make lib"
        )
    lib = ctypes.CDLL(path)
    for prefix, new in (("cp_", None), ("cp3_", None), ("cp4_", None),
                        ("cp5_", None), ("cp6_", None)):
        _ = prefix, new  # bound explicitly below for readability
    # cell
    lib.cp_vec_create.argtypes = [c_int32, c_uint32]
    lib.cp_vec_create.restype = c_void_p
    lib.cp_vec_free.argtypes = [c_void_p]
    lib.cp_vec_count.restype = c_int32
    lib.cp_vec_count.argtypes = [c_void_p]
    lib.cp_vec_reset_all.argtypes = [c_void_p, c_uint32]
    lib.cp_vec_step.argtypes = [c_void_p, POINTER(c_float), POINTER(c_float),
                                POINTER(c_float), POINTER(c_int32),
                                POINTER(c_int32), c_int32]
    lib.cp_vec_observe.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp_vec_greedy.argtypes = [c_void_p, POINTER(c_float)]
    # aqua
    lib.cp3_vec_create.argtypes = [c_int32, c_uint32]
    lib.cp3_vec_create.restype = c_void_p
    lib.cp3_vec_free.argtypes = [c_void_p]
    lib.cp3_vec_reset_all.argtypes = [c_void_p, c_uint32]
    lib.cp3_vec_step.argtypes = lib.cp_vec_step.argtypes
    lib.cp3_vec_observe.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp3_vec_greedy.argtypes = [c_void_p, POINTER(c_float)]
    # land
    lib.cp4_vec_create.argtypes = [c_int32, c_uint32]
    lib.cp4_vec_create.restype = c_void_p
    lib.cp4_vec_free.argtypes = [c_void_p]
    lib.cp4_vec_reset_all.argtypes = [c_void_p, c_uint32]
    lib.cp4_vec_step.argtypes = lib.cp_vec_step.argtypes
    lib.cp4_vec_observe.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp4_vec_greedy.argtypes = [c_void_p, POINTER(c_float)]
    # civ
    lib.cp5_vec_create.argtypes = [c_int32, c_uint32]
    lib.cp5_vec_create.restype = c_void_p
    lib.cp5_vec_free.argtypes = [c_void_p]
    lib.cp5_vec_reset_all.argtypes = [c_void_p, c_uint32]
    lib.cp5_vec_step.argtypes = lib.cp_vec_step.argtypes
    lib.cp5_vec_observe.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp5_vec_greedy.argtypes = [c_void_p, POINTER(c_float)]
    # tribe
    lib.cp6_vec_create.argtypes = [c_int32, c_uint32]
    lib.cp6_vec_create.restype = c_void_p
    lib.cp6_vec_free.argtypes = [c_void_p]
    lib.cp6_vec_reset_all.argtypes = [c_void_p, c_uint32]
    lib.cp6_vec_step.argtypes = lib.cp_vec_step.argtypes
    lib.cp6_vec_observe.argtypes = [c_void_p, POINTER(c_float)]
    lib.cp6_vec_greedy.argtypes = [c_void_p, POINTER(c_float)]
    return lib


_LIB = None


def lib():
    global _LIB
    if _LIB is None:
        _LIB = _load()
    return _LIB


# (create, free, reset_all, step, observe, greedy, obs_dim, act_dim)
_SPECS = {
    "cell":  ("cp_vec_create", "cp_vec_free", "cp_vec_reset_all", "cp_vec_step",
              "cp_vec_observe", "cp_vec_greedy", 97, 28),
    "aqua":  ("cp3_vec_create", "cp3_vec_free", "cp3_vec_reset_all", "cp3_vec_step",
              "cp3_vec_observe", "cp3_vec_greedy", 113, 46),
    "land":  ("cp4_vec_create", "cp4_vec_free", "cp4_vec_reset_all", "cp4_vec_step",
              "cp4_vec_observe", "cp4_vec_greedy", 170, 106),
    "civ":   ("cp5_vec_create", "cp5_vec_free", "cp5_vec_reset_all", "cp5_vec_step",
              "cp5_vec_observe", "cp5_vec_greedy", 132, 17),
    "tribe": ("cp6_vec_create", "cp6_vec_free", "cp6_vec_reset_all", "cp6_vec_step",
              "cp6_vec_observe", "cp6_vec_greedy", 41, 9),
    "space": ("cp7_vec_create", "cp7_vec_free", "cp7_vec_reset_all", "cp7_vec_step",
              "cp7_vec_observe", "cp7_vec_greedy", 137, 7),
}


class CporePuffer(_PufferBase):
    """One puffer env over N C worlds. Subclass per stage for a fixed spec."""

    _stage = "land"

    def __init__(self, num_envs=16, seed=0, buf=None):
        spec = _SPECS[self._stage]
        self._lib = lib()
        self._fns = [getattr(self._lib, n) for n in spec[:6]]
        self.single_obs_dim, self.single_act_dim = spec[6], spec[7]
        self.num_agents = int(num_envs)
        self._seed = seed
        self._h = self._fns[0](self.num_agents, c_uint32(seed & 0xFFFFFFFF))
        if not self._h:
            raise MemoryError(f"{self._stage} vec create failed")
        if _HAVE_PUFFER:
            import gymnasium.spaces
            self.single_observation_space = gymnasium.spaces.Box(
                low=-4.0, high=4.0,
                shape=(self.single_obs_dim,), dtype=np.float32)
            self.single_action_space = gymnasium.spaces.Box(
                low=-1.0, high=1.0,
                shape=(self.single_act_dim,), dtype=np.float32)
            super().__init__(buf)
        else:  # pragma: no cover - plain numpy buffers, same layout
            self.observations = np.zeros((self.num_agents, self.single_obs_dim),
                                         dtype=np.float32)
            self.rewards = np.zeros(self.num_agents, dtype=np.float32)
            self.terminals = np.zeros(self.num_agents, dtype=bool)
            self.truncations = np.zeros(self.num_agents, dtype=bool)
            self.masks = np.ones(self.num_agents, dtype=bool)
            self.actions = np.zeros((self.num_agents, self.single_act_dim),
                                    dtype=np.float32)
        self._term_i = np.zeros(self.num_agents, dtype=np.int32)
        self._trunc_i = np.zeros(self.num_agents, dtype=np.int32)
        self._ep_ret = np.zeros(self.num_agents, dtype=np.float32)
        self._ep_len = np.zeros(self.num_agents, dtype=np.int32)

    def close(self):
        if getattr(self, "_h", None):
            self._fns[1](self._h)
            self._h = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # -- helpers to hand numpy buffers straight to C ---------------------
    def _ptr(self, arr):
        return arr.ctypes.data_as(POINTER(c_float))

    def _ipt(self, arr):
        return arr.ctypes.data_as(POINTER(c_int32))

    # -- API --------------------------------------------------------------
    def reset(self, seed=None):
        if seed is None:
            seed = self._seed
        self._seed = seed
        self._fns[2](self._h, c_uint32(seed & 0xFFFFFFFF))
        self._fns[4](self._h, self._ptr(self.observations))
        self.rewards[:] = 0
        self.terminals[:] = False
        self.truncations[:] = False
        self._ep_ret[:] = 0
        self._ep_len[:] = 0
        return self.observations, [{} for _ in range(self.num_agents)]

    def step(self, actions):
        acts = np.asarray(actions, dtype=np.float32).reshape(
            self.num_agents, self.single_act_dim)
        self._fns[3](self._h, self._ptr(acts), self._ptr(self.observations),
                     self._ptr(self.rewards), self._ipt(self._term_i),
                     self._ipt(self._trunc_i), c_int32(1))
        self.terminals[:] = self._term_i.astype(bool)
        self.truncations[:] = self._trunc_i.astype(bool)
        self._ep_ret += self.rewards
        self._ep_len += 1
        infos = []
        for i in range(self.num_agents):
            if self.terminals[i] or self.truncations[i]:
                infos.append({"episode_return": float(self._ep_ret[i]),
                              "episode_length": int(self._ep_len[i])})
                self._ep_ret[i] = 0
                self._ep_len[i] = 0
            else:
                infos.append({})
        return (self.observations, self.rewards.copy(),
                self.terminals.copy(), self.truncations.copy(), infos)

    def greedy_actions(self):
        """Scripted-baseline actions for all lanes (sanity/throughput bar)."""
        out = np.zeros((self.num_agents, self.single_act_dim), dtype=np.float32)
        self._fns[5](self._h, self._ptr(out))
        return out


class CellPuffer(CporePuffer):
    _stage = "cell"


class AquaPuffer(CporePuffer):
    _stage = "aqua"


class LandPuffer(CporePuffer):
    _stage = "land"


class CivPuffer(CporePuffer):
    _stage = "civ"


class TribePuffer(CporePuffer):
    _stage = "tribe"


class SpacePuffer(CporePuffer):
    _stage = "space"


def make_puffer_env(stage="land", num_envs=16, seed=0, buf=None):
    """Factory: ``make_puffer_env("land", 64)``. Stage in
    cell/aqua/land/tribe/civ/space."""
    cls = {"cell": CellPuffer, "aqua": AquaPuffer, "land": LandPuffer,
           "tribe": TribePuffer, "civ": CivPuffer, "space": SpacePuffer}[stage]
    return cls(num_envs=num_envs, seed=seed, buf=buf)


if __name__ == "__main__":  # pragma: no cover
    import time
    for stage in ("cell", "land"):
        env = make_puffer_env(stage, num_envs=64, seed=0)
        env.reset()
        t0 = time.time()
        n = 1500
        for _ in range(n):
            env.step(env.greedy_actions())
        dt = time.time() - t0
        print(f"{stage:6s} puffer path: {64 * n / dt:,.0f} steps/s (64 envs, greedy)")
        env.close()
