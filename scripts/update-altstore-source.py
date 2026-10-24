#!/usr/bin/env python3
"""Add a release of PvZ2 Reflourished to the AltStore / SideStore source.

  scripts/update-altstore-source.py <ipa> --tag <release-tag> [--changelog <text>]
  scripts/update-altstore-source.py --check

The first form reads the IPA's Info.plist (CFBundleShortVersionString,
CFBundleVersion, CFBundleIdentifier, MinimumOSVersion), the IPA's size and
today's date, and inserts that version at the top of `versions` in
altstore/source.json, keeping the older versions and leaving no duplicates for
the same version string. The app entry's bundleIdentifier is synced to the
IPA's, so a renamed bundle id in a build heals the source by itself.

`--check` only validates the JSON shape of the source and exits non-zero if
something is off. Python 3 standard library only.
"""

import argparse
import datetime
import json
import os
import plistlib
import re
import sys
import zipfile

RELEASES = "https://github.com/oenderg/Reflourished-Darwin/releases/download"
DEFAULT_SOURCE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "altstore", "source.json"
)

HTTPS = re.compile(r"^https://\S+$")
DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def read_info_plist(ipa):
    """Return the Info.plist of the one app bundle in the IPA."""
    with zipfile.ZipFile(ipa) as z:
        names = [n for n in z.namelist() if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", n)]
        if not names:
            sys.exit(f"error: {ipa} has no Payload/*.app/Info.plist")
        return plistlib.loads(z.read(names[0]))


def add_version(source, ipa, tag, changelog):
    info = read_info_plist(ipa)
    version = info.get("CFBundleShortVersionString") or info.get("CFBundleVersion")
    if not version:
        sys.exit("error: the IPA's Info.plist has neither CFBundleShortVersionString nor CFBundleVersion")
    bundle_id = info.get("CFBundleIdentifier")
    if not bundle_id:
        sys.exit("error: the IPA's Info.plist has no CFBundleIdentifier")

    if not changelog:
        changelog = (f"Reflourished-Darwin {version} · PvZ2: Reflourished "
                     f"{info.get('PVZ2GameVersion', '?')}")

    new = {
        "version": str(version),
        "buildVersion": str(info.get("CFBundleVersion", "")),
        "date": datetime.date.today().isoformat(),
        "localizedDescription": changelog,
        "downloadURL": f"{RELEASES}/{tag}/{os.path.basename(ipa)}",
        "size": os.path.getsize(ipa),
        "minOSVersion": str(info.get("MinimumOSVersion", "12.0")),
    }

    apps = source.get("apps") or []
    app = next((a for a in apps if a.get("bundleIdentifier") == bundle_id), None)
    if app is None and len(apps) == 1:
        app = apps[0]
        print(f"note: the app entry's bundleIdentifier "
              f"{app.get('bundleIdentifier')!r} differs from the IPA's {bundle_id!r}; syncing it")
    if app is None:
        sys.exit(f"error: no app entry for {bundle_id} in the source (and not exactly one app to "
                 f"assume); add the entry to source.json by hand")

    old = app.get("versions") or []
    kept = [v for v in old if v.get("version") != new["version"]]
    app["bundleIdentifier"] = bundle_id
    app["versions"] = [new] + kept

    print(f"added {new['version']} (build {new['buildVersion']}, {new['size']} bytes, tag {tag!r}) "
          f"to {app.get('name')}: {len(kept)} older version(s) kept, "
          f"{len(old) - len(kept)} replaced")


def validate(source):
    errors = []

    def check_string(obj, key, where, required=True):
        v = obj.get(key)
        if not isinstance(v, str) or (required and not v):
            errors.append(f"{where}: {key} must be a non-empty string, got {v!r}")

    if not isinstance(source, dict):
        return ["the top level must be a JSON object"]

    for key in ("name", "identifier"):
        check_string(source, key, "source")
    for key in ("subtitle", "description", "tintColor"):
        check_string(source, key, "source", required=False)
    for key in ("iconURL", "website"):
        if not HTTPS.match(str(source.get(key, ""))):
            errors.append(f"source: {key} must be an https URL, got {source.get(key)!r}")
    if not isinstance(source.get("news", []), list):
        errors.append("source: news must be a list")

    apps = source.get("apps")
    if not isinstance(apps, list) or not apps:
        errors.append("source: apps must be a non-empty list")
        apps = []
    for i, app in enumerate(apps):
        where = f"apps[{i}]"
        if not isinstance(app, dict):
            errors.append(f"{where}: must be an object")
            continue
        for key in ("name", "bundleIdentifier", "developerName"):
            check_string(app, key, where)
        for key in ("subtitle", "localizedDescription", "tintColor", "category"):
            check_string(app, key, where, required=False)
        for key in ("developerWebsite", "iconURL"):
            if key in app and not HTTPS.match(str(app[key])):
                errors.append(f"{where}: {key} must be an https URL, got {app[key]!r}")
        for key in ("screenshots", "news"):
            if not isinstance(app.get(key, []), list):
                errors.append(f"{where}: {key} must be a list")

        versions = app.get("versions")
        if not isinstance(versions, list):
            errors.append(f"{where}: versions must be a list (empty until the first release)")
            versions = []
        for j, v in enumerate(versions):
            vw = f"{where}.versions[{j}]"
            if not isinstance(v, dict):
                errors.append(f"{vw}: must be an object")
                continue
            for key in ("version", "downloadURL", "minOSVersion"):
                if not v.get(key):
                    errors.append(f"{vw}: missing {key}")
            if not HTTPS.match(str(v.get("downloadURL", ""))):
                errors.append(f"{vw}: downloadURL must be an https URL, got {v.get('downloadURL')!r}")
            if not DATE.match(str(v.get("date", ""))):
                errors.append(f"{vw}: date must be YYYY-MM-DD, got {v.get('date')!r}")
            size = v.get("size")
            if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
                errors.append(f"{vw}: size must be a positive integer, got {size!r}")
            for key in ("buildVersion", "localizedDescription"):
                if key in v and not isinstance(v[key], str):
                    errors.append(f"{vw}: {key} must be a string, got {v[key]!r}")

        perms = app.get("appPermissions")
        if perms is not None:
            if not isinstance(perms, dict):
                errors.append(f"{where}: appPermissions must be an object")
            else:
                for key in ("entitlements", "privacy"):
                    items = perms.get(key)
                    if items is None:
                        continue
                    if not isinstance(items, list):
                        errors.append(f"{where}: appPermissions.{key} must be a list")
                        continue
                    for k, item in enumerate(items):
                        if not isinstance(item, dict) or not item.get("name"):
                            errors.append(f"{where}: appPermissions.{key}[{k}] must have a name")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ipa", nargs="?", help="the .ipa to add to the source")
    parser.add_argument("--tag", help="the GitHub release tag the IPA is published under")
    parser.add_argument("--changelog", default="",
                        help="text for the version's localizedDescription")
    parser.add_argument("--icon-url",
                        help="where the app's icon is (default: the one in the source, docs/banner/icon.png on main)")
    parser.add_argument("--source", default=DEFAULT_SOURCE,
                        help=f"path to altstore/source.json (default: {DEFAULT_SOURCE})")
    parser.add_argument("--check", action="store_true",
                        help="only validate the JSON shape of the source, then exit")
    args = parser.parse_args()

    if args.check:
        if args.ipa or args.tag:
            parser.error("--check takes no IPA and no tag")
        try:
            with open(args.source, encoding="utf-8") as f:
                source = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            sys.exit(f"error: cannot read {args.source}: {e}")
        errors = validate(source)
        if errors:
            for e in errors:
                print(f"error: {e}", file=sys.stderr)
            sys.exit(1)
        total = sum(len(a.get("versions") or []) for a in source.get("apps") or [])
        print(f"OK: {args.source} is a valid AltStore source "
              f"({len(source.get('apps') or [])} app(s), {total} version(s))")
        return

    if not args.ipa or not args.tag:
        parser.error("an IPA and --tag are required (or use --check)")

    try:
        with open(args.source, encoding="utf-8") as f:
            source = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        sys.exit(f"error: cannot read {args.source}: {e}")

    add_version(source, args.ipa, args.tag, args.changelog)
    if args.icon_url:
        source["iconURL"] = args.icon_url
        for app in source.get("apps") or []:
            app["iconURL"] = args.icon_url

    errors = validate(source)
    if errors:
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        sys.exit(f"error: the updated source would be invalid; {args.source} was NOT changed")

    with open(args.source, "w", encoding="utf-8") as f:
        json.dump(source, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"wrote {args.source}")


if __name__ == "__main__":
    main()
