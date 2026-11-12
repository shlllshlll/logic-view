#!/usr/bin/env bash
# 清理 macos/install_deps.sh 安装的 Homebrew 依赖。
#
# 用法：
#   ./macos/cleanup_deps.sh --dry-run     # 只显示会删什么
#   ./macos/cleanup_deps.sh               # 交互确认后卸载
#   ./macos/cleanup_deps.sh --yes         # 不确认直接卸载
#   ./macos/cleanup_deps.sh --yes --autoremove  # 同时清理不再被依赖的孤儿包
#   ./macos/cleanup_deps.sh --yes --build       # 同时删除 build/ install/ dist/
#
# 只会卸载“本次安装前不存在”的顶层 formula；
# 安装前就存在、被顺带升级的共享依赖（pcre2、sqlite、xz 等）不会被动，
# 也无法通过 brew 自动降级。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

FORMULAE=(
    qt@5
    glibmm@2.66
    libsigc++@2
    glib
    boost
    libusb
    hidapi
    libzip
    python@3.14
)

DRY_RUN=0
ASSUME_YES=0
CLEAN_BUILD=0
AUTOREMOVE=0

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --yes|-y)  ASSUME_YES=1 ;;
        --build)   CLEAN_BUILD=1 ;;
        --autoremove) AUTOREMOVE=1 ;;
        -h|--help)
            sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

BEFORE="$SCRIPT_DIR/brew-formulae-before.txt"
if [ ! -f "$BEFORE" ]; then
    echo "warning: $BEFORE 不存在，无法区分安装前已存在的包。" >&2
    echo "         将只按当前已安装状态处理。" >&2
    : > /tmp/empty-before.$$
    BEFORE="/tmp/empty-before.$$"
    trap 'rm -f /tmp/empty-before.$$' EXIT
fi

TO_REMOVE=()
for f in "${FORMULAE[@]}"; do
    brew list --formula "$f" >/dev/null 2>&1 || continue
    if grep -qx "$f" "$BEFORE"; then
        echo "skip (pre-existing): $f"
        continue
    fi
    TO_REMOVE+=("$f")
done

if [ ${#TO_REMOVE[@]} -eq 0 ]; then
    echo "没有需要卸载的 formula。"
else
    echo
    echo "将卸载以下 formula："
    printf '  - %s\n' "${TO_REMOVE[@]}"
    echo
    echo "注意："
    echo "  - 只卸载顶层包，不会自动删除它们引入的依赖。"
    echo "  - 若其他软件仍依赖这些包，brew 会拒绝卸载。"
    echo "  - 安装时被顺带升级的共享依赖不会回滚。"
    echo

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "[dry-run] 不执行卸载。"
    else
        if [ "$ASSUME_YES" -ne 1 ]; then
            read -r -p "确认卸载？[y/N] " ans
            case "$ans" in
                y|Y|yes|YES) ;;
                *) echo "已取消。"; exit 0 ;;
            esac
        fi
        for f in "${TO_REMOVE[@]}"; do
            echo "==> brew uninstall $f"
            brew uninstall "$f" || echo "warning: 卸载 $f 失败（可能被其他包依赖）" >&2
        done
        if [ "$AUTOREMOVE" -eq 1 ]; then
            echo
            echo "==> brew autoremove（清理不再被依赖的孤儿包）"
            brew autoremove || true
        else
            echo
            echo "提示：加 --autoremove 可清理本次引入、已无人依赖的库。"
        fi
    fi
fi

if [ "$CLEAN_BUILD" -eq 1 ]; then
    echo
    echo "将删除构建产物："
    for d in build install dist; do
        if [ -d "$PROJECT_DIR/$d" ]; then
            echo "  - $d/"
            if [ "$DRY_RUN" -ne 1 ]; then
                rm -rf "${PROJECT_DIR:?}/$d"
            fi
        fi
    done
fi

echo
echo "完成。"
