#!/usr/bin/env bash
UA="Mozilla/5.0"
echo "=== knik0/faad2 default branch ==="
curl -s -A "$UA" "https://api.github.com/repos/knik0/faad2" | grep -oE '"default_branch": *"[^"]+"'
echo "=== libfaad/*.c and *.h ==="
for br in master main; do
  echo "--- branch $br ---"
  curl -s -A "$UA" "https://api.github.com/repos/knik0/faad2/git/trees/$br?recursive=1" \
    | grep -oE '"path": *"libfaad/[^"]+\.(c|h)"' | sed 's/.*"path": *"//; s/"$//' | sort
  echo "--- include ---"
  curl -s -A "$UA" "https://api.github.com/repos/knik0/faad2/git/trees/$br?recursive=1" \
    | grep -oE '"path": *"include/[^"]+"' | sed 's/.*"path": *"//; s/"$//' | sort
done
