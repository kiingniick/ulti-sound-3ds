#!/usr/bin/env bash
docker run --rm devkitpro/devkitarm:latest bash -lc '
  echo "=== __stacksize__ in libctru ===";
  grep -rIn "__stacksize__" "$DEVKITPRO/libctru/" 2>/dev/null | head;
  echo "=== crt0 files ===";
  find "$DEVKITPRO" -name "*crt0*" 2>/dev/null | head;
  echo "=== __stacksize__ / __stack_size anywhere ===";
  grep -rIln "__stacksize__\|__stack_size\|_stacksize" "$DEVKITPRO/devkitARM/" 2>/dev/null | head;
'
