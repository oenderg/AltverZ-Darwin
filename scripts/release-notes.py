#!/usr/bin/env python3
"""Put a release's notes together (release-notes/README.md says what goes in them).

  scripts/release-notes.py <release-dir> --version <v> --tag <tag> [--since <ref>] [--repo <owner/name>]
  scripts/release-notes.py --whats-new --version <v> [--format md|text|html]

The first form prints the release page's Markdown: release-notes/<version>.md as
What's new, the feat/fix/perf commits since the previous tag (or --since) folded
under All changes, and release-notes/footer.md with a table of the files in the
release directory. The second form prints only What's new: as plain text for the
AltStore / SideStore changelog, or as HTML for the Mac update window.
Python 3 standard library only.
"""

import argparse
import html
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOTES = os.path.join(ROOT, "release-notes")
GROUPS = [("feat", "Features"), ("fix", "Fixes"), ("perf", "Performance")]
TITLE = re.compile(r"^(\w+)(?:\(([^)]+)\))?!?: (.+)$")
# What each release file is, by the end of its name
KINDS = [("-iOS.ipa", "iPhone / iPad"), ("-iOS-Full.ipa", "iPhone / iPad, game data included"),
         ("-Mac.dmg", "Mac (Apple silicon)"), ("-Mac-Full.dmg", "Mac, game data included")]


def whats_new(version):
    path = os.path.join(NOTES, f"{version}.md")
    if not os.path.isfile(path):
        sys.exit(f"no release notes for {version}: write {os.path.relpath(path, ROOT)} first "
                 "(release-notes/README.md)")
    return open(path).read().strip()


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True).stdout.strip()


def all_changes(tag, since, repo):
    """The feat/fix/perf commits since the previous tag, grouped; None for the first release."""
    if not since:
        since = git("describe", "--tags", "--abbrev=0", "--exclude", tag, "HEAD")
    if not since:
        return None
    groups = {kind: [] for kind, _ in GROUPS}
    for line in git("log", "--no-merges", "--format=%h\t%s", f"{since}..HEAD").splitlines():
        sha, subject = line.split("\t", 1)
        m = TITLE.match(subject)
        if m and m.group(1) in groups:
            scope = f"{m.group(2)}: " if m.group(2) else ""
            groups[m.group(1)].append(f"- {scope}{m.group(3)} ({sha})")
    parts = [f"**{name}**\n" + "\n".join(groups[kind]) for kind, name in GROUPS if groups[kind]]
    parts.append(f"**Full changelog:** [{since}...{tag}](https://github.com/{repo}/compare/{since}...{tag})")
    return "<details>\n<summary>All changes</summary>\n\n" + "\n\n".join(parts) + "\n\n</details>"


def size(n):
    return f"{n / 1e9:.1f} GB" if n >= 1e9 else f"{round(n / 1e6)} MB"


def downloads(release_dir):
    rows = []
    for suffix, what in KINDS:
        for name in sorted(os.listdir(release_dir)):
            if name.endswith(suffix):
                rows.append(f"| {what} | `{name}` | {size(os.path.getsize(os.path.join(release_dir, name)))} |")
    if not rows:
        sys.exit(f"no IPA or DMG in {release_dir}")
    return "| Device | File | Size |\n|---|---|---|\n" + "\n".join(rows)


def page(args):
    footer = open(os.path.join(NOTES, "footer.md")).read().strip()
    footer = footer.replace("{downloads}", downloads(args.release_dir)).replace("{repo}", args.repo)
    parts = ["## What's new", whats_new(args.version)]
    changes = all_changes(args.tag, args.since, args.repo)
    if changes:
        parts.append(changes)
    parts.append(footer)
    return "\n\n".join(parts) + "\n"


def inline_html(text):
    text = html.escape(text, quote=False)
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"`(.+?)`", r"<code>\1</code>", text)
    return re.sub(r"\[(.+?)\]\((https://[^)\s]+)\)", r'<a href="\2">\1</a>', text)


def as_html(md):
    """What's new as the simple HTML the Mac update window shows: paragraphs and bullet lists."""
    out, items = [], []
    for block in re.split(r"\n\s*\n", md):
        lines = [l.strip() for l in block.strip().splitlines() if l.strip()]
        if lines and all(l.startswith("- ") for l in lines):
            out.append("<ul>" + "".join(f"<li>{inline_html(l[2:])}</li>" for l in lines) + "</ul>")
        elif lines:
            out.append(f"<p>{inline_html(' '.join(lines))}</p>")
    return "".join(out)


def as_text(md):
    """What's new as plain text (AltStore shows the changelog without Markdown)."""
    text = re.sub(r"\*\*(.+?)\*\*", r"\1", md)
    text = re.sub(r"`(.+?)`", r"\1", text)
    text = re.sub(r"\[(.+?)\]\((\S+?)\)", r"\1 (\2)", text)
    return re.sub(r"^- ", "• ", text, flags=re.M)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("release_dir", nargs="?")
    p.add_argument("--version", required=True)
    p.add_argument("--tag")
    p.add_argument("--since", help="the previous release's tag or commit (default: the newest tag before --tag)")
    p.add_argument("--repo", default="oenderg/Reflourished-Darwin")
    p.add_argument("--whats-new", action="store_true")
    p.add_argument("--format", choices=["md", "text", "html"], default="md")
    args = p.parse_args()
    if args.whats_new:
        md = whats_new(args.version)
        print({"md": md, "text": as_text(md), "html": as_html(md)}[args.format])
        return
    if not (args.release_dir and args.tag):
        p.error("a release directory and --tag are needed")
    sys.stdout.write(page(args))


if __name__ == "__main__":
    main()
