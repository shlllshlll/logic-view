#!/usr/bin/env bash
# Build CH32H417 LogicAnalyzer for Linux (same libusb backend as macOS).
#
# Usage:
#   ./linux/build.sh            # incremental build + package
#   ./linux/build.sh --clean    # remove build/ install/ dist/ first
#
# Output:
#   build/pulseview/LogicAnalyzer
#   dist/LogicAnalyzer-linux-<arch>.tar.gz

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        -h|--help)
            sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

PREFIX="$ROOT/install"
BUILD="$ROOT/build"
DIST="$ROOT/dist"
JOBS="$(nproc 2>/dev/null || echo 4)"

if [ "$CLEAN" -eq 1 ]; then
    echo "==> cleaning $BUILD $PREFIX $DIST"
    rm -rf "$BUILD" "$PREFIX" "$DIST"
fi

mkdir -p "$BUILD" "$PREFIX" "$DIST"

CMAKE_COMMON=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DDISABLE_WERROR=ON
)

echo "==> [1/4] libsigrok"
cmake -S libsigrok -B "$BUILD/libsigrok" "${CMAKE_COMMON[@]}"
cmake --build "$BUILD/libsigrok" -j "$JOBS"
cmake --install "$BUILD/libsigrok"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "==> [2/4] libsigrokcxx"
cmake -S libsigrok/bindings/cxx -B "$BUILD/libsigrokcxx" "${CMAKE_COMMON[@]}" \
    -DCMAKE_PREFIX_PATH="$PREFIX"
cmake --build "$BUILD/libsigrokcxx" -j "$JOBS"
cmake --install "$BUILD/libsigrokcxx"

echo "==> [3/4] libsigrokdecode"
cmake -S libsigrokdecode -B "$BUILD/libsigrokdecode" "${CMAKE_COMMON[@]}"
cmake --build "$BUILD/libsigrokdecode" -j "$JOBS"
cmake --install "$BUILD/libsigrokdecode"

echo "==> [4/4] LogicAnalyzer"
cmake -S pulseview -B "$BUILD/pulseview" "${CMAKE_COMMON[@]}" \
    -DENABLE_DECODE=ON \
    -DENABLE_FLOW=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_SIGNALS=OFF \
    -DSTATIC_PKGDEPS_LIBS=OFF
cmake --build "$BUILD/pulseview" -j "$JOBS"
cmake --install "$BUILD/pulseview"

ARCH="$(uname -m)"
NAME="LogicAnalyzer-linux-$ARCH"
STAGE="$DIST/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$PREFIX/." "$STAGE/"

# The binary's RPATH is $ORIGIN/../lib, so bin/ and lib/ must stay together.
TARBALL="$DIST/$NAME.tar.gz"
tar -C "$DIST" -czf "$TARBALL" "$NAME"

echo
echo "==> done"
echo "    binary:  $BUILD/pulseview/LogicAnalyzer"
echo "    tarball: $TARBALL"
echo
echo "Install the udev rule for non-root USB access:"
echo "  sudo cp linux/99-logicanalyzer.rules /etc/udev/rules.d/"
echo "  sudo udevadm control --reload-rules && sudo udevadm trigger"
