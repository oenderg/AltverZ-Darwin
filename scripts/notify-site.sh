#!/bin/bash
# Ask the website (pvz.oender.dev, the repository oenderg/pvz-ports-site) to rebuild now, because something it shows
# changed here: a release, the AltStore source or the app icon. The site's deploy workflow takes the "port-release"
# repository_dispatch; without one it still rebuilds once a day.
#
#   scripts/notify-site.sh <why>
#
# Needs SITE_DISPATCH_TOKEN: a fine-grained token for oenderg/pvz-ports-site only, Contents read and write (a
# repository secret; a run's own GITHUB_TOKEN cannot reach another repository). Without it nothing is sent and the
# script passes, so forks and test runs are not held up. GITHUB_REPOSITORY (the Actions environment) names the sender.
set -euo pipefail

why="${1:-}"
site="oenderg/pvz-ports-site"

if [ -z "${SITE_DISPATCH_TOKEN:-}" ]; then
    echo "notify-site: no SITE_DISPATCH_TOKEN, so the website is not told (it rebuilds once a day anyway)"
    exit 0
fi

body="$(python3 -c 'import json, sys; print(json.dumps({"event_type": "port-release",
    "client_payload": {"repo": sys.argv[1], "why": sys.argv[2]}}))' "${GITHUB_REPOSITORY:-}" "$why")"
out="$(mktemp)"
code="$(curl -sS -o "$out" -w '%{http_code}' -X POST \
    -H "Authorization: Bearer $SITE_DISPATCH_TOKEN" \
    -H "Accept: application/vnd.github+json" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    "https://api.github.com/repos/$site/dispatches" -d "$body")" || code="no answer"
if [ "$code" != 204 ]; then
    echo "notify-site: $site did not take the message (HTTP $code): $(cat "$out")" >&2
    rm -f "$out"
    exit 1
fi
rm -f "$out"
echo "notify-site: told $site to rebuild ($why)"
