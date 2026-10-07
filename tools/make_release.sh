#!/usr/bin/env bash
# =============================================================================
# make_release.sh: pack a release of the port (Linux)
# =============================================================================
#
# A release is what a player downloads: the LAUNCHER (ready-built) plus the
# port's SOURCE, which the launcher's Setup tab builds on the player's own
# computer, from their own disc (the built game can't be handed out: it IS
# the game's code, translated). Nothing from the game disc is in it.
#
#   out/release/<version>/
#     MindOverRecomp-<version>-linux-x86_64.tar.gz   the download (Windows: -windows-
#                                              x86_64.zip), unpacking to:
#       Mind over Recomp/
#         Mind over Recomp           the launcher (Windows: ... .exe)
#         READ ME FIRST.txt          tools/release/READ_ME_FIRST[_windows].txt
#         source/                    every tracked file of this repo (no .git,
#                                    no submodule: Setup downloads the SDK)
#           RELEASE.toml             version + the exact ReXGlue SDK to use
#     feed-<system>.toml             this system's lines of the update feed:
#                                    <system>_archive / <system>_sha256
#     release.toml                   the UPDATE FEED: version, date + every
#                                    feed-*.toml in the folder (launcher/update.h)
#
# Publishing = a GitHub release tagged v<version> with BOTH files attached:
# launchers look for releases/latest/download/release.toml, then download the
# archive named in it from the same release.
#
# Linux: .tar.gz, not .zip (a zip can lose the launcher's "may run" flag:
# double-click then does nothing). Windows: .zip, its usual format (no such
# flag there). On Windows this script runs in Git for Windows' bash, inside
# Visual Studio's x64 environment (clang needs its libraries): GitHub
# Actions' release workflow sets that up; by hand, from a "x64 Native Tools"
# prompt: "C:\Program Files\Git\bin\bash.exe" tools/make_release.sh
#
# Usage:
#   tools/make_release.sh                 version from VERSION.txt, needs a clean tree
#   tools/make_release.sh --allow-dirty   pack the working tree as it is (tests:
#                                         uncommitted and new untracked files too)
#   tools/make_release.sh --out <dir>     instead of out/release
#   tools/make_release.sh --system windows --launcher <crash_mom_launcher.exe>
#                                         package the OTHER system's release here,
#                                         with a launcher built there (tests:
#                                         the Windows test PC has no git clone)
#
# The launcher is linked with libstdc++ / libgcc built in, but still needs a
# glibc at least as new as the machine that built it: real releases are built
# by GitHub Actions on Ubuntu 24.04 (.github/workflows/release.yml), the
# oldest system that can build the game anyway (clang 20+). A local run makes
# a launcher only for systems as new as this one (fine for tests).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

allow_dirty=0
out_base=out/release
system_override=""
launcher_file=""
while [ $# -gt 0 ]; do
  case "$1" in
    --allow-dirty) allow_dirty=1 ;;
    --out) out_base=$2; shift ;;
    --system) system_override=$2; shift ;;
    --launcher) launcher_file=$2; shift ;;
    *) echo "unknown option $1"; exit 1 ;;
  esac
  shift
done

version=$(tr -d '[:space:]' < VERSION.txt)
# The PROJECT's name, not the game's: the port isn't an official product
# (0.1.0-alpha's first build used the game's name; the launcher's updater
# finds the new folder and launcher by their contents, not their names).
name="Mind over Recomp"
out=$out_base/$version
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) host=windows ;;
  *) host=linux ;;
esac
system=${system_override:-$host}
if [ "$system" != "$host" ] && [ -z "$launcher_file" ]; then
  echo "--system $system needs --launcher <its launcher, built on $system>"
  exit 1
fi
case "$system" in
  windows) exe=".exe"; archive="MindOverRecomp-$version-windows-x86_64.zip" ;;
  *) exe=""; archive="MindOverRecomp-$version-linux-x86_64.tar.gz" ;;
esac

# A real release is exactly a commit: refuse uncommitted changes (the SDK
# submodule's applied patches don't count: they're not part of the repo).
dirty=$(git status --porcelain --ignore-submodules=dirty)
if [ -n "$dirty" ] && [ $allow_dirty -eq 0 ]; then
  echo "Uncommitted changes (commit first, or --allow-dirty for a test release):"
  echo "$dirty"
  exit 1
fi

