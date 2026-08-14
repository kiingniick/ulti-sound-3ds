#!/usr/bin/env bash
UA="Mozilla/5.0"
echo "=== ultraembedded/libhelix-aac (recursive tree) ==="
curl -s -A "$UA" "https://api.github.com/repos/ultraembedded/libhelix-aac/git/trees/master?recursive=1" \
  | grep -oE '"path": *"[^"]+\.(c|h)"' | sed 's/.*"path": *"//; s/"$//' | sort | head -80
echo
echo "=== pschatzmann/arduino-libhelix src/libhelix-aac ==="
curl -s -A "$UA" "https://api.github.com/repos/pschatzmann/arduino-libhelix/contents/src/libhelix-aac" \
  | grep -oE '"name": *"[^"]+"' | sed 's/.*"name": *"//; s/"$//' | sort | head -80
