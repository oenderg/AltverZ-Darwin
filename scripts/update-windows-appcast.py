#!/usr/bin/env python3
"""Write the Windows update feed (WinSparkle's appcast) for a release.

  scripts/update-windows-appcast.py <setup.exe> --tag <release-tag> --ed-signature <signature>
                                    [--notes-html <html>] [--appcast <file>] [--url <download-url>]
  scripts/update-windows-appcast.py --check [--appcast <file>]

The first form writes sparkle/windows-appcast.xml with that release as the one update on
offer: WinSparkle, like Sparkle, only needs the newest. The setup's file name gives the
version (PvZ2-Reflourished-<game>-Darwin-<version>-Windows-Setup.exe, as build-installer.cmd names it) and
must match VERSION; the build number, which WinSparkle compares with the running app's, is
major*10000 + minor*100 + patch, as the app computes it (runtime/tools/pvz2-windows-updates.c).

The one setup installs either architecture, so the item has two enclosures with the same
file, one per `sparkle:os` (windows-x64, windows-arm64); `sparkle:os` and
`sparkle:installerArguments` are enclosure attributes in WinSparkle's parser. The signature
is the `sparkle:edSignature` that Sparkle's sign_update printed for the same file, with the
Mac app's key (docs/windows-shipping.md); the key never comes near this script.

`--check` only validates the feed and exits non-zero if something is off.
Python 3 standard library only.
"""

import argparse
import datetime
import email.utils
import html
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASES = "https://github.com/oenderg/Reflourished-Darwin/releases"
DEFAULT_APPCAST = os.path.join(ROOT, "sparkle", "windows-appcast.xml")
SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"
SETUP_NAME = re.compile(r"PvZ2-Reflourished-\d+\.\d+\.\d+-Darwin-(\d+)\.(\d+)\.(\d+)-Windows-Setup\.exe")
SYSTEMS = ("windows-x64", "windows-arm64")
INSTALLER_ARGUMENTS = "/SILENT /SP- /NOICONS"
MINIMUM_WINDOWS = "10.0"


def build_number(major, minor, patch):
    if minor >= 100 or patch >= 100:
        sys.exit("minor and patch must be below 100 for the build number formula")
    return major * 10000 + minor * 100 + patch


def write(args):
    name = os.path.basename(args.setup)
    m = SETUP_NAME.fullmatch(name)
    if not m:
        sys.exit(f"{name}: expected PvZ2-Reflourished-<game>-Darwin-<version>-Windows-Setup.exe")
    major, minor, patch = map(int, m.groups())
    version = f"{major}.{minor}.{patch}"
    with open(os.path.join(ROOT, "VERSION")) as f:
        repo_version = f.read().strip()
    if version != repo_version:
        sys.exit(f"{name} is version {version}, VERSION says {repo_version}")
    build = build_number(major, minor, patch)
    if not re.fullmatch(r"[A-Za-z0-9+/]{86}==", args.ed_signature):
        sys.exit("--ed-signature is not an EdDSA signature (sign_update prints it)")
    url = args.url or f"{RELEASES}/download/{args.tag}/{name}"
    page = f"{RELEASES}/tag/{args.tag}"
    notes = args.notes_html or f"<p>PvZ2 Reflourished {html.escape(version)} for Windows.</p>"
    if "]]>" in notes:
        sys.exit("--notes-html cannot contain ]]>")
    description = notes + f'<p><a href="{html.escape(page)}">The release on GitHub</a></p>'
    length = os.path.getsize(args.setup)
    enclosures = "\n".join(
        f'      <enclosure url="{html.escape(url)}" length="{length}" type="application/octet-stream"\n'
        f'                 sparkle:edSignature="{args.ed_signature}" sparkle:os="{system}"\n'
        f'                 sparkle:installerArguments="{INSTALLER_ARGUMENTS}"/>'
        for system in SYSTEMS
    )
    feed = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="{SPARKLE}">
  <channel>
    <title>PvZ2 Reflourished for Windows</title>
    <link>{RELEASES}</link>
    <description>Updates of PvZ2 Reflourished for Windows</description>
    <language>en</language>
    <item>
      <title>PvZ2 Reflourished {html.escape(version)} for Windows</title>
      <pubDate>{email.utils.format_datetime(datetime.datetime.now(datetime.timezone.utc))}</pubDate>
      <link>{html.escape(page)}</link>
      <sparkle:version>{build}</sparkle:version>
      <sparkle:shortVersionString>{html.escape(version)}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>{MINIMUM_WINDOWS}</sparkle:minimumSystemVersion>
      <description><![CDATA[{description}]]></description>
{enclosures}
    </item>
  </channel>
</rss>
"""
    os.makedirs(os.path.dirname(args.appcast), exist_ok=True)
    with open(args.appcast, "w") as f:
        f.write(feed)
    print(f"{args.appcast}: {version} (build {build}) -> {url}")


def check(path):
    tree = ET.parse(path)
    items = tree.getroot().findall("./channel/item")
    if not items:
        sys.exit(f"{path}: no item")
    for item in items:
        build = item.findtext(f"{{{SPARKLE}}}version", "")
        short = item.findtext(f"{{{SPARKLE}}}shortVersionString", "")
        if not build.isdigit():
            sys.exit(f"{path}: sparkle:version must be the build number, got {build!r}")
        parts = short.split(".")
        if len(parts) != 3 or not all(p.isdigit() for p in parts) or \
                build_number(*map(int, parts)) != int(build):
            sys.exit(f"{path}: sparkle:version {build} does not match the version {short!r}")
        enclosures = item.findall("enclosure")
        systems = sorted(e.get(f"{{{SPARKLE}}}os", "") for e in enclosures)
        if systems != sorted(SYSTEMS):
            sys.exit(f"{path}: expected one enclosure each for {', '.join(SYSTEMS)}, got {systems}")
        for e in enclosures:
            url = e.get("url", "")
            if not url.startswith("https://") or not SETUP_NAME.fullmatch(url.rsplit("/", 1)[-1]):
                sys.exit(f"{path}: an enclosure that is not an https link to a setup: {url!r}")
            if not re.fullmatch(r"[A-Za-z0-9+/]{86}==", e.get(f"{{{SPARKLE}}}edSignature", "")):
                sys.exit(f"{path}: an enclosure without its EdDSA signature")
            if not e.get("length", "").isdigit():
                sys.exit(f"{path}: an enclosure without its length")
            if e.get(f"{{{SPARKLE}}}installerArguments") != INSTALLER_ARGUMENTS:
                sys.exit(f"{path}: an enclosure without the silent setup arguments")
        if len({(e.get("url"), e.get("length"), e.get(f"{{{SPARKLE}}}edSignature")) for e in enclosures}) != 1:
            sys.exit(f"{path}: the two enclosures must name the same setup, length and signature")
    print(f"{path}: ok ({len(items)} item)")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("setup", nargs="?")
    p.add_argument("--tag")
    p.add_argument("--ed-signature")
    p.add_argument("--notes-html")
    p.add_argument("--url")
    p.add_argument("--appcast", default=DEFAULT_APPCAST)
    p.add_argument("--check", action="store_true")
    args = p.parse_args()
    if args.check:
        check(args.appcast)
        return
    if not (args.setup and args.tag and args.ed_signature):
        p.error("a setup, --tag and --ed-signature are needed")
    write(args)


if __name__ == "__main__":
    main()
