#!/bin/bash
# Run the Mac app as a test: its own data folder, a scripted tap sequence, window captures
# at given seconds, then it is closed. Your own game data and settings are never touched,
# no input goes anywhere but the test app, and nothing takes the focus from what you are doing.
#
#   scripts/test-run.sh [--app <PvZ2 AltVerZ.app>] [--save <data folder to copy>]
#                       [--tap "<script>"] [--seconds N] [--shots "t1 t2 ..."] [--screen N]
#                       [--out <folder>] [-- <extra launch arguments>]
#
#   --app      default: out/PvZ2 AltVerZ.app
#   --save     a data folder to start from (copied, never changed); default: an empty one,
#              which means the first-launch screens (and, for a slim build, the OBB download)
#   --tap      the tap script, see docs/TESTING.md; default: just wait
#   --seconds  how long the run lasts (default 40)
#   --shots    seconds after the start at which the window is captured (shot-<t>.png)
#   --screen   start on display N (0 = the main display)
#   --out      where the run goes (default: out/runs/<time>); it holds run.log, the shots, data/
#   after --    launch arguments for the app, e.g. -resizeMode Fixed -fixedAspect game
#
# Prints the run folder. Exit 0 if the app was still running at the end, 1 if it exited
# (look for "CRASH" in run.log).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/out/PvZ2 AltVerZ.app" SAVE="" TAP="wait:600" SECS=40 SHOTS="" SCREEN="" RUN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --app) APP="$2"; shift 2 ;;     --save) SAVE="$2"; shift 2 ;;
        --tap) TAP="$2"; shift 2 ;;     --seconds) SECS="$2"; shift 2 ;;
        --shots) SHOTS="$2"; shift 2 ;; --screen) SCREEN="$2"; shift 2 ;;
        --out) RUN="$2"; shift 2 ;;     --) shift; break ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$APP/Contents/MacOS/PvZ2" ] || { echo "no app at $APP (build it: ./build.sh --mac ...)" >&2; exit 2; }
# A locked login session still runs the app, but the window server releases its
# drawables at about 10-15 fps, so the fps lines in the log fall from the game's
# rate to the locked screen's rate. Say so, or the next reader looks for a game
# bug that is not there.
if ioreg -n Root -d1 -a 2>/dev/null | grep -A1 CGSSessionScreenIsLocked | grep -q '<true/>'; then
    echo "note: the login session is locked; the window server throttles the test window to about 10-15 fps" >&2
fi
RUN="${RUN:-$ROOT/out/runs/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$RUN/data"
[ -n "$SAVE" ] && cp -R "$SAVE/." "$RUN/data/"

# The window of one process, by its pid (CoreGraphics, through a one-line Swift program).
window_of() {
    swift -e 'import CoreGraphics; let pid = Int(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
for w in list where (w[kCGWindowOwnerPID as String] as? Int) == pid && (w[kCGWindowLayer as String] as? Int) == 0 {
    if let n = w[kCGWindowNumber as String] as? Int { print(n); break } }' "$1" 2>/dev/null
}

env TL_DATA="$RUN/data" TL_PVZ2_LOG="$RUN/run.log" TL_PVZ2_TAP="$TAP" TL_PVZ2_TAP_DELAY=1 \
    TL_PVZ2_QUIET=1 TL_KEEP_RUNNING_IN_BACKGROUND=1 TL_AUDIO_MUTE=1 \
    ${SCREEN:+TL_PVZ2_TEST_SCREEN=$SCREEN} \
    "$APP/Contents/MacOS/PvZ2" "$@" > "$RUN/run.out" 2>&1 &
PID=$!
START=$(date +%s)
for t in $SHOTS; do
    while [ $(( $(date +%s) - START )) -lt "$t" ]; do sleep 0.5; done
    WID="$(window_of $PID)"
    [ -n "$WID" ] && screencapture -x -o -l "$WID" "$RUN/shot-$t.png"
done
while [ $(( $(date +%s) - START )) -lt "$SECS" ] && kill -0 $PID 2>/dev/null; do sleep 1; done
if kill -0 $PID 2>/dev/null; then
    kill $PID; rc=0
else
    echo "the app exited by itself; see $RUN/run.log" >&2; rc=1
fi
echo "$RUN"
exit $rc
