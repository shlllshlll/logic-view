#!/usr/bin/env bash
# 安装 macOS 构建依赖，并记录安装前后的状态。
#
# 用法：
#   ./macos/install_deps.sh
#
# 产物：
#   macos/brew-formulae-before.txt   安装前的 formula 列表
#   macos/brew-formulae-after.txt    安装后的 formula 列表
#   macos/deps.txt                   本次安装/升级的精确版本记录
#   macos/brew-install.log           完整安装日志

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

if ! command -v brew >/dev/null 2>&1; then
    echo "error: Homebrew not found" >&2
    exit 1
fi

# 顶层依赖。注意：glibmm 必须是 2.66（glibmm-2.4 ABI），不是 Homebrew 的 glibmm 2.90。
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

# 构建工具：通常已安装；缺失时提示，不主动升级
for tool in cmake ninja pkgconf; do
    if ! brew list --formula "$tool" >/dev/null 2>&1; then
        echo "error: '$tool' 未安装，请先执行: brew install $tool" >&2
        exit 1
    fi
done

mkdir -p "$SCRIPT_DIR"
brew list --formula 2>/dev/null | sort > "$SCRIPT_DIR/brew-formulae-before.txt"

echo "==> Installing: ${FORMULAE[*]}"
echo "==> 日志: $SCRIPT_DIR/brew-install.log"

# 不自动升级无关的 dependents；构建目录不自动清理
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1
export HOMEBREW_NO_INSTALL_CLEANUP=1

set +e
brew install "${FORMULAE[@]}" 2>&1 | tee "$SCRIPT_DIR/brew-install.log"
status=${PIPESTATUS[0]}
set -e

echo "BREW_EXIT=$status" | tee -a "$SCRIPT_DIR/brew-install.log"

brew list --formula 2>/dev/null | sort > "$SCRIPT_DIR/brew-formulae-after.txt"

{
    echo "# macOS 构建依赖记录"
    echo
    echo "- 时间: $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "- 系统: $(sw_vers -productName) $(sw_vers -productVersion) ($(uname -m))"
    echo "- Homebrew: $(brew --version | head -1)"
    echo "- 安装命令: brew install ${FORMULAE[*]}"
    echo
    echo "## 顶层依赖版本"
    echo
    printf '| formula | version |\n|---|---|\n'
    for f in "${FORMULAE[@]}"; do
        v="$(brew list --versions "$f" 2>/dev/null | awk '{$1=""; sub(/^ /,""); print}')"
        printf '| `%s` | %s |\n' "$f" "${v:-未安装}"
    done
    echo
    echo "## 本次新安装的 formula"
    echo
    comm -13 "$SCRIPT_DIR/brew-formulae-before.txt" "$SCRIPT_DIR/brew-formulae-after.txt" | sed 's/^/- /'
    echo
    echo "## 安装前已存在、本次可能被升级的 formula"
    echo
    comm -12 "$SCRIPT_DIR/brew-formulae-before.txt" "$SCRIPT_DIR/brew-formulae-after.txt" \
        | while read -r f; do
            old="$(grep -E "^$f " "$SCRIPT_DIR/brew-install.log" >/dev/null 2>&1 && echo yes || true)"
            [ -n "$old" ] && echo "- $f"
        done
    echo
    echo "> 注：Homebrew 可能顺带升级共享依赖（pcre2 / libunistring / sqlite / xz 等），"
    echo "> 这些是安装前就存在的包，cleanup_deps.sh 不会卸载它们，也无法自动回滚版本。"
} > "$SCRIPT_DIR/deps.txt"

echo
echo "==> 完成。记录写入 $SCRIPT_DIR/deps.txt"
[ "$status" -eq 0 ] || { echo "error: brew install failed ($status)" >&2; exit "$status"; }

echo
echo "下一步： source macos/env.sh && ./macos/build.sh"
