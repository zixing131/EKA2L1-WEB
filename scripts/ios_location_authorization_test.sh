#!/usr/bin/env bash
set -euo pipefail

# Requires an Apple Silicon Mac and xcodebuildmcp.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIMULATOR_ID="${1:?Usage: ios_location_authorization_test.sh SIMULATOR_UDID}"
TEST_DIR="$(mktemp -d /tmp/eka2l1-location-authorization.XXXXXX)"
APP="$TEST_DIR/LocationAuthorizationTests.app"
BUNDLE_ID="com.eka2l1.location-authorization-tests"
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$APP"

SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
clang++ -target arm64-apple-ios16.0-simulator -isysroot "$SDK" \
    -std=c++17 -fobjc-arc -O2 -g -I "$ROOT_DIR/src/emu/drivers/include" \
    "$ROOT_DIR/src/emu/ios/Tests/LocationAuthorizationTests.mm" \
    "$ROOT_DIR/src/emu/drivers/src/location/backend/ios/location_ios.mm" \
    -framework Foundation -framework CoreLocation -framework UIKit \
    -o "$APP/LocationAuthorizationTests"

python3 - "$APP" "$BUNDLE_ID" <<'PY'
from pathlib import Path
import plistlib, sys
app = Path(sys.argv[1])
with (app / 'Info.plist').open('wb') as out:
    plistlib.dump(dict(CFBundleIdentifier=sys.argv[2], CFBundleExecutable='LocationAuthorizationTests',
                      CFBundleName='LocationAuthorizationTests', CFBundlePackageType='APPL',
                      CFBundleVersion='1', CFBundleShortVersionString='1.0',
                      MinimumOSVersion='16.0', LSRequiresIPhoneOS=True,
                      UIDeviceFamily=[1, 2],
                      UIApplicationSceneManifest=dict(UIApplicationSupportsMultipleScenes=False,
                          UISceneConfigurations=dict(UIWindowSceneSessionRoleApplication=[
                              dict(UISceneConfigurationName='Location authorization tests',
                                   UISceneDelegateClassName='LocationAuthorizationSceneDelegate')]))), out)
PY
codesign --force --sign - "$APP"
xcodebuildmcp simulator stop --simulator-id "$SIMULATOR_ID" --bundle-id "$BUNDLE_ID" >/dev/null 2>&1 || true
xcodebuildmcp simulator install --simulator-id "$SIMULATOR_ID" --app-path "$APP"
DATA_DIR="$(xcrun simctl get_app_container "$SIMULATOR_ID" "$BUNDLE_ID" data)"
rm -f "$DATA_DIR/Documents/result.txt"
xcodebuildmcp simulator launch-app --simulator-id "$SIMULATOR_ID" --bundle-id "$BUNDLE_ID"
for _ in {1..20}; do
    if [ -f "$DATA_DIR/Documents/result.txt" ]; then
        cat "$DATA_DIR/Documents/result.txt"
        xcodebuildmcp simulator stop --simulator-id "$SIMULATOR_ID" --bundle-id "$BUNDLE_ID"
        exit 0
    fi
    sleep 1
done
echo "FAIL: location authorization harness did not complete; inspect the simulator crash report" >&2
exit 1
