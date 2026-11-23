#!/usr/bin/env bash
#
# iOS Brother in Arms 3D smoke/perf waypoint.
#
# Drives the booted simulator to a real 3D gameplay frame and saves screenshots
# under the results directory. This is intentionally separate from the broad
# Final Battle/Calculator regression script because BIA has a longer title/menu
# path and is useful as a repeatable FPS checkpoint.
#
# Requirements: a booted iPhone simulator with EKA2L1 installed, the rm-409
# device mounted, Brother in Arms installed, plus `xcodebuildmcp`, `jq`, `magick`
# and `xcrun` on PATH. It does NOT build. Pass --install <app.app> to install a
# freshly built app before launching.
#
# Usage:
#   scripts/ios_bia_gameplay_test.sh
#   scripts/ios_bia_gameplay_test.sh --install build/ios-simulator/src/emu/ios/Release-iphonesimulator/EKA2L1.app
#
# Env overrides:
#   EKA2L1_BUNDLE_ID       default com.eka2l1.emulator
#   EKA2L1_BIA_OUTDIR      default /tmp/eka2l1-bia-gameplay
#   EKA2L1_BIA_ROM         default rm-409
#   EKA2L1_BIA_UID         default 0x20004380
#   EKA2L1_BIA_MENU_WAIT   wait after title/menu inputs (default 5)
#   EKA2L1_BIA_GAME_WAIT   wait after Continue/skip before final screenshot (default 10)

set -uo pipefail

BUNDLE_ID="${EKA2L1_BUNDLE_ID:-com.eka2l1.emulator}"
OUTDIR="${EKA2L1_BIA_OUTDIR:-/tmp/eka2l1-bia-gameplay}"
BIA_ROM="${EKA2L1_BIA_ROM:-rm-409}"
BIA_UID="${EKA2L1_BIA_UID:-0x20004380}"
MENU_WAIT="${EKA2L1_BIA_MENU_WAIT:-5}"
GAME_WAIT="${EKA2L1_BIA_GAME_WAIT:-10}"

SCREEN_DIFF_MIN="${EKA2L1_BIA_SCREEN_DIFF_MIN:-4000}"
BLANK_STDEV_MAX="0.04"
CRASH_REGEX='Active scheduler dump|E32USER-CBase|panicked|access violation|Emulation halt|KERN-EXEC|Unhandled'

PASS=0; FAIL=0
declare -a RESULTS

INSTALL_APP=""
if [ "${1:-}" = "--install" ]; then
    INSTALL_APP="${2:-}"; shift 2
fi

