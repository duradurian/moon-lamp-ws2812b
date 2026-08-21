#!/usr/bin/env bash

set -euo pipefail

# ESP-IDF's CMake configure step rejects project paths containing spaces. Keep
# the checked-out project where it is, but compile an isolated copy in /tmp.
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
build_root="$(mktemp -d "${TMPDIR:-/tmp}/esp32-music-sync.XXXXXX")"

cleanup() {
  rm -rf -- "$build_root"
}
trap cleanup EXIT

chmod 700 "$build_root"
rsync -a \
  --exclude='.git/' \
  --exclude='.pio/' \
  --exclude='managed_components/' \
  --exclude='sdkconfig.defaults' \
  --exclude='sdkconfig.esp32dev' \
  "$project_root/" "$build_root/"

(
  cd "$build_root"
  pio run "$@"
)

# Preserve the useful outputs even though the isolated source tree is removed.
artifact_root="$project_root/.pio/memory-build"
mkdir -p "$artifact_root"
for artifact in firmware.bin firmware.elf firmware.map partitions.bin bootloader.bin; do
  source_path="$build_root/.pio/build/esp32dev/$artifact"
  if [[ -f "$source_path" ]]; then
    cp "$source_path" "$artifact_root/$artifact"
  fi
done

printf 'Memory-optimized artifacts: %s\n' "$artifact_root"
