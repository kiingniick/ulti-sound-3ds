#!/usr/bin/env bash
set -e
docker run --rm devkitpro/devkitarm:latest bash -lc '
  echo "DEVKITPRO=$DEVKITPRO"
  echo "DEVKITARM=$DEVKITARM"
  echo "--- tools ---"
  for t in arm-none-eabi-gcc 3dsxtool smdhtool bannertool tex3ds picasso; do
    printf "%s: " "$t"; (command -v "$t" || echo MISSING)
  done
  echo "--- libctru ---"
  ls $DEVKITPRO/libctru/lib/libctru.a 2>/dev/null || echo "NO libctru"
  echo "--- citro2d / citro3d ---"
  ls $DEVKITPRO/libctru/lib/libcitro2d.a 2>/dev/null || echo "NO libcitro2d (checking portlibs)"
  ls $DEVKITPRO/portlibs/3ds/lib/libcitro2d.a 2>/dev/null || echo "NO libcitro2d in portlibs"
  ls $DEVKITPRO/portlibs/3ds/lib/libcitro3d.a 2>/dev/null || echo "NO libcitro3d in portlibs"
  echo "--- headers ---"
  ls $DEVKITPRO/portlibs/3ds/include/citro2d.h 2>/dev/null || echo "NO citro2d.h"
  echo "--- 3ds_rules APP_TITLE usage ---"
  grep -n "APP_TITLE\|_SMDH\|smdhtool\|default_icon" $DEVKITARM/3ds_rules | head -20 || true
'
