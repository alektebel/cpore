"""cpore's stages as PufferLib environments.

One stage so far. `cell` is the microbe stage: a pond, food that is either
plant or meat, a body you design out of parts you paid for, and other cells
that are variously lunch, rivals or the thing that eats you.
"""

__all__ = ["Cell"]


def __getattr__(name):
    # Imported lazily so that `import cpore_puffer` does not pull in numpy,
    # gymnasium and the extension for a caller that only wanted the version.
    if name == "Cell":
        from cpore_puffer.cell import Cell
        return Cell
    raise AttributeError(name)
