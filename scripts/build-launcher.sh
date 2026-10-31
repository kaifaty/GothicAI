#!/usr/bin/env bash
# Builds the launcher: signed Gothic AI.app on macOS, a native binary on Linux.
#
#   scripts/build-launcher.sh               release build, signed, installed
#   scripts/build-launcher.sh --no-install  only build and sign
#   scripts/build-launcher.sh --debug       a debug build (faster to compile)
#
# The signing identity comes from GOTHIC_SIGNING_IDENTITY (the environment or the repository's .env),
# e.g. "Apple Development: Your Name" (`security find-identity -v -p codesigning` lists them). A stable
# signature keeps the microphone permission across rebuilds; without one the app is signed ad hoc and
# macOS asks for the microphone again after every build.
#
# The app runs the services and the game from this repository (launcher/core, OpenGothic/build), so
# it is built for this checkout and this Mac.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_SRC="$ROOT/launcher/app"
TARGET="/Applications/Gothic AI.app"
PROFILE=release
INSTALL=1
for arg in "$@"; do
  case "$arg" in
    --debug) PROFILE=debug ;;
    --no-install) INSTALL=0 ;;
    *) echo "usage: $0 [--debug] [--no-install]" >&2; exit 2 ;;
  esac
done

if [ "$(uname -s)" = Linux ]; then
  export PATH="$HOME/.local/bin:$HOME/.bun/bin:$PATH"
  if ! command -v bun >/dev/null; then
    # Install only the binary in the user's home; leave shell profiles and bun.lock alone.
    python3 - <<'PY'
import hashlib
import io
import os
import urllib.request
import zipfile
from pathlib import Path

url = "https://github.com/oven-sh/bun/releases/download/bun-v1.4.3/bun-linux-x64.zip"
expected_sha256 = "3e730528d89775f03f87f05c06787c4df4099d0a37f3f252e7b829cb23816e18"
with urllib.request.urlopen(url) as response:
    data = response.read()
if hashlib.sha256(data).hexdigest() != expected_sha256:
    raise SystemExit("Bun 1.4.3 download failed SHA-256 verification; nothing was installed")
archive = zipfile.ZipFile(io.BytesIO(data))
binary = Path.home() / ".local/bin/bun"
binary.parent.mkdir(parents=True, exist_ok=True)
temporary = binary.with_suffix(".download")
temporary.write_bytes(archive.read("bun-linux-x64/bun"))
temporary.chmod(0o755)
os.replace(temporary, binary)
PY
  fi
  cd "$APP_SRC"
  bun install --frozen-lockfile
  args=(--no-bundle)
  if [ "$PROFILE" = debug ]; then args+=(--debug); fi
  bun tauri build "${args[@]}"
  # ryzen-play.sh installs the desktop entry pointing into this checkout.
  echo "built: $APP_SRC/src-tauri/target/$PROFILE/gothic-launcher"
  exit 0
fi

identity="${GOTHIC_SIGNING_IDENTITY:-}"
if [ -z "$identity" ] && [ -f "$ROOT/.env" ]; then
  identity="$(sed -n 's/^GOTHIC_SIGNING_IDENTITY=//p' "$ROOT/.env" | tail -1 | sed -e "s/^[\"']//" -e "s/[\"']\$//")"
fi
if [ -z "$identity" ]; then
  echo "GOTHIC_SIGNING_IDENTITY is not set: signing ad hoc, macOS will ask for the microphone after each build" >&2
  identity="-"
fi

if [ "$INSTALL" = 1 ] && pgrep -f "$TARGET/Contents/MacOS/" >/dev/null; then
  echo "Gothic AI is running: quit it first (a game it started would stop)" >&2
  exit 1
fi

# the game's own bundle, so it has a name and an icon in the Dock (the launcher prefers it)
if [ -x "$ROOT/OpenGothic/build/opengothic/Gothic2Notr" ]; then
  "$ROOT/scripts/make-dev-app.sh" >/dev/null
fi

cd "$APP_SRC"
[ -d node_modules ] || bun install
args=(--bundles app)
if [ "$PROFILE" = debug ]; then args+=(--debug); fi
APPLE_SIGNING_IDENTITY="$identity" bun tauri build "${args[@]}"

BUILT="$APP_SRC/src-tauri/target/$PROFILE/bundle/macos/Gothic AI.app"
codesign --verify --strict "$BUILT"
if ! codesign -d --entitlements - "$BUILT" 2>/dev/null | grep -q "com.apple.security.device.audio-input"; then
  echo "the app has no microphone entitlement: push-to-talk would be refused" >&2
  exit 1
fi
codesign -dvv "$BUILT" 2>&1 | grep -E "^(Authority|Signature|Runtime Version)" | head -3 || true

if [ "$INSTALL" = 1 ]; then
  rm -rf "$TARGET"
  ditto "$BUILT" "$TARGET"
  echo "installed: $TARGET"
else
  echo "built: $BUILT"
fi
