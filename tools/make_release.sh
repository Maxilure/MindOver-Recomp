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
program_dir=""  # --program: a Windows game build folder (READY-BUILT release, below)
python_dir=""   # --python: Python's embeddable package (with --program)
kit_dir=""   # --kit: the Windows build kit (release workflow, job windows-sdk)
while [ $# -gt 0 ]; do
  case "$1" in
    --allow-dirty) allow_dirty=1 ;;
    --out) out_base=$2; shift ;;
    --system) system_override=$2; shift ;;
    --launcher) launcher_file=$2; shift ;;
    --kit) kit_dir=$2; shift ;;
    --program) program_dir=$2; shift ;;
    --python) python_dir=$2; shift ;;
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

# 2b. THE BUILD KIT (Windows, launcher/setup.cpp "THE BUILD KIT"): the SDK
#     already built + patched by GitHub Actions (thirdparty/rexglue-install,
#     with share/mindoverrecomp/prebuilt.txt and sdk-headers/) and our pinned
#     tools (buildtools/cmake, ninja, glslc, python). The player's PC then
#     builds only the game, with nothing taken from its own PATH.
if [ -n "$kit_dir" ]; then
  for need in rexglue-install/share/mindoverrecomp/prebuilt.txt buildtools/cmake/bin/cmake.exe \
              buildtools/ninja/ninja.exe buildtools/glslc/glslc.exe buildtools/python/python.exe; do
    [ -f "$kit_dir/$need" ] || { echo "The kit has no $need"; exit 1; }
  done
  echo "== Adding the build kit ($(du -sh "$kit_dir" | cut -f1))"
  mkdir -p "$stage/source/thirdparty"
  cp -a "$kit_dir/rexglue-install" "$stage/source/thirdparty/rexglue-install"
  cp -a "$kit_dir/buildtools" "$stage/source/buildtools"
  cat > "$stage/source/buildtools/kit.txt" <<EOF
The build kit of Mind over Recomp $version: the SDK ($sdk_tag + our patches) is
ready-built in thirdparty/rexglue-install, and this folder holds the CMake,
Ninja, glslc and Python every build step uses. Made by GitHub Actions.
EOF
fi

# 2c. READY-BUILT (Windows, launcher/setup.cpp "READY-BUILT"; what Unleashed
#     Recompiled and Zelda 64: Recompiled do): program/ = the game already
#     built (on a PC with the disc: the build needs default.xex, which never
#     goes online), next to the installer. Only the build's OWN files: the
#     exe (installed name), the SDK's DLLs, the exe's .pdb (crash reports),
#     assets/ and languages/. Never the icon (crash_mom.ico/.png are made
#     from the player's disc) and never anything from game/.
if [ -n "$program_dir" ]; then
  [ -n "$python_dir" ] && [ -f "$python_dir/python.exe" ] || { echo "--program needs --python <embeddable Python folder>"; exit 1; }
  [ -f "$program_dir/crash_mom.exe" ] || { echo "$program_dir has no crash_mom.exe"; exit 1; }
  echo "== Adding the ready-built game from $program_dir"
  p="$stage/program"
  mkdir -p "$p"
  cp "$program_dir/crash_mom.exe" "$p/Crash Mind over Mutant.exe"
  cp "$program_dir/crash_mom.pdb" "$p/"
  for f in "$program_dir"/*.dll; do cp "$f" "$p/"; done
  cp -r "$program_dir/assets" "$p/assets"
  [ -d "$program_dir/languages" ] && cp -r "$program_dir/languages" "$p/languages"
  if find "$p" -iname '*.ico' -o -iname 'crash_mom.png' -o -iname '*.xex' -o -iname '*.rcf' | grep -q .; then
    echo "program/ holds game-derived files: refused"; exit 1
  fi
  mkdir -p "$stage/source/buildtools"
  cp -r "$python_dir" "$stage/source/buildtools/python"
  cp tools/release/BUILDTOOLS_LICENSES.txt "$stage/source/buildtools/LICENSES.txt"
  cat > "$stage/source/buildtools/prebuilt.txt" <<EOF
Mind over Recomp $version comes READY-BUILT: program/ next to the installer
is the game already built, and the installer copies it in. Nothing is
compiled on this PC. python/ runs the disc tools (tools/xiso_extract.py).
EOF
fi

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
  # DATES: a zip entry's date has NO time zone, and Windows reads it as local
  # time. Dated "now" in UTC (GitHub's machines), the files of 0.2.0 looked up
  # to ~10 hours in the FUTURE west of UTC on release day, and the build gave
  # up ("build.ninja still dirty after 100 tries", GitHub issue #7). Every
  # entry is dated ONE DAY AGO instead: in the past in every time zone (they
  # span UTC-12..UTC+14). Fresh installs don't need real dates, and updates
  # re-date what changed themselves (launcher/update.cpp, the swap).
  python3 - "$out/stage" "$name" "$out/$archive" <<'PY'
import os, sys, time, zipfile
root, name, target = sys.argv[1], sys.argv[2], sys.argv[3]
day_ago = time.gmtime(time.time() - 24 * 3600)[:6]
with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
    for folder, _, files in os.walk(os.path.join(root, name)):
        for f in files:
            full = os.path.join(folder, f)
            info = zipfile.ZipInfo.from_file(full, os.path.relpath(full, root))
            info.date_time = day_ago
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(full, "rb") as data:
                z.writestr(info, data.read())
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
  # The self-update's promise (launcher/update.h kUpdaterVersion): raise it
  # only when a release changes what an installed Mind over Recomp must do to
  # update itself; older ones then send players to the manual download.
  echo "updater = 1"
  echo "date = \"$(date -u +%Y-%m-%d)\""
  cat "$out"/feed-*.toml
} > "$out/release.toml"
rm -rf "$out/stage"

echo "== Done: $out/"
ls -la "$out"
