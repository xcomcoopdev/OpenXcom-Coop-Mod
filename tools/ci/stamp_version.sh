#!/bin/sh
# Stamps the computed release version into src/version.h before a build.
#
# Every build job has to run this, not just the x64 one: the WinXP and Linux
# binaries used to ship unstamped, reporting src/version.h's checked-in
# "Extended 8.4.2 (v2025-10-06)" from inside a zip named after the real version.
# POSIX sh + sed because the WinXP job builds inside an ubuntu:20.04 container
# that has no pwsh.
#
# Usage: sh tools/ci/stamp_version.sh <version> <git-suffix> [version.h]
set -eu
v="$1"
suffix="$2"
f="${3:-src/version.h}"

maj="${v%%.*}"; rest="${v#*.}"; min="${rest%%.*}"; pat="${rest#*.}"
case "$maj$min$pat" in *[!0-9]*) echo "version '$v' is not <major>.<minor>.<patch>"; exit 1;; esac

sed -i.bak \
  -e "s|OPENXCOM_VERSION_SHORT \"[^\"]*\"|OPENXCOM_VERSION_SHORT \"Extended $v\"|" \
  -e "s|OPENXCOM_VERSION_LONG \"[^\"]*\"|OPENXCOM_VERSION_LONG \"$v.0\"|" \
  -e "s|OPENXCOM_VERSION_NUMBER [0-9, ]*|OPENXCOM_VERSION_NUMBER $maj,$min,$pat,0|" \
  -e "s|OPENXCOM_VERSION_GIT \"[^\"]*\"|OPENXCOM_VERSION_GIT \"$suffix\"|" \
  "$f"
rm -f "$f.bak"

grep -q "OPENXCOM_VERSION_SHORT \"Extended $v\"" "$f" || { echo "stamp failed in $f"; exit 1; }
echo "stamped $f: $v$suffix"
grep OPENXCOM_VERSION_ "$f"
