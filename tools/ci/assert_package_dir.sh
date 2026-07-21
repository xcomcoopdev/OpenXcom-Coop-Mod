#!/bin/sh
# Asserts a staged package directory is shippable, before it gets archived.
# (The Windows x64 job checks the finished .zip instead - see assert_package.ps1;
# the ubuntu:20.04 container here has no unzip/python3 to list an archive with.)
#
#   1. Too much: UFO/ and TFTD/ are whitelisted down to their multiplayer/
#      subdirectory (plus the repo's own README.txt). Anything else under them is
#      licensed retail X-COM data.
#   2. Too little: every package must carry the coop art (Globe's ctor loads
#      multiplayer/base.png unguarded, so a package without it crashes the moment a
#      player starts a new game - nightly 8.4.13203), plus rendezvous.json (without
#      it the Official server list comes up empty - the WinXP zip shipped that way
#      through 8.4.13203) and LICENSE.txt.
#
# Usage: tools/ci/assert_package_dir.sh <staged-dir>
set -eu
d="$1"

bad=$(find "$d/UFO" "$d/TFTD" -mindepth 1 -maxdepth 1 ! -name multiplayer ! -name README.txt 2>/dev/null || true)
if [ -n "$bad" ]; then
  echo "licensed retail data leaked into $d:"; echo "$bad"; exit 1
fi

for f in UFO/multiplayer/base.png TFTD/multiplayer/base.png; do
  if [ ! -f "$d/$f" ]; then
    echo "$d is missing $f - new game would crash in Globe's ctor"; exit 1
  fi
done

for f in rendezvous.json LICENSE.txt; do
  if [ ! -f "$d/$f" ]; then
    echo "$d is missing $f"; exit 1
  fi
done

echo "package OK ($d): coop art + rendezvous.json + LICENSE.txt present, no licensed retail data"
