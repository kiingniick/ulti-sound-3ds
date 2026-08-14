#!/usr/bin/env bash
# Configure + build the project with CMake inside the devkitPro container.
#   scripts/build.sh          # configure (if needed) and build
#   scripts/build.sh clean    # remove the build directory
set -e

PROJECT_WIN="/mnt/c/Users/monke/Projects/ulti-sound-3ds"

# Make sure the Docker daemon is up (it persists across WSL calls while running).
if ! docker info >/dev/null 2>&1; then
  mkdir -p /var/log
  nohup dockerd --iptables=false --bridge=none >/var/log/dockerd.log 2>&1 &
  disown || true
  for i in $(seq 1 60); do docker info >/dev/null 2>&1 && break; sleep 2; done
fi

if [ "${1:-}" = "clean" ]; then
  echo "=== clean ==="
  docker run --rm -v "${PROJECT_WIN}:/project" -w /project \
    devkitpro/devkitarm:latest bash -lc "rm -rf build"
  exit 0
fi

echo "=== cmake configure + build ==="
docker run --rm \
  -v "${PROJECT_WIN}:/project" \
  -w /project \
  devkitpro/devkitarm:latest \
  bash -lc '
    set -e
    cmake -S . -B build -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/3DS.cmake" \
      -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc)"
  '

echo "=== artifacts ==="
ls -la "${PROJECT_WIN}"/*.3dsx "${PROJECT_WIN}"/*.smdh 2>/dev/null || echo "no artifacts yet"
