#!/usr/bin/env python3
"""Write the Mac app's update feed (Sparkle's appcast) for a release.

  scripts/update-appcast.py <mac.dmg> --tag <release-tag> --ed-signature <signature>
                            [--notes-html <html>] [--appcast <file>] [--url <download-url>]
  scripts/update-appcast.py --check [--appcast <file>]

The first form mounts the DMG (read-only, hdiutil), reads the app's Info.plist
(CFBundleShortVersionString, CFBundleVersion, PVZ2GameVersion,
LSMinimumSystemVersion) and writes sparkle/appcast.xml with that release as the
one update on offer: Sparkle only needs the newest. The signature is the
`sparkle:edSignature` value Sparkle's sign_update printed for the same DMG (the
key never comes near this script). The download link is the release asset's,
from the DMG's file name, unless --url gives another. --notes-html is what the
update window shows (scripts/release-notes.py --whats-new --format html), with a
link to the release under it.

`--check` only validates the feed and exits non-zero if something is off.
Python 3 standard library only.
"""

import argparse
import datetime
import email.utils
import html
import os
import plistlib
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

RELEASES = "https://github.com/oenderg/Reflourished-Darwin/releases"
DEFAULT_APPCAST = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sparkle", "appcast.xml"
)
SPARKLE = "http://www.andymatuschak.org/xml-namespaces/sparkle"


def read_app_plist(dmg):
    """Return the Info.plist of the one app in the disk image."""
    mount = tempfile.mkdtemp(prefix="appcast-")
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", mount, dmg],
                   check=True, stdout=subprocess.DEVNULL)
    try:
        apps = [n for n in os.listdir(mount) if n.endswith(".app")]
        if len(apps) != 1:
            sys.exit(f"{dmg}: expected one app, found {apps}")
        with open(os.path.join(mount, apps[0], "Contents", "Info.plist"), "rb") as f:
            return plistlib.load(f)
    finally:
        subprocess.run(["hdiutil", "detach", mount, "-quiet"], check=False)
        os.rmdir(mount)


def write(args):
    info = read_app_plist(args.dmg)
    version = info["CFBundleShortVersionString"]
    build = info["CFBundleVersion"]
    game = info.get("PVZ2GameVersion", "?")
    minimum = info.get("LSMinimumSystemVersion", "12.0")
    if not re.fullmatch(r"[A-Za-z0-9+/]{86}==", args.ed_signature):
        sys.exit("--ed-signature is not an EdDSA signature (sign_update prints it)")
    url = args.url or f"{RELEASES}/download/{args.tag}/{os.path.basename(args.dmg)}"
    page = f"{RELEASES}/tag/{args.tag}"
    notes = args.notes_html or f"<p>Reflourished-Darwin {html.escape(version)} · PvZ2: Reflourished {html.escape(game)}.</p>"
    if "]]>" in notes:
        sys.exit("--notes-html cannot contain ]]>")
    description = notes + f'<p><a href="{html.escape(page)}">The release on GitHub</a></p>'
    feed = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="{SPARKLE}">
  <channel>
    <title>PvZ2 Reflourished</title>
    <link>{RELEASES}</link>
    <description>Updates of PvZ2 Reflourished for the Mac (Reflourished-Darwin)</description>
    <language>en</language>
    <item>
      <title>Reflourished-Darwin {html.escape(version)} · PvZ2: Reflourished {html.escape(game)}</title>
      <pubDate>{email.utils.format_datetime(datetime.datetime.now(datetime.timezone.utc))}</pubDate>
      <link>{html.escape(page)}</link>
      <sparkle:version>{html.escape(build)}</sparkle:version>
      <sparkle:shortVersionString>{html.escape(version)}</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>{html.escape(minimum)}</sparkle:minimumSystemVersion>
      <description><![CDATA[{description}]]></description>
      <enclosure url="{html.escape(url)}" length="{os.path.getsize(args.dmg)}"
                 type="application/octet-stream" sparkle:edSignature="{args.ed_signature}"/>
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
        enclosure = item.find("enclosure")
        build = item.findtext(f"{{{SPARKLE}}}version", "")
        if enclosure is None or not enclosure.get("url", "").startswith("https://"):
            sys.exit(f"{path}: an item without an https enclosure")
        if not enclosure.get(f"{{{SPARKLE}}}edSignature") or not enclosure.get("length", "").isdigit():
            sys.exit(f"{path}: an enclosure without its signature or length")
        if not build.isdigit():
            sys.exit(f"{path}: sparkle:version must be the build number, got {build!r}")
    print(f"{path}: ok ({len(items)} item)")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("dmg", nargs="?")
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
    if not (args.dmg and args.tag and args.ed_signature):
        p.error("a DMG, --tag and --ed-signature are needed")
    write(args)


if __name__ == "__main__":
    main()
