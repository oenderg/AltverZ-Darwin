#!/usr/bin/env python3
"""Check that the converted libraries on disk are the ones the converter described.

The converter writes a `.facts` file and a `.manifest.json` beside every dylib it
produces. The manifest records the hash of the file it wrote and the file offset and
word of every patch it baked in, so the two can be checked against the dylib that is
actually on disk.

This matters because a build can be *interrupted* and still leave a dylib behind: the
converter writes the image, and only then patches it, so a converter that dies in
between leaves a complete, loadable, **unpatched** library -- one that faults on the
first `mrs tpidr_el0` it reaches, and whose facts and manifest are from an earlier,
successful run. The hash check catches that, and the patch check catches anything else.

    python3 scripts/check-build.py [aot-dir]
"""

import hashlib
import json
import os
import struct
import sys


def check(dylib, manifest_path):
    m = json.load(open(manifest_path))
    data = open(dylib, "rb").read()
    problems = []

    want = m.get("dylib_sha256")
    got = hashlib.sha256(data).hexdigest()
    if want and want != got:
        problems.append("the file on disk is not the one the manifest describes "
                        "(hash %s, the manifest says %s) -- the build was interrupted "
                        "or the file was replaced" % (got[:16], want[:16]))

    patches = m.get("patches", [])
    missing = 0
    for p in patches:
        off = p.get("file_off")
        if off is None:
            continue
        word = struct.unpack_from("<I", data, off)[0]
        if word != (p["word"] & 0xFFFFFFFF):
            missing += 1
    if missing:
        problems.append("%d of %d baked patches are not in the file" % (missing, len(patches)))

    return len(patches), problems


def main(argv):
    d = argv[1] if len(argv) > 1 else os.environ.get(
        "AOT_DIR", "aot")
    bad = 0
    for name in sorted(os.listdir(d)):
        if not name.endswith(".dylib.manifest.json"):
            continue
        lib = name[:-len(".manifest.json")]
        path = os.path.join(d, lib)
        if not os.path.exists(path):
            print("  %-28s MISSING" % lib)
            bad += 1
            continue
        n, problems = check(path, os.path.join(d, name))
        if problems:
            bad += 1
            print("  %-28s BAD (%d patches)" % (lib, n))
            for p in problems:
                print("      %s" % p)
        else:
            print("  %-28s ok (%d patches)" % (lib, n))
    if bad:
        print("%d librar%s not what the converter described" % (bad, "y is" if bad == 1 else "ies are"))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
