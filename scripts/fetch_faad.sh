#!/usr/bin/env bash
set -e
PROJ=/mnt/c/Users/monke/Projects/ulti-sound-3ds

export DEBIAN_FRONTEND=noninteractive
command -v git >/dev/null 2>&1 || { apt-get update -y; apt-get install -y --no-install-recommends git ca-certificates; }

rm -rf /tmp/faad2
git clone --depth 1 https://github.com/knik0/faad2 /tmp/faad2

mkdir -p "$PROJ/source/faad"
# copy the library sources + codebook subdir (headers)
cp -r /tmp/faad2/libfaad/. "$PROJ/source/faad/"
# public API header
cp /tmp/faad2/include/neaacdec.h "$PROJ/include/"

# faad2 ships its own decoder.c; keep it (our file is now usdec.c) but the
# object name 'decoder.o' is now unique to faad2 -- fine.

echo "=== source/faad contents (top) ==="
ls "$PROJ/source/faad" | sort | head -40
echo "=== codebook ==="
ls "$PROJ/source/faad/codebook" 2>/dev/null | head
echo "neaacdec.h: $(wc -l < "$PROJ/include/neaacdec.h") lines"
echo FAAD_DONE
