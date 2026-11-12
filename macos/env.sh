#!/usr/bin/env bash
# macOS 构建环境变量。用法： source macos/env.sh
#
# 说明：
#   - qt@5 是 keg-only，必须显式加入 PATH / PKG_CONFIG_PATH / CMAKE_PREFIX_PATH
#   - glibmm 必须用 2.66 系列（glibmm-2.4 ABI），不要用 Homebrew 的 glibmm 2.90

if ! command -v brew >/dev/null 2>&1; then
    echo "error: Homebrew not found" >&2
    return 1 2>/dev/null || exit 1
fi

export HOMEBREW_PREFIX="$(brew --prefix)"
export QT5="$(brew --prefix qt@5 2>/dev/null || true)"
export GLIBMM="$(brew --prefix glibmm@2.66 2>/dev/null || true)"
export PYTHON_PREFIX="$(brew --prefix python@3.14 2>/dev/null || true)"

if [ -z "$QT5" ] || [ ! -d "$QT5" ]; then
    echo "warning: qt@5 not installed. run macos/install_deps.sh" >&2
fi
if [ -z "$GLIBMM" ] || [ ! -d "$GLIBMM" ]; then
    echo "warning: glibmm@2.66 not installed. run macos/install_deps.sh" >&2
fi

export PATH="$QT5/bin:$HOMEBREW_PREFIX/bin:$PYTHON_PREFIX/libexec/bin:$PATH"
export PKG_CONFIG_PATH="$QT5/lib/pkgconfig:$GLIBMM/lib/pkgconfig:$PYTHON_PREFIX/lib/pkgconfig:$HOMEBREW_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$QT5:$GLIBMM:$HOMEBREW_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

echo "HOMEBREW_PREFIX = $HOMEBREW_PREFIX"
echo "QT5             = $QT5"
echo "GLIBMM          = $GLIBMM"
echo "PYTHON_PREFIX   = $PYTHON_PREFIX"
