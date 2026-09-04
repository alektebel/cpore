"""The cell stage, as a PufferLib environment.

    from cpore_puffer.cell import Cell
    env = Cell(num_envs=1024)

The simulation is cpore's C, compiled into this package's extension. What
this file adds is the parts that are properly Python's: the spaces, the
buffers PufferLib hands down to C, and the loop that turns a batch of actions
into a batch of transitions.

The action space is deliberately small and deliberately the same one a person
plays with. Nine ways to push - let go, then the compass - a boost, and a
discharge. A key press and an action index are the same number, so `play.py`
and a policy are driving the same environment rather than two that resemble
each other. The parts of the cell are not in here: the editor opens on its own
when the meter crosses a segment, and the simulation designs the next body.
Handing that decision to a policy is a second action space and a separate
piece of work.
"""

import gymnasium
import numpy as np

import pufferlib

from cpore_puffer import binding

DIMS = binding.dims()
OBS_DIM = DIMS["obs_dim"]
N_MOVE = DIMS["n_move"]

#: What each move index means, for anything that has to show or read one.
MOVES = ["drift", "N", "NE", "E", "SE", "S", "SW", "W", "NW"]

#: Renderer styles, in the order the C enum declares them. `drop` is the
#: continuous-tone one; the rest are the pixel-art pipeline.
STYLES = ["abyss", "dmg", "neon", "petri", "c64", "terra", "drop", "vista"]


class Cell(pufferlib.PufferEnv):
    """One microbe per env, in its own pond.

    `respawn` is the one flag worth understanding. Left on, being eaten costs
    a chunk of the meter and puts the cell back in the water somewhere else,
    and the episode runs until the meter fills or the clock stops - which is
    how the stage is actually played. Turned off, death ends the episode,
    which is the conventional RL framing and makes for a shorter, easier
    credit assignment problem at the cost of teaching an agent to hide.
    """

    def __init__(self, num_envs=1, render_mode=None, log_interval=128,
                 episode_len=DIMS["max_steps"], respawn=True, buf=None, seed=0):
        self.single_observation_space = gymnasium.spaces.Box(
            low=-np.inf, high=np.inf, shape=(OBS_DIM,), dtype=np.float32)
        self.single_action_space = gymnasium.spaces.MultiDiscrete([N_MOVE, 2, 2])
        self.render_mode = render_mode
        self.num_agents = num_envs

        super().__init__(buf)
        self.c_envs = binding.vec_init(
            self.observations, self.actions, self.rewards, self.terminals,
            self.truncations, num_envs, seed,
            episode_len=int(episode_len), respawn=int(bool(respawn)))
        self.episode_len = int(episode_len)
        self.respawn = bool(respawn)
        self._frame = None

    def reset(self, seed=0):
        binding.vec_reset(self.c_envs, seed)
        return self.observations, []

    def step(self, actions):
        self.actions[:] = actions
        binding.vec_step(self.c_envs)
        info = [binding.vec_log(self.c_envs)]
        return (self.observations, self.rewards,
                self.terminals, self.truncations, info)

    def greedy(self, out=None):
        """The scripted baseline's actions, in the learner's action space.

        A new environment has to answer one question before any training curve
        means anything: is a decent score reachable, and how far above random
        is it? Something that answered it while steering with a richer action
        space than the policy gets would not be answering it, so this projects
        onto the same nine directions.
        """
        if out is None:
            out = np.zeros((self.num_agents, 3), dtype=np.int32)
        binding.greedy(self.c_envs, out)
        return out

    def render(self, env_id=0, width=640, height=360, style="drop"):
        """One env's frame, as an (H, W, 4) uint8 array.

        Drawn by cpore's own renderer, which writes bytes rather than owning a
        window - so the same frame goes to a window, a PNG or a browser
        without the environment knowing which.
        """
        if (self._frame is None or self._frame.shape[0] != height
                or self._frame.shape[1] != width):
            self._frame = np.zeros((height, width, 4), dtype=np.uint8)
        s = STYLES.index(style) if isinstance(style, str) else int(style)
        binding.render_rgba(self.c_envs, int(env_id), self._frame, s)
        return self._frame

    def close(self):
        binding.vec_close(self.c_envs)


if __name__ == "__main__":
    import time

    N = 1024
    env = Cell(num_envs=N)
    env.reset()

    CACHE = 1024
    actions = np.stack([
        np.random.randint(0, N_MOVE, (CACHE, N)),
        np.random.randint(0, 2, (CACHE, N)),
        np.random.randint(0, 2, (CACHE, N)),
    ], axis=-1).astype(np.int32)

    steps = 0
    start = time.time()
    while time.time() - start < 5:
        env.step(actions[steps % CACHE])
        steps += 1
    print("Cell SPS:", int(env.num_agents * steps / (time.time() - start)))
    env.close()
