#!/usr/bin/env bash
# Packs the prebuilt Linux x64 release into one .tar.gz, with its build manifest and SHA256SUMS: the Linux
# counterpart of scripts/windows/package.ps1. Runs after scripts/linux/build.sh and reads that build's
# outputs. The archive holds what a player runs, copied by whitelist into the repository's own layout, so
# the launcher finds everything where it looks:
#   Setup-GothicAI.sh, Start-GothicAI.sh, START_HERE.linux.ru.md, LICENSE, scripts/linux/common.sh
#   launcher/core, server, asr-service, tts-service, libs/chatgpt-auth
#                                          sources, pyproject.toml, uv.lock, .python-version (no tests)
#   OpenGothic/build/opengothic/           Gothic2Notr, content-convert and every other file of that folder
#   OpenGothic/ai_bridge/storygraph/build/ storygraph
#   tools/uv/                              the pinned official uv and its licenses
#   licenses/                              the licenses of everything compiled in or shipped
# tar keeps the executable bits. Python and the locked wheels are not packed: Setup downloads them once
# through uv.
#
# The archive is refused when something private or build-only slipped in, when an ELF binary is not
# x86-64, needs a library outside the system list below, needs glibc newer than 2.35 or needs libstdc++
# (GLIBCXX_, CXXABI_ versions: it must be linked statically).
#
#   scripts/linux/package.sh --uv-tar <uv-x86_64-unknown-linux-gnu.tar.gz> --uv-licenses <folder>
#                            --out-dir <folder> --name <archive base name> [--repository <url>]
#                            [--commit <sha>] [--run-url <url>] [--vulkan-sdk-version <v>]
#                            [--uv-version <v>] [--compiler <text>] [--root <repository>]
# READELF names the readelf to run (binutils' by default).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
READELF="${READELF:-readelf}"
REPOSITORY=https://github.com/kaifaty/GothicAI
COMMIT='' RUN_URL='' VULKAN_SDK_VERSION='' UV_VERSION='' COMPILER=''
UV_TAR='' UV_LICENSES='' OUT_DIR='' NAME=''

fail() { echo "package.sh: $*" >&2; exit 1; }

while (($#)); do
  (($# >= 2)) || fail "$1 needs a value"
  case "$1" in
    --uv-tar) UV_TAR="$2" ;;
    --uv-licenses) UV_LICENSES="$2" ;;
    --out-dir) OUT_DIR="$2" ;;
    --name) NAME="$2" ;;
    --repository) REPOSITORY="$2" ;;
    --commit) COMMIT="$2" ;;
    --run-url) RUN_URL="$2" ;;
    --vulkan-sdk-version) VULKAN_SDK_VERSION="$2" ;;
    --uv-version) UV_VERSION="$2" ;;
    --compiler) COMPILER="$2" ;;
    --root) ROOT="$(cd "$2" && pwd)" ;;
    *) fail "unknown argument: $1" ;;
  esac
  shift 2
done
[[ -n "$UV_TAR" && -n "$UV_LICENSES" && -n "$OUT_DIR" && -n "$NAME" ]] ||
  fail 'needs --uv-tar, --uv-licenses, --out-dir and --name'

PYTHON_PROJECTS=(launcher/core server asr-service tts-service libs/chatgpt-auth)
GAME=OpenGothic/build/opengothic
STORYGRAPH=OpenGothic/ai_bridge/storygraph/build
LICENSE_NAME='^(licen[cs]e|copying|notice|unlicense)([._-].*)?$|\.license$'
# The list of scripts/windows/package.ps1 ($Forbidden) with its Linux object files (.o, .a): build-only
# files, game data, saves, settings, keys, sign-ins, venvs, tests and the internal port briefs
FORBIDDEN='(\.(pdb|ilk|obj|lib|exp|o|a|pyc|pyo|sav|vdf|zen|log)$)|(^|/)(\.env[^/]*|secrets\.toml|settings\.toml|chatgpt\.json|launcher\.json|ou\.bin|gothic\.dat|\.venv|__pycache__|\.git|tests)(/|$)'
FORBIDDEN_CASED='(^|/)PORT_[^/]*\.md(/|$)'
# What a player's system brings: glibc's own libraries and loader, the Vulkan loader of the graphics
# driver, X11 and ALSA. Everything else must be linked in or packed.
SYSTEM_LIBRARIES=(
  libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 ld-linux-x86-64.so.2
  libvulkan.so.1 libX11.so.6 libXcursor.so.1 libasound.so.2
)
# uv is the official prebuilt release, not compiled here: Rust links libgcc_s, which glibc itself needs
# and every glibc system has
UV_LIBRARIES=(libgcc_s.so.1)
GLIBC_MAX=2.35

