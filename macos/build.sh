#!/usr/bin/env bash
# 一键构建 CH32H417 上位机的 macOS 版本。
#
# 用法：
#   ./macos/build.sh              # 增量构建
#   ./macos/build.sh --clean      # 清理 build/ install/ 后全量构建
#   ./macos/build.sh --bundle     # 构建后把 Qt/Python 等依赖打包进 .app
#   ./macos/build.sh --install    # 构建并安装到 /Applications（隐含 --bundle）
#   LOGICVIEW_INSTALL_DIR=~/Applications ./macos/build.sh --install
#                                 # 安装到指定目录，默认 /Applications
#
# 产物：
#   build/pulseview/LogicAnalyzer.app
#   install/                     libsigrok / libsigrokcxx / libsigrokdecode + 解码器
#   $LOGICVIEW_INSTALL_DIR/LogicAnalyzer.app（加 --install 时）
#
# 依赖：先执行 ./macos/install_deps.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

CLEAN=0
BUNDLE=0
INSTALL=0
for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=1 ;;
        --bundle) BUNDLE=1 ;;
        --install) INSTALL=1 ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

# 装到 /Applications 的必须是自包含产物，否则目标机器缺 Homebrew 就跑不起来
if [ "$INSTALL" -eq 1 ] && [ "$BUNDLE" -eq 0 ]; then
    echo "==> --install 隐含 --bundle"
    BUNDLE=1
fi

# shellcheck source=env.sh
source "$SCRIPT_DIR/env.sh"

PREFIX="$ROOT/install"
BUILD="$ROOT/build"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

APP_NAME="LogicAnalyzer.app"
APP="$BUILD/pulseview/$APP_NAME"
INSTALL_DIR="${LOGICVIEW_INSTALL_DIR:-/Applications}"

# 按进程名精确匹配：用 pgrep -f 匹配路径会误伤命令行里带该路径的编译/链接进程
APP_PROC_NAME="LogicAnalyzer"

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

if [ -d "$APP" ]; then
    echo "==> ad-hoc signing"
    codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || \
        echo "warning: codesign failed (app still runs locally)"
fi

if [ "$BUNDLE" -eq 1 ] && [ -d "$APP" ]; then
    echo "==> bundling runtime dependencies"
    "$SCRIPT_DIR/bundle.sh" "$APP"
fi

# 退出正在运行的上位机：先试 AppleScript 正常退出，超时再 SIGTERM
stop_running_app() {
    pgrep -x "$APP_PROC_NAME" >/dev/null 2>&1 || return 0

    echo "==> 退出正在运行的上位机"
    osascript -e 'tell application "LogicAnalyzer" to quit' >/dev/null 2>&1 || true
    for _ in $(seq 1 10); do
        pgrep -x "$APP_PROC_NAME" >/dev/null 2>&1 || return 0
        sleep 0.5
    done

    echo "    正常退出超时，发送 SIGTERM"
    pkill -TERM -x "$APP_PROC_NAME" 2>/dev/null || true
    for _ in $(seq 1 10); do
        pgrep -x "$APP_PROC_NAME" >/dev/null 2>&1 || return 0
        sleep 0.5
    done

    echo "error: 上位机仍在运行，请手动退出后重试" >&2
    return 1
}

# 安装：备份旧版本 -> ditto -> 校验签名
install_app() {
    local src="$1"
    local dest_dir="$2"
    local dest="$dest_dir/$APP_NAME"

    if [ ! -d "$src" ] || [ ! -f "$src/Contents/Info.plist" ]; then
        echo "error: 构建产物不可用: $src" >&2
        return 1
    fi

    stop_running_app
    mkdir -p "$dest_dir" 2>/dev/null || {
        echo "error: 无法创建 $dest_dir（权限不足）" >&2
        return 1
    }

    if [ -d "$dest" ]; then
        local backup="/tmp/$APP_NAME.bak-$(date +%Y%m%d-%H%M%S)"
        if ! mv "$dest" "$backup"; then
            echo "error: 无法替换 $dest（建议用管理员账号执行，或指定 LOGICVIEW_INSTALL_DIR）" >&2
            return 1
        fi
        echo "    旧版本已备份: $backup"

        # 只保留最新一份备份，避免 /tmp 里堆积几百 MB 的历史副本
        local old
        for old in /tmp/"$APP_NAME".bak-*; do
            [ -d "$old" ] || continue
            [ "$old" = "$backup" ] && continue
            rm -rf "$old" && echo "    已删除更旧的备份: $old"
        done
    fi

    echo "    复制到 $dest"
    ditto "$src" "$dest" || return 1

    codesign --verify --strict "$dest" 2>/dev/null && echo "    签名校验: OK"
    echo "    已安装: $dest"
}

if [ "$INSTALL" -eq 1 ]; then
    echo "==> 安装到 $INSTALL_DIR"
    install_app "$APP" "$INSTALL_DIR"
fi

echo
echo "==> done"
echo "    app: $APP"
if [ "$INSTALL" -eq 1 ]; then
    echo "    installed: $INSTALL_DIR/$APP_NAME"
    echo "    run: open \"$INSTALL_DIR/$APP_NAME\""
else
    echo "    run: open \"$APP\""
    echo "    install: ./macos/build.sh --install    # 安装到 $INSTALL_DIR"
fi
