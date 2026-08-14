#!/usr/bin/env bash
set -e
SRC="/mnt/c/Users/monke/.cursor/projects/c-Users-monke-Projects-ulti-sound-3ds/assets/icon_src.png"
DST="/mnt/c/Users/monke/Projects/ulti-sound-3ds/icon.png"

if ! command -v convert >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -y
  apt-get install -y --no-install-recommends imagemagick
fi

# SMDH icons must be exactly 48x48, 24-bit RGB PNG.
convert "$SRC" -background white -alpha remove -alpha off -resize 48x48\! -type TrueColor "$DST"
identify "$DST"
echo "ICON_DONE"
