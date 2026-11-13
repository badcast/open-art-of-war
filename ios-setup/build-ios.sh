#!/usr/bin/env bash
# Configures and builds Open Art of War for iOS (device or simulator) using Xcode + CMake.
# Optionally downloads bundled game data (*.blob, *.mod) via GAME_DATA_URLS / GAME_DATA_TOKEN
# (same variables as android-setup / .github/workflows/ios.yml).
#
# Usage:
#   ./ios-setup/build-ios.sh [device|simulator] [--ipa] [--sign]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

TARGET="device"
MAKE_IPA=0
ALLOW_SIGNING=0

for arg in "$@"; do
    case "$arg" in
        device|iphoneos) TARGET="device" ;;
        simulator|iphonesimulator) TARGET="simulator" ;;
        --ipa) MAKE_IPA=1 ;;
        --sign) ALLOW_SIGNING=1 ;;
        *)
            echo "Unknown option: $arg (expected: device | simulator [--ipa] [--sign])" >&2
            exit 1
            ;;
    esac
done

if [ "$(uname -s)" != "Darwin" ]; then
    echo "iOS builds require macOS with Xcode installed." >&2
    exit 1
fi

if [ "$TARGET" = "simulator" ]; then
    SYSROOT="iphonesimulator"
    ARCH="${OAOW_IOS_ARCH:-arm64}"
    BUILD_DIR="${SCRIPT_DIR}/build-simulator"
else
    SYSROOT="iphoneos"
    ARCH="${OAOW_IOS_ARCH:-arm64}"
    BUILD_DIR="${SCRIPT_DIR}/build-device"
fi

# Bundle GAME_DATA_URLS assets (if set and not already staged manually)
ASSETS_DIR="${SCRIPT_DIR}/assets"
if [ -n "${GAME_DATA_URLS:-}" ]; then
    echo "Fetching game data via tools/fetch_game_data.sh..."
    "${ROOT_DIR}/tools/fetch_game_data.sh" "${BUILD_DIR}/game-data"
    rm -rf "${ASSETS_DIR}"
    mkdir -p "${ASSETS_DIR}"
    if [ -s "${BUILD_DIR}/game-data/bundled_files.txt" ]; then
        cp "${BUILD_DIR}/game-data/"* "${ASSETS_DIR}/"
    fi
fi

cmake -G Xcode \
    -S "${ROOT_DIR}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="${SYSROOT}" \
    -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${OAOW_IOS_DEPLOYMENT_TARGET:-15.0}" \
    -DOAOW_BUILD_SERVER=OFF \
    -DOAOW_BUILD_NET_TESTS=OFF \
    -DBUILD_ASSETS_BLOB=OFF

XCODE_SIGN_FLAGS=()
if [ "$ALLOW_SIGNING" -eq 0 ]; then
    XCODE_SIGN_FLAGS+=(
        CODE_SIGNING_ALLOWED=NO
        CODE_SIGNING_REQUIRED=NO
        CODE_SIGN_IDENTITY=""
    )
fi

xcodebuild \
    -project "${BUILD_DIR}/OpenArtOfWar.xcodeproj" \
    -target openaow-game \
    -configuration Release \
    -sdk "${SYSROOT}" \
    -jobs "$(sysctl -n hw.ncpu)" \
    "${XCODE_SIGN_FLAGS[@]}" \
    build

APP_PATH="${BUILD_DIR}/Release-${SYSROOT}/OpenArtOfWar.app"
echo "Built iOS app bundle: ${APP_PATH}"

if [ "$MAKE_IPA" -eq 1 ]; then
    if [ "$SYSROOT" != "iphoneos" ]; then
        echo "--ipa is only valid for device (iphoneos) builds." >&2
        exit 1
    fi
    OUT_DIR="${SCRIPT_DIR}/out"
    rm -rf "${OUT_DIR}/Payload"
    mkdir -p "${OUT_DIR}/Payload"
    cp -a "${APP_PATH}" "${OUT_DIR}/Payload/"
    IPA_PATH="${OUT_DIR}/OpenArtOfWar-ios.ipa"
    (cd "${OUT_DIR}" && zip -qr "${IPA_PATH}" Payload)
    echo "Packaged IPA: ${IPA_PATH}"
fi
