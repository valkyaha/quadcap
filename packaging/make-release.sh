#!/usr/bin/env bash

set -euo pipefail

[[ $# -eq 4 ]] || {
  echo "usage: $0 VERSION BUILD_DIR DRIVER_DIR OUTPUT_DIR" >&2
  exit 2
}

VERSION="$1"
BUILD_DIR="$(realpath "$2")"
DRIVER_DIR="$(realpath "$3")"
OUTPUT_DIR="$(mkdir -p "$4" && realpath "$4")"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCHIVE_NAME="quadcap-${VERSION}-linux-x86_64"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${PROJECT_DIR}" show -s --format=%ct HEAD)}"
STAGING_DIR="$(mktemp -d)"

cleanup() {
  rm -rf "${STAGING_DIR}"
}
trap cleanup EXIT

[[ -x "${BUILD_DIR}/quadcap" ]] || { echo "quadcap binary missing" >&2; exit 1; }
[[ -x "${BUILD_DIR}/quadcapd" ]] || { echo "quadcapd binary missing" >&2; exit 1; }
[[ -f "${DRIVER_DIR}/dkms.conf" ]] || { echo "driver source missing" >&2; exit 1; }

ROOT="${STAGING_DIR}/${ARCHIVE_NAME}"
mkdir -p "${ROOT}"
git -C "${PROJECT_DIR}" archive HEAD | tar -x -C "${ROOT}"
mkdir -p "${ROOT}/prebuilt/linux-x86_64" "${ROOT}/third_party/sc0710"
install -m0755 "${BUILD_DIR}/quadcap" "${ROOT}/prebuilt/linux-x86_64/quadcap"
install -m0755 "${BUILD_DIR}/quadcapd" "${ROOT}/prebuilt/linux-x86_64/quadcapd"
cp -a "${DRIVER_DIR}/." "${ROOT}/third_party/sc0710/"
rm -rf "${ROOT}/third_party/sc0710/.git" "${ROOT}/third_party/sc0710/build"

find "${ROOT}" -print0 | xargs -0 touch --date="@${SOURCE_DATE_EPOCH}"
tar --sort=name --mtime="@${SOURCE_DATE_EPOCH}" --owner=0 --group=0 --numeric-owner \
  -C "${STAGING_DIR}" -cJf "${OUTPUT_DIR}/${ARCHIVE_NAME}.tar.xz" "${ARCHIVE_NAME}"

cd "${OUTPUT_DIR}"
sha256sum "${ARCHIVE_NAME}.tar.xz" >"${ARCHIVE_NAME}.tar.xz.sha256"
