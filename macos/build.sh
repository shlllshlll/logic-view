#!/usr/bin/env bash
# 一键构建 CH32H417 上位机的 macOS 版本。
#
# 用法：
#   ./macos/build.sh            # 增量构建
#   ./macos/build.sh --clean    # 清理 build/ install/ 后全量构建
#   ./macos/build.sh --bundle   # 构建后把 Qt/Python 等依赖打包进 .app
#
# 产物：
#   build/pulseview/LogicAnalyzer.app
#   install/                     libsigrok / libsigrokcxx / libsigrokdecode + 解码器
#
# 依赖：先执行 ./macos/install_deps.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

CLEAN=0
BUNDLE=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --bundle) BUNDLE=1 ;;
        -h|--help)
            sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

PREFIX="$ROOT/install"
BUILD="$ROOT/build"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

if [ "$CLEAN" -eq 1 ]; then
    echo "==> cleaning $BUILD and $PREFIX"
    rm -rf "$BUILD" "$PREFIX"
fi

mkdir -p "$BUILD" "$PREFIX"

CMAKE_COMMON=(
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_PREFIX_PATH="$QT5;$GLIBMM;$PREFIX"
    -DDISABLE_WERROR=ON
)

echo "==> [1/4] libsigrok"
cmake -S libsigrok -B "$BUILD/libsigrok" "${CMAKE_COMMON[@]}"
cmake --build "$BUILD/libsigrok" -j "$JOBS"
cmake --install "$BUILD/libsigrok"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"

echo "==> [2/4] libsigrokcxx"
cmake -S libsigrok/bindings/cxx -B "$BUILD/libsigrokcxx" "${CMAKE_COMMON[@]}"
cmake --build "$BUILD/libsigrokcxx" -j "$JOBS"
cmake --install "$BUILD/libsigrokcxx"

echo "==> [3/4] libsigrokdecode"
cmake -S libsigrokdecode -B "$BUILD/libsigrokdecode" "${CMAKE_COMMON[@]}"
cmake --build "$BUILD/libsigrokdecode" -j "$JOBS"
cmake --install "$BUILD/libsigrokdecode"

echo "==> [4/4] PulseView"
cmake -S pulseview -B "$BUILD/pulseview" "${CMAKE_COMMON[@]}" \
    -DENABLE_DECODE=ON \
    -DENABLE_FLOW=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_SIGNALS=OFF \
    -DSTATIC_PKGDEPS_LIBS=OFF
cmake --build "$BUILD/pulseview" -j "$JOBS"

APP="$BUILD/pulseview/LogicAnalyzer.app"
if [ -d "$APP" ]; then
    echo "==> ad-hoc signing"
    codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || \
        echo "warning: codesign failed (app still runs locally)"
fi

if [ "$BUNDLE" -eq 1 ] && [ -d "$APP" ]; then
    echo "==> bundling runtime dependencies"
    "$SCRIPT_DIR/bundle.sh" "$APP"
fi

echo
echo "==> done"
echo "    app: $APP"
echo "    run: open \"$APP\""
