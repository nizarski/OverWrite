#!/usr/bin/env bash
# OverWrite integration test using a loop-backed ext4 filesystem.
# Requires: root/sudo, losetup, mkfs.ext4, mount, dd, cmake, gcc.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
IMG="${ROOT}/test_loop.img"
MNT="${ROOT}/test_mnt"
LOOP=""
MOUNTED=0

cleanup() {
    if [[ "${MOUNTED}" -eq 1 ]]; then
        umount "${MNT}" 2>/dev/null || true
    fi
    if [[ -n "${LOOP}" ]]; then
        losetup -d "${LOOP}" 2>/dev/null || true
    fi
    rm -rf "${MNT}" "${IMG}"
}
trap cleanup EXIT

if [[ "${EUID}" -ne 0 ]]; then
    echo "This test must run as root (loop device + mount)." >&2
    exit 1
fi

cmake -B "${BUILD}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD}" -j"$(nproc)"

BIN="${BUILD}/overwrite"
if [[ ! -x "${BIN}" ]]; then
    echo "binary not found: ${BIN}" >&2
    exit 1
fi

dd if=/dev/urandom of="${IMG}" bs=1M count=32 status=none
LOOP="$(losetup --find --show "${IMG}")"
mkfs.ext4 -F "${LOOP}" >/dev/null
mkdir -p "${MNT}"
mount "${LOOP}" "${MNT}"
MOUNTED=1

SECRET="${MNT}/secret.bin"
dd if=/dev/urandom of="${SECRET}" bs=4K count=16 status=none

echo "=== dry-run file wipe ==="
echo "${SECRET}" | "${BIN}" --dry-run "${SECRET}" 2>&1 | head -5 || true

echo "=== wipe file with filesystem-shadow (overwrite + secure delete) ==="
echo "${SECRET}" | "${BIN}" --profile filesystem-shadow --rng turbo "${SECRET}"
test ! -f "${SECRET}" || { echo "file still exists"; exit 1; }

echo "=== free-space shadow wipe ==="
dd if=/dev/urandom of="${MNT}/payload.bin" bs=64K count=32 status=none
rm -f "${MNT}/payload.bin"

FILLER="${MNT}/.overwrite_filler_manual.tmp"
dd if=/dev/zero of="${FILLER}" bs=1M count=8 status=none
echo "${FILLER}" | "${BIN}" --profile filesystem-shadow --rng turbo "${FILLER}"
test ! -f "${FILLER}" || { echo "filler still exists"; exit 1; }

echo "=== slack-hunter dry-run ==="
dd if=/dev/urandom of="${MNT}/slack_test.bin" bs=500 count=1 status=none
echo "${MNT}/slack_test.bin" | "${BIN}" --dry-run --profile slack-hunter "${MNT}/slack_test.bin" 2>&1 | grep -i slack || true
rm -f "${MNT}/slack_test.bin"

echo "=== partition map / unallocated dry-run ==="
echo "${LOOP}" | "${BIN}" --dry-run --unallocated "${LOOP}" 2>&1 | head -10 || true

echo "All loop-device tests passed."