# The ReXGlue SDK this commit uses: its URL, commit and (nightly) tag.
sdk_url=$(git config -f .gitmodules submodule.thirdparty/rexglue-sdk.url)
sdk_commit=$(git ls-tree HEAD thirdparty/rexglue-sdk | awk '{print $3}')
sdk_tag=$(git -C thirdparty/rexglue-sdk tag --points-at "$sdk_commit" 2>/dev/null | head -1 || true)
if [ -z "$sdk_tag" ]; then
  # A shallow submodule checkout (GitHub Actions) has no tags: ask the SDK's
  # repository which tag points at that commit.
  sdk_tag=$(git ls-remote --tags "$sdk_url" | awk -v c="$sdk_commit" '$1 == c {print $2}' \
            | sed 's|refs/tags/||; s|\^{}||' | head -1)
fi
if [ -z "$sdk_tag" ]; then
  echo "The SDK commit $sdk_commit has no tag: Setup downloads by tag."
  exit 1
fi

echo "== Release $version for $system (SDK $sdk_tag)"
# The other system's files of the same version stay (a release has both).
rm -rf "$out/stage" "$out/$archive" "$out/feed-$system.toml"
stage="$out/stage/$name"
mkdir -p "$stage/source"

# 1. The launcher, built on its own (static libstdc++ / libgcc). CC / CXX
#    choose the compiler (default clang; the GitHub Actions workflow passes
#    its own: .github/workflows/release.yml).
echo "== Building the launcher"
if [ -n "$launcher_file" ]; then
  echo "   (using $launcher_file)"
  cp "$launcher_file" "$stage/$name$exe"
else
if [ $system = linux ]; then
  link_flags="-static-libstdc++ -static-libgcc"
else
  link_flags=""  # Windows: the C runtime is built in already (launcher/CMakeLists.txt)
fi
cmake -S launcher -B out/build/launcher-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
      -DCMAKE_EXE_LINKER_FLAGS="$link_flags" > /dev/null
cmake --build out/build/launcher-release > /dev/null
cp "out/build/launcher-release/crash_mom_launcher$exe" "$stage/$name$exe"
[ $system = linux ] && strip "$stage/$name"
fi

# 2. The source: tracked files (+ untracked, not ignored ones with
#    --allow-dirty), never the submodule's folder, never deleted files.
echo "== Copying the source"
if [ $allow_dirty -eq 1 ]; then
  git ls-files --cached --others --exclude-standard
else
  git ls-files --cached
fi | grep -v '^thirdparty/rexglue-sdk$' | while IFS= read -r f; do
  [ -f "$f" ] && printf '%s\0' "$f"
done | tar --null -T - -cf - | tar -xf - -C "$stage/source"

cat > "$stage/source/RELEASE.toml" <<EOF
# This release's version and the exact ReXGlue SDK it builds with
# (written by tools/make_release.sh; read by the launcher's Setup and Update).
version = "$version"
sdk_url = "$sdk_url"
sdk_tag = "$sdk_tag"
sdk_commit = "$sdk_commit"
EOF

# 3. The readme, with the version filled in.
readme=tools/release/READ_ME_FIRST.txt
[ $system = windows ] && readme=tools/release/READ_ME_FIRST_windows.txt
sed "s/@VERSION@/$version/" "$readme" > "$stage/READ ME FIRST.txt"

# 4. The archive and the update feed.
echo "== Packing"
if [ $system = linux ]; then
  tar -C "$out/stage" -czf "$out/$archive" "$name"
elif [ $host = linux ]; then
  # A Windows zip made on Linux (python's zipfile: no extra tool needed).
  python3 - "$out/stage" "$name" "$out/$archive" <<'PY'
import os, sys, zipfile
root, name, target = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
    for folder, _, files in os.walk(os.path.join(root, name)):
        for f in files:
            full = os.path.join(folder, f)
            z.write(full, os.path.relpath(full, root))
PY
else
  powershell -NoProfile -Command "Compress-Archive -Force -Path '$(cygpath -w "$out/stage/$name")' -DestinationPath '$(cygpath -w "$out/$archive")'"
fi
sha=$(sha256sum "$out/$archive" | awk '{print $1}')
cat > "$out/feed-$system.toml" <<EOF
${system}_archive = "$archive"
${system}_sha256 = "$sha"
EOF
{
  echo "# The update feed of Mind over Recomp (launcher/update.h)."
  echo "version = \"$version\""
  echo "date = \"$(date -u +%Y-%m-%d)\""
  cat "$out"/feed-*.toml
} > "$out/release.toml"
rm -rf "$out/stage"

echo "== Done: $out/"
ls -la "$out"
