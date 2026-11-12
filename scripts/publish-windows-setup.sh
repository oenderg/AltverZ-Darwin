#!/bin/bash
# Sign a Windows setup, check the signature against the key the app carries, and write the
# Windows update feed (sparkle/windows-appcast.xml) for it. The release workflow runs it
# with the SPARKLE_PRIVATE_KEY secret (--key-file - on stdin); by hand it runs on the release
# Mac with the key from the login keychain, for a setup made outside the workflow.
#
#   scripts/publish-windows-setup.sh <PvZ2-Reflourished-<game>-Darwin-<version>-Windows-Setup.exe> --tag <tag>
#       [--key-file <file>]   the EdDSA private key file, or - for stdin (default: the Mac
#                             app's key in the login keychain, where sign_update looks by itself)
#       [--notes-html <html>] what the update window shows (default: the CHANGELOG's entry
#                             for VERSION, as the Mac feed has it)
#       [--public-key <file>] the public key to check against (default: sparkle/public-key,
#                             the one the app carries; only a test with a throwaway key changes it)
#       [--upload]            also attach the setup to the GitHub release <tag> (gh)
#       [--commit]            also commit the feed (chore(sparkle-windows): ...); never pushes
#
# The key is only ever read by sign_update: it is not printed, copied or logged.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
usage() { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

SETUP="" TAG="" KEY_FILE="" PUBLIC_KEY="$REPO/sparkle/public-key" NOTES="" UPLOAD=0 COMMIT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --tag) TAG="${2:?}"; shift 2 ;;
        --key-file) KEY_FILE="${2:?}"; shift 2 ;;
        --notes-html) NOTES="${2:?}"; shift 2 ;;
        --public-key) PUBLIC_KEY="${2:?}"; shift 2 ;;
        --upload) UPLOAD=1; shift ;;
        --commit) COMMIT=1; shift ;;
        -h|--help) usage ;;
        -*) echo "unknown option $1" >&2; usage ;;
        *) [ -z "$SETUP" ] || usage; SETUP="$1"; shift ;;
    esac
done
[ -n "$SETUP" ] && [ -n "$TAG" ] || usage
[ -f "$SETUP" ] || { echo "no file $SETUP" >&2; exit 1; }

SIGN="$REPO/out/.work/sparkle/bin/sign_update"
[ -x "$SIGN" ] || "$REPO/scripts/fetch-sparkle.sh" "$REPO/out/.work/sparkle"

if [ -n "$KEY_FILE" ]; then
    sig="$("$SIGN" --ed-key-file "$KEY_FILE" -p "$SETUP")"
else
    sig="$("$SIGN" -p "$SETUP")"
fi

# The signature must verify with the public key compiled into the Windows program, or every
# installed copy would refuse the update.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$PUBLIC_KEY" "$TMP/pub.pem" "$sig" "$TMP/sig.bin" <<'PY'
import base64, sys
raw = base64.b64decode(open(sys.argv[1]).read().strip())
if len(raw) != 32:
    sys.exit(sys.argv[1] + " is not a 32-byte Ed25519 key")
der = bytes.fromhex("302a300506032b6570032100") + raw
pem = "-----BEGIN PUBLIC KEY-----\n" + base64.b64encode(der).decode() + "\n-----END PUBLIC KEY-----\n"
open(sys.argv[2], "w").write(pem)
open(sys.argv[4], "wb").write(base64.b64decode(sys.argv[3]))
PY
# Ed25519 needs OpenSSL 3 or later (pkeyutl -rawin); macOS's own openssl is LibreSSL, and a
# GitHub runner's PATH finds that one first, so Homebrew's is taken when it is there.
OPENSSL=openssl
for o in /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl; do
    if [ -x "$o" ]; then OPENSSL="$o"; break; fi
done
if ! "$OPENSSL" pkeyutl -verify -pubin -inkey "$TMP/pub.pem" -rawin -in "$SETUP" -sigfile "$TMP/sig.bin" >/dev/null; then
    echo "the signature does not verify with $PUBLIC_KEY: wrong key?" >&2
    exit 1
fi
echo "signed: the signature verifies with $PUBLIC_KEY"

if [ -z "$NOTES" ]; then
    NOTES="$(python3 "$REPO/scripts/release-notes.py" --whats-new --version "$(cat "$REPO/VERSION")" --format html)"
fi
python3 "$REPO/scripts/update-windows-appcast.py" "$SETUP" --tag "$TAG" --ed-signature "$sig" --notes-html "$NOTES"
python3 "$REPO/scripts/update-windows-appcast.py" --check

if [ "$UPLOAD" = 1 ]; then
    gh release upload "$TAG" "$SETUP" --repo oenderg/Reflourished-Darwin
fi
if [ "$COMMIT" = 1 ]; then
    git -C "$REPO" add sparkle/windows-appcast.xml
    git -C "$REPO" diff --cached --quiet || git -C "$REPO" commit -m "chore(sparkle-windows): update the Windows update feed for $TAG"
fi
