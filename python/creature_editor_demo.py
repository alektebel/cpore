"""Build a creature by clicking on it.

Every edit below is made in pixel coordinates, the way a mouse would make it.
Nothing here knows what a genome looks like: it asks the viewport where the
body is, drops parts there, drags a vertebra, paints the result, and hands the
finished animal to the simulation.

That is the whole point of the studio underneath. The renderer, the picker and
the surface query are the same ray march, so a part dropped on a pixel lands
on the surface that pixel showed - which is the one promise a creature editor
has to keep, and the one that is impossible to keep if picking gets its own
idea of where the body is.

    python3 python/creature_editor_demo.py [out_prefix]
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))

from cpore.env import CreatureEditor, LandEnv  # noqa: E402

SIZE = 560


def body_pixels(ed, step, top=0.0, bottom=1.0):
    """Walk the viewport and yield the pixels that are over the animal.

    A front end does not do this - it has a pointer - but a script has to find
    the body somehow, and asking the same question a hover cursor asks is the
    honest way to do it.
    """
    for y in range(int(SIZE * top) + 40, int(SIZE * bottom) - 40, step):
        for x in range(60, SIZE - 60, step):
            if ed.surface(x, y) is not None:
                yield x, y


def main(prefix="creature"):
    ed = CreatureEditor(SIZE, SIZE)

    # A jaw on a five-vertebra spine with a bit of arch in it, and nothing else.
    ed.load({"parts": [(2, 0, 0, 0, 128, 0, 128, 0)], "nseg": 5, "girth": 175})
    ed.spine(arch=45)
    print("start: %d of %d DNA" % (ed.cost, ed.budget))

    # Drop parts where the body is. Legs go on the underside, plates and horns
    # on top - which is the only thing this script knows about anatomy, and it
    # expresses it as a band of the screen rather than as a gene.
    plan = (("leg",   3, 40, 0.56, 1.00, dict(length=215, bend=75)),
            ("plate", 4, 26, 0.00, 0.44, {}),
            ("horn",  2, 40, 0.00, 0.50, dict(scale=190)),
            ("eye",   2, 55, 0.10, 0.50, {}),
            ("tail",  1, 70, 0.30, 0.70, dict(length=230)))

    for part, cap, step, top, bottom, shape in plan:
        placed = 0
        for x, y in body_pixels(ed, step, top, bottom):
            if placed >= cap or not ed.can_afford(part, mirror=1):
                break
            slot = ed.drop(x, y, part)      # mirror decided from where it lands
            if slot is None:                # off the body, or unaffordable
                continue
            if shape:
                ed.shape(slot, **shape)
            placed += 1
        print("  dropped %d x %s   (%d DNA)" % (placed, part, ed.cost))

    # Pull a vertebra up into a hump, and fatten it.
    for y in range(0, SIZE, 4):
        hump = None
        for x in range(0, SIZE, 4):
            v = ed.spine_pick(x, y, grab=9.0)
            if v is not None and 0 < v < 4:
                ed.spine_drag(v, x, y - 60)
                ed.spine_girth(v, 0.55)
                hump = v
                break
        if hump is not None:
            print("  humped vertebra %d" % hump)
            break

    # Paint. Three coats, which until now nothing outside the genome could set.
    ed.paint(hue=28, hue2=145, hue3=205, sat=220, val=210)
    ed.coats(pattern="bands", scale=105, pattern2="spots", scale2=215)

    st = ed.stats()
    print("final: %d of %d DNA, %d parts" % (ed.cost, ed.budget, len(ed.parts())))
    print("       speed %.0f  hp %.0f  bite %.0f  jump %.0f  charm %.2f"
          % (st["speed"], st["hp"], st["bite"], st["jump"], st["charm"]))

    for i, az in enumerate((2.36, 3.75, 1.15)):
        ed.view(azimuth=az, elev=0.22)
        path = "%s_%d.png" % (prefix, i)
        ed.save_png(path, quality=3)
        print("  wrote %s" % path)

    # And the finished animal goes straight into the world it was built for.
    finished = ed.finish()
    env = LandEnv(seed=3, genome=finished)
    env.reset()
    total, steps, done = 0.0, 0, "running"
    for _ in range(600):
        _, r, term, trunc, _ = env.step(env.greedy_action())
        total += r
        steps += 1
        if term or trunc:
            done = "terminated" if term else "truncated"
            break
    print("test drive: %d steps, return %.1f, %s" % (steps, total, done))
    env.close()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "creature")
