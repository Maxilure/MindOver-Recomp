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
#         prerequisites/            Windows vendor installers and VS layout
#         source/                    every tracked file of this repo (no .git,
#                                    no checked-out submodule)
#           build-tools/             Windows CMake, Ninja and SDK archive
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
#   --windows-vs-layout <dir>             prepared Build Tools 2022 offline layout
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
vs_layout=""
while [ $# -gt 0 ]; do
  case "$1" in
    --allow-dirty) allow_dirty=1 ;;
    --out) out_base=$2; shift ;;
    --system) system_override=$2; shift ;;
    --launcher) launcher_file=$2; shift ;;
    --windows-vs-layout) vs_layout=$2; shift ;;
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
if [ "$system" = windows ] && { [ -z "$vs_layout" ] || [ ! -f "$vs_layout/vs_buildtools.exe" ] ||
                                [ ! -f "$vs_layout/Catalog.json" ] || [ ! -f "$vs_layout/Response.json" ]; }; then
  echo "Windows packaging needs --windows-vs-layout <dir> (see docs/01-building.md)."
  exit 1
fi

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
if [ "$system" = windows ]; then
  cp "out/build/launcher-release/mind_over_recomp.exe" "$stage/$name$exe"
else
  cp "out/build/launcher-release/crash_mom_launcher$exe" "$stage/$name$exe"
fi
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

if [ "$system" = windows ]; then
  echo "== Staging the Windows build tools"
  if [ "$host" = windows ]; then
    python tools/release/windows_prerequisites.py "$(cygpath -w "$stage")" "$(cygpath -w "$vs_layout")"
  else
    python3 tools/release/windows_prerequisites.py "$stage" "$vs_layout"
  fi

  # Keep Git metadata for the pin checks and SDK patches. Materialize symlinks
  # as link text just as Git for Windows does, so setup can repair them.
  sdk_stage="$out/stage/rexglue-sdk"
  git -c core.symlinks=false -c core.autocrlf=false -c core.longpaths=true clone --depth 1 \
      --branch "$sdk_tag" --recurse-submodules --shallow-submodules --jobs 6 \
      "$sdk_url" "$sdk_stage"
  [ "$(git -C "$sdk_stage" rev-parse HEAD)" = "$sdk_commit" ]
  sdk_config='git config core.filemode false && git config core.symlinks false &&
              git config core.autocrlf false && git config core.longpaths true'
  (cd "$sdk_stage" && sh -c "$sdk_config")
  git -C "$sdk_stage" submodule foreach --recursive "$sdk_config"
  tar -C "$out/stage" -czf "$stage/source/build-tools/rexglue-sdk.tar.gz" rexglue-sdk
fi

# 3. The readme, with the version filled in.
readme=tools/release/READ_ME_FIRST.txt
[ $system = windows ] && readme=tools/release/READ_ME_FIRST_windows.txt
sed "s/@VERSION@/$version/" "$readme" > "$stage/READ ME FIRST.txt"

# 4. The archive and the update feed.
echo "== Packing"
if [ $system = linux ]; then
  tar -C "$out/stage" -czf "$out/$archive" "$name"
else
  # Stream the Windows ZIP with ZIP64 support for the offline layout.
  # DATES: a zip entry's date has NO time zone, and Windows reads it as local
  # time. Dated "now" in UTC (GitHub's machines), the files of 0.2.0 looked up
  # to ~10 hours in the FUTURE west of UTC on release day, and the build gave
  # up ("build.ninja still dirty after 100 tries", GitHub issue #7). Every
  # entry is dated ONE DAY AGO instead: in the past in every time zone (they
  # span UTC-12..UTC+14). Fresh installs don't need real dates, and updates
  # re-date what changed themselves (launcher/update.cpp, the swap).
  python_cmd=python3
  [ "$host" != windows ] || python_cmd=python
  "$python_cmd" - "$out/stage" "$name" "$out/$archive" <<'PY'
import os, shutil, sys, time, zipfile
root, name, target = sys.argv[1], sys.argv[2], sys.argv[3]
day_ago = time.gmtime(time.time() - 24 * 3600)[:6]
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
    for folder, _, files in os.walk(os.path.join(root, name)):
        for f in files:
            full = os.path.join(folder, f)
            info = zipfile.ZipInfo.from_file(full, os.path.relpath(full, root), strict_timestamps=False)
            info.date_time = day_ago
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(full, "rb") as data, z.open(info, "w") as entry:
                shutil.copyfileobj(data, entry)
PY
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
