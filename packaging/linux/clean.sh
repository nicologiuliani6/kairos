#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

rm -rf "${ROOT_DIR}/.build"
rm -f "${ROOT_DIR}"/kairosapp_*.deb
rm -f "${ROOT_DIR}"/kairosapp-*.rpm
rm -f "${ROOT_DIR}"/kairosapp-*.pkg.tar.zst

echo "General clean completed in ${ROOT_DIR}"
