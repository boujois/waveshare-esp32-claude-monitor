#!/bin/sh
# Builds the menu bar app: ./build.sh <out-dir> [version]  ->  "<out-dir>/Claude Monitor.app"
# Needs only the Command Line Tools (xcrun swiftc), no Xcode project. Universal (arm64 + x86_64)
# unless MENUBAR_HOST_ARCH_ONLY=1, which builds just this Mac's architecture (quicker).
# Swift 6 language mode when the compiler has it, else 5; MENUBAR_SWIFT_VERSION=5|6 picks one.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$1" ]; then
  echo "usage: $0 <out-dir> [version]" >&2
  exit 2
fi
OUT="$1"
VERSION="${2:-dev}"
NAME="Claude Monitor"
APP="$OUT/$NAME.app"

if [ "$(uname)" != Darwin ]; then
  echo "The menu bar app can only be built on macOS" >&2
  exit 1
fi
if ! xcrun --find swiftc >/dev/null 2>&1; then
  echo "swiftc not found: install the Command Line Tools (xcode-select --install)" >&2
  exit 1
fi

# Swift 5 compilers (Command Line Tools before 16, so any macOS 13) don't know -swift-version 6
SWIFT_MODE="$MENUBAR_SWIFT_VERSION"
if [ -z "$SWIFT_MODE" ]; then
  major="$(xcrun swiftc --version 2>&1 | sed -n 's/.*Swift version \([0-9][0-9]*\)\..*/\1/p' | head -n 1)"
  if [ -n "$major" ] && [ "$major" -ge 6 ]; then SWIFT_MODE=6; else SWIFT_MODE=5; fi
fi

if [ "$MENUBAR_HOST_ARCH_ONLY" = 1 ]; then
  ARCHS="$(uname -m)"
else
  ARCHS="arm64 x86_64"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/claude-monitor-build.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# The sources as "$@", so paths with spaces survive
set -- "$DIR"/Sources/*.swift
for arch in $ARCHS; do
  echo "== Compiling Claude Monitor ($arch, Swift $SWIFT_MODE mode)"
  xcrun swiftc -O -parse-as-library -swift-version "$SWIFT_MODE" -module-name ClaudeMonitor \
    -target "$arch-apple-macos13.0" -o "$WORK/$arch" "$@"
done

set --
for arch in $ARCHS; do
  set -- "$@" "$WORK/$arch"
done
mkdir -p "$OUT"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
lipo -create "$@" -output "$APP/Contents/MacOS/$NAME"

# macOS wants bundle versions as numbers (v1.4.0 -> 1.4.0; dev builds are 0.0.0). The tag itself
# goes in ClaudeMonitorVersion.
NUMERIC="$(printf '%s\n' "$VERSION" | sed -n 's/^v\{0,1\}\([0-9][0-9]*\(\.[0-9][0-9]*\)\{0,2\}\).*/\1/p')"
[ -n "$NUMERIC" ] || NUMERIC=0.0.0
PLIST="$APP/Contents/Info.plist"
cp "$DIR/Info.plist" "$PLIST"
plutil -replace CFBundleShortVersionString -string "$NUMERIC" "$PLIST"
plutil -replace CFBundleVersion -string "$NUMERIC" "$PLIST"
plutil -replace ClaudeMonitorVersion -string "$VERSION" "$PLIST"
plutil -lint -s "$PLIST"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# Ad-hoc signature: enough for Apple silicon and for launchd to run it
codesign --force --sign - "$APP"
echo "Built $APP ($VERSION, $(lipo -archs "$APP/Contents/MacOS/$NAME"))"