forbidden() { grep -Eiq "$FORBIDDEN" <<< "$1" || grep -Eq "$FORBIDDEN_CASED" <<< "$1"; }

sha256() {
  if command -v sha256sum > /dev/null; then sha256sum "$1" | awk '{print $1}'; else shasum -a 256 "$1" | awk '{print $1}'; fi
}

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
stage="$OUT_DIR/$NAME"
rm -rf "$stage"
mkdir -p "$stage"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

copy_into() {  # <source file> <relative target>
  [[ -f "$1" ]] || fail "missing: $1"
  mkdir -p "$(dirname "$stage/$2")"
  cp -p "$1" "$stage/$2"
}
copy_from_repo() { copy_into "$ROOT/$1" "$1"; }

# the entry points and the player's guide
for file in Setup-GothicAI.sh Start-GothicAI.sh START_HERE.linux.ru.md LICENSE scripts/linux/common.sh; do
  copy_from_repo "$file"
done

# the AI services and the launcher: what `uv sync --locked` and `uv run` need, without tests or caches
for project in "${PYTHON_PROJECTS[@]}"; do
  for file in pyproject.toml uv.lock .python-version README.md; do
    if [[ -f "$ROOT/$project/$file" ]]; then copy_from_repo "$project/$file"; fi
  done
  [[ -d "$ROOT/$project/src" ]] || fail "missing: $ROOT/$project/src"
  while IFS= read -r -d '' file; do
    relative="${file#"$ROOT/"}"
    forbidden "$relative" || copy_from_repo "$relative"
  done < <(find "$ROOT/$project/src" -type f -print0)
done

# the game, its base and video converter, and every other file of their output folder
for needed in "$GAME/Gothic2Notr" "$GAME/content-convert" "$STORYGRAPH/storygraph"; do
  [[ -x "$ROOT/$needed" ]] || fail "not built: $ROOT/$needed (scripts/linux/build.sh)"
done
while IFS= read -r -d '' file; do
  copy_from_repo "${file#"$ROOT/"}"
done < <(find "$ROOT/$GAME" -maxdepth 1 -type f -print0)
copy_from_repo "$STORYGRAPH/storygraph"

# uv, pinned and checked by the caller
mkdir -p "$work/uv" "$stage/tools/uv"
tar -xzf "$UV_TAR" -C "$work/uv"
uv_binary="$(find "$work/uv" -type f -name uv | head -1)"
[[ -n "$uv_binary" ]] || fail "no uv in $UV_TAR"
install -m 755 "$uv_binary" "$stage/tools/uv/uv"
for license in LICENSE-MIT LICENSE-APACHE; do copy_into "$UV_LICENSES/$license" "tools/uv/$license"; done

# licenses: the engine, its vendored libraries, what CMake fetched, the GCC runtime
licenses=()
add_license() {  # <source file> <relative path under licenses/>
  copy_into "$1" "licenses/$2"
  licenses+=("$2")
}
add_license "$ROOT/OpenGothic/LICENSE" OpenGothic/LICENSE
add_license "$ROOT/OpenGothic/lib/VENDORED.md" OpenGothic/lib/VENDORED.md
while IFS= read -r -d '' file; do
  if grep -Eiq "$LICENSE_NAME" <<< "$(basename "$file")"; then
    add_license "$file" "OpenGothic/lib/${file#"$ROOT/OpenGothic/lib/"}"
  fi
done < <(find "$ROOT/OpenGothic/lib" -type f -print0 | sort -z)
fetched=ixwebsocket  # linked into the game; the DirectX shader compiler of Windows is not fetched here
dep="$ROOT/OpenGothic/build/_deps/$fetched-src"
found=0
while IFS= read -r -d '' file; do
  if grep -Eiq "$LICENSE_NAME" <<< "$(basename "$file")"; then
    add_license "$file" "$fetched/${file#"$dep/"}"
    found=1
  fi
