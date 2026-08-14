#!/usr/bin/env bash
# Install Docker Engine inside WSL and pull the devkitPro toolchain image.
set -e
export DEBIAN_FRONTEND=noninteractive

echo "=== [1/3] ensure docker.io installed ==="
if ! command -v dockerd >/dev/null 2>&1; then
  apt-get update -y
  apt-get install -y --no-install-recommends docker.io ca-certificates
fi
echo "docker: $(docker --version 2>/dev/null || echo none)"

echo "=== [2/3] ensure dockerd running ==="
if ! docker info >/dev/null 2>&1; then
  mkdir -p /var/log
  # Build-only usage: disable container networking bits that need iptables/bridge
  nohup dockerd --iptables=false --bridge=none >/var/log/dockerd.log 2>&1 &
  disown || true
  for i in $(seq 1 90); do
    if docker info >/dev/null 2>&1; then break; fi
    sleep 2
  done
fi

if docker info >/dev/null 2>&1; then
  echo "DOCKERD_OK"
else
  echo "DOCKERD_FAIL"
  tail -n 40 /var/log/dockerd.log 2>/dev/null || true
  exit 1
fi

echo "=== [3/3] pull devkitpro/devkitarm ==="
docker pull devkitpro/devkitarm:latest
echo "PULL_DONE"
