#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

rm -rf "${ROOT_DIR}/.build"
rm -f "${ROOT_DIR}"/kairos_*.deb
rm -f "${ROOT_DIR}"/kairos-*.rpm
rm -f "${ROOT_DIR}"/kairos-*.pkg.tar.zst

echo "General clean completed in ${ROOT_DIR}"