done < <(find "$dep" -type f -print0 2> /dev/null | sort -z)
((found)) || fail "no license file in $dep"
cat > "$work/GCC-Runtime.txt" << EOF
libstdc++ and libgcc of ${COMPILER:-GCC} are linked statically into Gothic2Notr, content-convert and
storygraph. They are licensed under the GNU GPL version 3 with the GCC Runtime Library Exception, which
permits this: https://www.gnu.org/licenses/gcc-exception-3.1.html
EOF
add_license "$work/GCC-Runtime.txt" GCC-Runtime.txt
cat > "$work/SOURCE.txt" << EOF
Built from $REPOSITORY at commit $COMMIT.
The complete corresponding source of every binary here, including OpenAL Soft linked statically into
Gothic2Notr (LGPL, OpenGothic/lib/Tempest/Engine/thirdparty/openal-soft/COPYING), is that commit of the
repository; scripts/linux/build.sh rebuilds and relinks it.
EOF
add_license "$work/SOURCE.txt" SOURCE.txt

# refuse anything private or build-only
bad=()
while IFS= read -r -d '' file; do
  relative="${file#"$stage/"}"
  if forbidden "$relative"; then bad+=("$relative"); fi
done < <(find "$stage" -type f -print0 | sort -z)
((${#bad[@]} == 0)) || fail "not for the archive: ${bad[*]}"

# x86-64 ELF binaries that need only the system's libraries, glibc up to 2.35 and no libstdc++
is_elf() { [[ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" == 7f454c46 ]]; }
version_needs() {  # the version names of the "Version needs" section, one per line
  "$READELF" -V --wide "$1" | awk '
    /^Version needs section/ { needs = 1; next }
    /^Version (symbols|definition) section/ { needs = 0 }
    needs && match($0, /Name: [^ ]+/) { print substr($0, RSTART + 6, RLENGTH - 6) }'
}
binaries="$work/binaries.tsv"
: > "$binaries"
while IFS= read -r -d '' file; do
  is_elf "$file" || continue
  relative="${file#"$stage/"}"
  machine="$("$READELF" -h --wide "$file" | sed -n 's/^ *Machine: *//p')"
  [[ "$machine" == *X86-64* ]] || fail "not an x86-64 binary: $relative ($machine)"
  needed="$("$READELF" -d --wide "$file" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')"
  allowed=("${SYSTEM_LIBRARIES[@]}")
  if [[ "$relative" == tools/uv/uv ]]; then allowed+=("${UV_LIBRARIES[@]}"); fi
  for library in $needed; do
    known=0
    for system in "${allowed[@]}"; do [[ "$library" == "$system" ]] && known=1; done
    ((known)) || fail "$relative needs $library, which is neither linked in nor part of the player's system"
  done
  versions="$(version_needs "$file")"
  glibc=''
  for version in $versions; do
    case "$version" in
      GLIBCXX_* | CXXABI_*)
        fail "$relative needs libstdc++ ($version): link it statically (-static-libstdc++)" ;;
      GLIBC_[0-9]*)
        number="${version#GLIBC_}"
        newest="$(printf '%s\n%s\n' "$GLIBC_MAX" "$number" | sort -V | tail -1)"
        [[ "$newest" == "$GLIBC_MAX" ]] || fail "$relative needs $version, newer than glibc $GLIBC_MAX"
        if [[ -z "$glibc" ]] || [[ "$(printf '%s\n%s\n' "$glibc" "$number" | sort -V | tail -1)" == "$number" ]]; then
          glibc="$number"
        fi ;;
      GLIBC_*) fail "$relative needs $version, which only glibc newer than $GLIBC_MAX or its internals have" ;;
    esac
  done
  printf '%s\t%s\t%s\t%s\n' "$relative" "$machine" "$(tr '\n' ' ' <<< "$needed")" "$glibc" >> "$binaries"
done < <(find "$stage" -type f -print0 | sort -z)
for needed in "$GAME/Gothic2Notr" "$GAME/content-convert" "$STORYGRAPH/storygraph" tools/uv/uv; do
  cut -f1 "$binaries" | grep -qxF "$needed" || fail "$needed is not an ELF binary in the archive"
done

# the archive, its manifest and their checksums
archive="$OUT_DIR/$NAME.tar.gz"
rm -f "$archive"
if tar --version 2> /dev/null | grep -q 'GNU tar'; then
  tar --sort=name --owner=0 --group=0 --numeric-owner -czf "$archive" -C "$OUT_DIR" "$NAME"
else  # bsdtar (a local run on a Mac): without the AppleDouble ._ files
  COPYFILE_DISABLE=1 tar -czf "$archive" -C "$OUT_DIR" "$NAME"
fi
listing="$(tar -tvzf "$archive")"
for needed in Setup-GothicAI.sh Start-GothicAI.sh tools/uv/uv "$GAME/Gothic2Notr" "$GAME/content-convert" \
              "$STORYGRAPH/storygraph"; do
  grep -Eq "^-rwx.* $NAME/$needed\$" <<< "$listing" || fail "the archive lacks an executable $NAME/$needed"
done
for needed in server/uv.lock launcher/core/uv.lock START_HERE.linux.ru.md scripts/linux/common.sh; do
  grep -Eq " $NAME/$needed\$" <<< "$listing" || fail "the archive lacks $NAME/$needed"
done

files="$work/files.tsv"
: > "$files"
while IFS= read -r -d '' file; do
  printf '%s\t%s\t%s\n' "${file#"$stage/"}" "$(wc -c < "$file" | tr -d ' ')" "$(sha256 "$file")" >> "$files"
done < <(find "$stage" -type f -print0 | sort -z)
manifest="$OUT_DIR/$NAME.manifest.json"
printf '%s\n' "${licenses[@]}" > "$work/licenses.txt"
NAME="$NAME" REPOSITORY="$REPOSITORY" COMMIT="$COMMIT" RUN_URL="$RUN_URL" COMPILER="$COMPILER" \
  GLIBC_MAX="$GLIBC_MAX" VULKAN_SDK_VERSION="$VULKAN_SDK_VERSION" UV_VERSION="$UV_VERSION" \
  python3 -I - "$manifest" "$files" "$binaries" "$work/licenses.txt" << 'EOF'
import json, os, sys
from datetime import datetime, timezone

manifest, files, binaries, licenses = sys.argv[1:]
rows = [line.rstrip("\n").split("\t") for line in open(files, encoding="utf-8")]
elf = {}
for line in open(binaries, encoding="utf-8"):
    path, machine, needed, glibc = line.rstrip("\n").split("\t")
    elf[path] = {"machine": machine, "needed": needed.split(), "glibc": glibc or None}
listed = []
for path, size, digest in rows:
    entry = {"path": path, "bytes": int(size), "sha256": digest}
    if path in elf:
        entry.update(elf[path])
    listed.append(entry)
env = os.environ
data = {
    "name": env["NAME"],
    "repository": env["REPOSITORY"],
    "commit": env["COMMIT"],
    "run": env["RUN_URL"],
    "built_utc": datetime.now(timezone.utc).isoformat(),
    "compiler": env["COMPILER"],
    "static_runtime": "libstdc++, libgcc",
    "glibc_max": env["GLIBC_MAX"],
    "vulkan_sdk": env["VULKAN_SDK_VERSION"],
    "uv": env["UV_VERSION"],
    "files": listed,  # ELF binaries also with their machine, NEEDED libraries and newest GLIBC_ version
    "licenses": [line.strip() for line in open(licenses, encoding="utf-8") if line.strip()],
    "verified": "built and checked on ubuntu-22.04; gameplay, graphics, sound and microphone not yet tested on a real Linux PC",
}
with open(manifest, "w", encoding="utf-8") as out:
    json.dump(data, out, ensure_ascii=False, indent=2)
    out.write("\n")
EOF
(
  cd "$OUT_DIR"
  for file in "$NAME.tar.gz" "$NAME.manifest.json"; do printf '%s  %s\n' "$(sha256 "$file")" "$file"; done
) > "$OUT_DIR/SHA256SUMS"
count="$(wc -l < "$files" | tr -d ' ')"
rm -rf "$stage"
echo "packed $count files:"
for file in "$archive" "$manifest" "$OUT_DIR/SHA256SUMS"; do
  printf '  %s (%s bytes)\n' "$(basename "$file")" "$(wc -c < "$file" | tr -d ' ')"
done
