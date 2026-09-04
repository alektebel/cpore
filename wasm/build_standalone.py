#!/usr/bin/env python3
"""Fold the editor into one file that needs no server.

editor.html is ES modules fetching a .wasm, which is the right shape for
development and the wrong shape for handing to someone: it needs an origin, so
opening it from disk fails and there is nothing to send but a directory. This
inlines the module and base64s the WebAssembly into a single document that
runs from a file:// URL, an email attachment or a hosted page with a strict
content policy that blocks every outbound request.

It composes from the real files rather than keeping a second copy of the
editor, so there is one implementation and this is a packaging step.

    python3 wasm/build_standalone.py [out.html]
    python3 wasm/build_standalone.py --fragment [out.html]

--fragment strips the document tags as well, for hosts that supply their own
doctype, head and body and expect only the content.
"""

import base64
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main(out_path, fragment=False):
    with open(os.path.join(HERE, "cpore.wasm"), "rb") as f:
        wasm_b64 = base64.b64encode(f.read()).decode()

    with open(os.path.join(HERE, "cpore-edit.js")) as f:
        module = f.read()

    with open(os.path.join(HERE, "editor.html")) as f:
        page = f.read()

    # The module becomes plain script: no exports, no default.
    module = re.sub(r"^export default .*$", "", module, flags=re.M)
    module = module.replace("export const ", "const ").replace("export class ", "class ")

    # And the page stops importing it.
    page = re.sub(
        r"import \{[^}]*\}\s*\n?\s*from '\./cpore-edit\.js';",
        "/* cpore-edit.js, inlined below */",
        page,
    )

    boot = (
        "const WASM_B64 = '%s';\n"
        "const _bin = Uint8Array.from(atob(WASM_B64), c => c.charCodeAt(0));\n" % wasm_b64
    )
    page = page.replace(
        "await CreatureEditor.create('./cpore.wasm',",
        "await CreatureEditor.create(_bin.buffer,",
    )

    page = page.replace(
        "<script type=\"module\">",
        "<script type=\"module\">\n" + boot + "\n" + module + "\n",
        1,
    )

    if fragment:
        # Keep the title, the style and the markup; drop the document tags the
        # host supplies. `<header>` contains the substring "<head", so this
        # matches tags rather than text.
        title = re.search(r"<title>.*?</title>", page, re.S).group(0)
        style = re.search(r"<style>.*?</style>", page, re.S).group(0)
        inner = re.search(r"<body>(.*?)</body>", page, re.S).group(1)
        page = title + "\n" + style + "\n" + inner.strip() + "\n"
        for tag in ("html", "head", "body"):
            assert not re.search(r"</?%s[\s>]" % tag, page, re.I), tag
    assert not re.search(r"https?://", page), "must make no outbound request"

    with open(out_path, "w") as f:
        f.write(page)
    kb = os.path.getsize(out_path) / 1024.0
    print("wrote %s  (%.0f KB, no external requests)" % (out_path, kb))


if __name__ == "__main__":
    args = sys.argv[1:]
    frag = "--fragment" in args
    args = [a for a in args if a != "--fragment"]
    default = "editor-fragment.html" if frag else "editor-standalone.html"
    main(args[0] if args else os.path.join(HERE, default), frag)
