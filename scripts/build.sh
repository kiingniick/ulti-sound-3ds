#!/usr/bin/env bash
# Compile the project inside the devkitPro container.
set -e

PROJECT_WIN="/mnt/c/Users/monke/Projects/ulti-sound-3ds"

# Make sure the daemon is up (it persists across WSL calls while a process runs).
if ! docker info >/dev/null 2>&1; then
  mkdir -p /var/log
  nohup dockerd --iptables=false --bridge=none >/var/log/dockerd.log 2>&1 &
  disown || true
  for i in $(seq 1 60); do docker info >/dev/null 2>&1 && break; sleep 2; done
fi

echo "=== make ${1:-all} ==="
docker run --rm \
  -v "${PROJECT_WIN}:/project" \
  -w /project \
  devkitpro/devkitarm:latest \
  bash -lc "make ${1:-all} 2>&1"

echo "=== artifacts ==="
ls -la "${PROJECT_WIN}"/*.3dsx "${PROJECT_WIN}"/*.smdh "${PROJECT_WIN}"/*.elf 2>/dev/null || echo "no artifacts yet"
