#!/usr/bin/env bash
# Make LogicAnalyzer.app self-contained: Qt, glib/boost/libusb/... and
# Python.framework are copied into the bundle, so the target Mac needs no
# Homebrew packages at runtime.
#
# Usage:
#   ./macos/bundle.sh [path/to/LogicAnalyzer.app]
#
# Build-time dependencies: qt@5 (macdeployqt), python@3.14.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
APP="${1:-$ROOT/build/pulseview/LogicAnalyzer.app}"

if [ ! -d "$APP" ]; then
    echo "app bundle not found: $APP" >&2
    exit 1
fi
if [ ! -f "$APP/Contents/Info.plist" ]; then
    echo "missing $APP/Contents/Info.plist (build the app with cmake first)" >&2
    exit 1
fi

export HOMEBREW_PREFIX="$(brew --prefix)"
QT_PREFIX="$(brew --prefix qt@5)"
PY_PREFIX="$(brew --prefix python@3.14)"
PYTHON="$PY_PREFIX/bin/python3"
if [ ! -x "$PYTHON" ]; then
    for cand in "$PY_PREFIX"/bin/python3.[0-9]*; do
        [ -x "$cand" ] || continue
        case "$cand" in *-config|*m) continue ;; esac
        PYTHON="$cand"
        break
    done
fi
[ -x "$PYTHON" ] || { echo "python3 not found under $PY_PREFIX" >&2; exit 1; }
PY_VER="$("$PYTHON" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"

echo "==> macdeployqt ($QT_PREFIX)"
MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
[ -x "$MACDEPLOYQT" ] || { echo "macdeployqt not found" >&2; exit 1; }
set +e
"$MACDEPLOYQT" "$APP" -always-overwrite > /tmp/logicanalyzer-macdeployqt.log 2>&1
MACDEPLOYQT_RC=$?
set -e
if [ "$MACDEPLOYQT_RC" -ne 0 ] && [ ! -d "$APP/Contents/Frameworks/QtCore.framework" ]; then
    tail -20 /tmp/logicanalyzer-macdeployqt.log >&2
    exit 1
fi
[ -f "$APP/Contents/Info.plist" ] || {
    echo "macdeployqt removed Info.plist; aborting" >&2
    exit 1
}

# These are not used by PulseView and only add size/signing noise.
rm -rf "$APP/Contents/PlugIns/virtualkeyboard" \
       "$APP/Contents/PlugIns/platforminputcontexts"

echo "==> Python.framework $PY_VER"
PYFW="$APP/Contents/Frameworks/Python.framework"
rm -rf "$PYFW"
ditto "$PY_PREFIX/Frameworks/Python.framework" "$PYFW"
rm -rf "$PYFW/Versions/$PY_VER/Headers" \
       "$PYFW/Versions/$PY_VER/include" \
       "$PYFW/Versions/$PY_VER/share" \
       "$PYFW/Versions/$PY_VER/bin" \
       "$PYFW/Versions/$PY_VER/Resources/Python.app"
ln -sfn "$PY_VER" "$PYFW/Versions/Current"
ln -sfn "Versions/Current/Python" "$PYFW/Python"
ln -sfn "Versions/Current/Resources" "$PYFW/Resources"

echo "==> completing dependencies and signing"
"$PYTHON" "$SCRIPT_DIR/fix_bundle_deps.py" "$APP"

echo
echo "==> bundled app: $APP"
echo "    size: $(du -sh "$APP" | cut -f1)"
echo "    verify: otool -L \"$APP/Contents/MacOS/LogicAnalyzer\" | grep homebrew"
