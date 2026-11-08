#!/usr/bin/env python3
"""Drops DEPS entries that a Direct3D 11 renderer does not need.

gclient checks out every entry of ANGLE's DEPS, and that list carries what ANGLE's own
tests and its other backends need: Chromium's catapult, dawn, the LLVM sources,
SwiftShader and the Vulkan conformance suite. Removing them before "gclient sync" takes
most of the download, and a couple of gigabytes of tree, out of a build that only wants
libEGL and libGLESv2 over D3D11.

An entry is removed whole: the script counts the braces of its dict, so an entry that
grows or shrinks a line cannot leave a broken file behind. Entries listed in DEPS's
recursedeps are fine to drop; gclient only recurses into checkouts that exist.

    python3 trim-angle-deps.py DEPS third_party/dawn third_party/SwiftShader

An entry that is already gone is reported and skipped, which is what a second run over
a trimmed file does.
"""
import re
import sys


def _span(lines, name):
    """The (start, stop) line range of the 'name': {...} entry, or None if it is absent."""
    pattern = re.compile(r"^\s*'%s'\s*:\s*\{" % re.escape(name))
    for start, line in enumerate(lines):
        if pattern.match(line):
            break
    else:
        return None
    depth = 0
    for end in range(start, len(lines)):
        # Braces inside a string do not count: cipd package names carry '{{platform}}'.
        code = re.sub(r"'[^']*'", "''", lines[end])
        depth += code.count('{')
        depth -= code.count('}')
        if depth <= 0:
            break
    else:
        raise SystemExit('DEPS: the entry for %s does not close' % name)
    stop = end + 1
    if stop < len(lines) and not lines[stop].strip():
        stop += 1  # the blank line the entry leaves behind
    return start, stop


def main(argv):
    if len(argv) < 3:
        sys.stderr.write('usage: trim-angle-deps.py DEPS NAME [NAME ...]\n')
        return 2
    path, names = argv[1], argv[2:]
    # newline='' keeps the file's own line endings, whatever gclient gave it.
    with open(path, encoding='utf-8', newline='') as f:
        lines = f.readlines()
    for name in names:
        span = _span(lines, name)
        if span is None:
            print('    %s is not in DEPS' % name)
            continue
        del lines[span[0]:span[1]]
        print('    dropped %s' % name)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.writelines(lines)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
