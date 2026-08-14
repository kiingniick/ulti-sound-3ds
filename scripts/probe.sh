#!/usr/bin/env bash
# Probe which devkitPro endpoints are reachable from this machine.
UA="Mozilla/5.0 (X11; Linux x86_64)"
urls=(
  "https://apt.devkitpro.org/"
  "https://apt.devkitpro.org/install-devkitpro-pacman"
  "https://apt.devkitpro.org/dists/stable/InRelease"
  "https://apt.devkitpro.org/devkitpro-keyring.gpg"
  "https://pacman.devkitpro.org/"
  "https://pacman.devkitpro.org/devkitpro-keyring-1.2.1-1-any.pkg.tar.xz"
  "https://downloads.devkitpro.org/"
  "https://github.com/devkitPro"
)
for u in "${urls[@]}"; do
  code=$(curl -s -o /dev/null -w "%{http_code}" -A "$UA" --max-time 20 "$u")
  echo "$code  $u"
done
