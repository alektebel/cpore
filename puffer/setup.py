"""Build the cell stage as a PufferLib C extension.

    pip install -e puffer/

The extension is cpore's own simulation - src/world.c and friends, compiled
straight in - plus PufferLib's binding layer, taken from the installed
PufferLib rather than vendored so the two cannot drift apart. Nothing here
downloads anything or needs a toolchain beyond the C compiler that already
built the rest of the project.
"""

import os
import sys

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def pufferlib_ocean():
    try:
        import pufferlib
    except ImportError:
        sys.exit("PufferLib is required to build this extension: pip install pufferlib")
    ocean = os.path.join(os.path.dirname(pufferlib.__file__), "ocean")
    if not os.path.exists(os.path.join(ocean, "env_binding.h")):
        sys.exit("This PufferLib has no ocean/env_binding.h; expected PufferLib >= 3.0")
    return ocean


def numpy_include():
    try:
        import numpy
    except ImportError:
        sys.exit("NumPy is required to build this extension")
    return numpy.get_include()


# Stage 1 only: the simulation, its genome and rng, and the two renderers the
# cell stage draws through. render3d.c is stage 2's and drags the whole
# aquatic simulation in behind it, which this environment has no use for.
SIM = ["src/world.c", "src/genome.c", "src/rng.c",
       "src/render.c", "src/render_cell.c", "src/render_pond.c",
       "src/policy.c"]

setup(
    name="cpore-puffer",
    version="0.1.0",
    description="cpore's stages as PufferLib environments",
    packages=["cpore_puffer"],
    package_dir={"cpore_puffer": "cpore_puffer"},
    ext_modules=[
        Extension(
            # PufferLib's binding layer names its module init PyInit_binding, so
            # the extension has to be called `binding` - one per package, which
            # is why each ocean environment is its own package.
            "cpore_puffer.binding",
            sources=[os.path.join(HERE, "binding.c")]
                    + [os.path.join(ROOT, s) for s in SIM],
            include_dirs=[
                os.path.join(ROOT, "include"),
                HERE,
                pufferlib_ocean(),
                numpy_include(),
            ],
            extra_compile_args=[
                "-O2", "-std=c99",
                "-DNPY_NO_DEPRECATED_API=NPY_1_7_API_VERSION",
                "-D_POSIX_C_SOURCE=200809L",
            ],
            libraries=["m"],
        )
    ],
    install_requires=["pufferlib>=3.0", "numpy<2"],
    python_requires=">=3.9",
)