die() { echo "FATAL: $*" >&2; exit 2; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing tool: $1"; }

booted_sim() {
    xcrun simctl list devices booted 2>/dev/null \
        | sed -n 's/.*(\([0-9A-Fa-f-]\{36\}\)) (Booted).*/\1/p' | head -1
}

log_path() {
    local data; data="$(xcrun simctl get_app_container "$SIM" "$BUNDLE_ID" data 2>/dev/null)"
    [ -n "$data" ] && echo "$data/Documents/data/EKA2L1.log"
}

wait_s() { local e=$((SECONDS+$1)); until [ $SECONDS -ge $e ]; do sleep 1; done; }

SNAP="$OUTDIR/.snapshot.json"
snapshot() {
    xcodebuildmcp simulator snapshot-ui --simulator-id "$SIM" --output json 2>/dev/null >"$SNAP" || true
}

ref_for() {
    local want="$1" mode="${2:-exact}"
    jq -r '.data.capture.targets[]' "$SNAP" 2>/dev/null | awk -F'|' -v w="$want" -v m="$mode" '
        { lbl=$4 }
        m=="exact"  && lbl==w        { print $1; exit }
        m=="prefix" && index(lbl,w)==1 { print $1; exit }'
}

# On Xcode 27 dtuhidd activates its virtual touchscreen service only once it has
# a peer and drops whatever arrives before that, so a tap whose process exits
# immediately is reported as SUCCEEDED and never reaches the guest.
TAP_POST_DELAY="${EKA2L1_REG_TAP_POST_DELAY:-0.6}"

tap_ref() {
    local ref="$1" i r
    [ -z "$ref" ] && return 1
    for i in 1 2 3 4 5 6; do
        r="$(xcodebuildmcp ui-automation tap --simulator-id "$SIM" --element-ref "$ref" \
             --post-delay "$TAP_POST_DELAY" --output json 2>/dev/null \
             | grep -o '"status": "[A-Z]*"' | head -1)"
        echo "$r" | grep -q SUCCEEDED && return 0
        sleep 1
    done
    return 1
}

tap_label() {
    snapshot
    local ref; ref="$(ref_for "$1" "${2:-exact}")"
    [ -z "$ref" ] && { echo "    ! no element for label '$1'" >&2; return 1; }
    tap_ref "$ref"
}

shot() {
    local name="$1"
    local path="$OUTDIR/$name.png"
    snapshot
    wait_s 2
    xcrun simctl io "$SIM" screenshot "$path" >/dev/null 2>&1
    echo "$path"
}

screen_diff_px() {
    magick compare -metric AE "$1" "$2" null: 2>&1 | awk 'NR==1{printf "%.0f", $1+0; exit} END{if(NR==0)print 0}'
}
screens_differ() { [ "$(screen_diff_px "$1" "$2")" -ge "$SCREEN_DIFF_MIN" ]; }

is_blank() {
    local sd; sd="$(magick "$1" -colorspace Gray -format "%[fx:standard_deviation]" info: 2>/dev/null)"
    awk -v s="${sd:-0}" -v m="$BLANK_STDEV_MAX" 'BEGIN{exit !(s<m)}'
}

log_baseline() { wc -l < "$LOG" 2>/dev/null | tr -d ' '; }

check() {
    if [ "$1" = PASS ]; then
        PASS=$((PASS+1)); RESULTS+=("  PASS  $2"); echo "  [PASS] $2"
    else
        FAIL=$((FAIL+1)); RESULTS+=("  FAIL  $2"); echo "  [FAIL] $2"
    fi
}

assert_no_crash() {
    local base="$1" hit
    hit="$(tail -n "+$((base+1))" "$LOG" 2>/dev/null | grep -niE "$CRASH_REGEX" | head -3)"
    if [ -n "$hit" ]; then
        check FAIL "BIA: no guest crash"
        echo "$hit" | sed 's/^/      /'
    else
        check PASS "BIA: no guest crash"
    fi
}

launch_bia() {
    xcrun simctl terminate "$SIM" "$BUNDLE_ID" >/dev/null 2>&1 || true
    wait_s 2
    xcrun simctl launch "$SIM" "$BUNDLE_ID" -EKA2L1RegressionMode 1 -LaunchROMCode "$BIA_ROM" -LaunchAppUID "$BIA_UID" >/dev/null 2>&1
}

need xcodebuildmcp; need jq; need magick; need xcrun
SIM="$(booted_sim)"; [ -z "$SIM" ] && die "no booted iPhone simulator"
mkdir -p "$OUTDIR"

echo "simulator: $SIM"
echo "rom/app:   $BIA_ROM / $BIA_UID"
echo "results:   $OUTDIR"

if [ -n "$INSTALL_APP" ]; then
    [ -d "$INSTALL_APP" ] || die "app not found: $INSTALL_APP"
    echo "installing $INSTALL_APP"
    xcrun simctl install "$SIM" "$INSTALL_APP" || die "install failed"
fi

echo "== Brother in Arms 3D gameplay waypoint =="
launch_bia
LOG="$(log_path)"; [ -z "$LOG" ] && die "cannot find emulator log"
wait_s 22
BASE="$(log_baseline)"

S_SOUND="$(shot bia_1_sound_prompt)"
is_blank "$S_SOUND" && check FAIL "BIA: sound prompt rendered" \
                    || check PASS "BIA: sound prompt rendered"

tap_label "LSK" || check FAIL "BIA: answer YES to sound prompt"
wait_s "$MENU_WAIT"
S_TITLE="$(shot bia_2_title)"
screens_differ "$S_SOUND" "$S_TITLE" \
    && check PASS "BIA: advanced to title screen" \
    || check FAIL "BIA: advanced to title screen"

tap_label "LSK" || check FAIL "BIA: open main menu"
wait_s "$MENU_WAIT"
S_MENU="$(shot bia_3_main_menu)"
screens_differ "$S_TITLE" "$S_MENU" \
    && check PASS "BIA: main menu rendered" \
    || check FAIL "BIA: main menu rendered"

# The default selected item is Continue when save data exists. This is the most
# stable waypoint for performance comparisons because it skips profile/new-game
# setup and lands in the same early 3D scene each run.
tap_label "OK" || check FAIL "BIA: select Continue"
wait_s "$MENU_WAIT"
S_CUTSCENE="$(shot bia_4_3d_entry)"
screens_differ "$S_MENU" "$S_CUTSCENE" \
    && check PASS "BIA: entered 3D scene" \
    || check FAIL "BIA: entered 3D scene"

# BIA displays "Press 5 to skip/continue" on both the intro and first tutorial
# overlay. Send it twice so the final waypoint is past the initial "continue"
# prompt when save data lands at the first Normandy scene. If either screen
# ignores one press, the final screenshot remains a valid 3D waypoint and the
# diff check below still confirms we are past the menu.
tap_label "5, JKL" || tap_label "5" prefix || check FAIL "BIA: press 5 on 3D prompt"
wait_s 3
tap_label "5, JKL" || tap_label "5" prefix || true
wait_s "$GAME_WAIT"
S_GAME="$(shot bia_5_gameplay)"

is_blank "$S_GAME" && check FAIL "BIA: gameplay frame rendered (non-blank)" \
                   || check PASS "BIA: gameplay frame rendered (non-blank)"
screens_differ "$S_MENU" "$S_GAME" \
    && check PASS "BIA: reached gameplay waypoint past menu" \
    || check FAIL "BIA: reached gameplay waypoint past menu"

assert_no_crash "$BASE"

xcrun simctl terminate "$SIM" "$BUNDLE_ID" >/dev/null 2>&1 || true

echo
echo "===== BIA gameplay summary ====="
printf '%s\n' "${RESULTS[@]}"
echo "-------------------------------"
echo "PASS=$PASS FAIL=$FAIL   screenshots in $OUTDIR"
[ "$FAIL" -eq 0 ]
