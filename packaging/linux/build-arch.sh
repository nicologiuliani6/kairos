#!/usr/bin/env bash
set -euo pipefail

PKG_NAME="${PKG_NAME:-kairosapp}"
PKG_REL="${PKG_REL:-1}"
MAINTAINER="${MAINTAINER:-Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>}"
DESCRIPTION="${DESCRIPTION:-KairosApp with dap shared library}"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/.build"
VERSION_FILE="${ROOT_DIR}/VERSION"
ARCH_BUILD_DIR="${BUILD_DIR}/archpkg"

if ! command -v makepkg >/dev/null 2>&1; then
  echo "makepkg not found. Install base-devel (pacman) first."
  exit 1
fi

if [[ -f "${VERSION_FILE}" ]]; then
  CURRENT_VERSION="$(tr -d '[:space:]' < "${VERSION_FILE}")"
else
  CURRENT_VERSION="1.0.0"
fi

if [[ ! "${CURRENT_VERSION}" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  echo "Invalid VERSION format: ${CURRENT_VERSION}"
  echo "Expected semantic version like: 1.0.0"
  exit 1
fi

MAJOR="${BASH_REMATCH[1]}"
MINOR="${BASH_REMATCH[2]}"
PATCH="${BASH_REMATCH[3]}"
PKG_VERSION="${MAJOR}.${MINOR}.$((PATCH + 1))"

APP_SRC="${PROJECT_ROOT}/build/dist/KairosApp"
DAP_SRC="${PROJECT_ROOT}/build/libvm_dap.so"

echo "Project root: ${PROJECT_ROOT}"
echo "Version bump: ${CURRENT_VERSION} -> ${PKG_VERSION}"
echo "Running build pipeline..."
(cd "${PROJECT_ROOT}" && make release && make build-dap)

if [[ ! -f "${APP_SRC}" ]]; then
  echo "Missing file after build: ${APP_SRC}"
  exit 1
fi
if [[ ! -f "${DAP_SRC}" ]]; then
  echo "Missing file after build: ${DAP_SRC}"
  exit 1
fi

rm -rf "${ARCH_BUILD_DIR}"
mkdir -p "${ARCH_BUILD_DIR}"
rm -f "${ROOT_DIR}/${PKG_NAME}-${PKG_VERSION}-${PKG_REL}-"*.pkg.tar.zst

cat > "${ARCH_BUILD_DIR}/PKGBUILD" <<EOF
pkgname=${PKG_NAME}
pkgver=${PKG_VERSION}
pkgrel=${PKG_REL}
pkgdesc="${DESCRIPTION}"
arch=('x86_64' 'aarch64')
license=('custom')
depends=('glibc')
source=()
sha256sums=()
pkgdir_ref="${ARCH_BUILD_DIR}/pkgdir"
app_src="${APP_SRC}"
dap_src="${DAP_SRC}"

package() {
  install -d "\${pkgdir}/opt/kairosapp"
  install -d "\${pkgdir}/usr/local/lib/kairosapp"
  install -d "\${pkgdir}/usr/local/bin"
  install -m 755 "\${app_src}" "\${pkgdir}/opt/kairosapp/KairosApp"
  install -m 644 "\${dap_src}" "\${pkgdir}/usr/local/lib/kairosapp/dap.so"
  ln -sf /opt/kairosapp/KairosApp "\${pkgdir}/usr/local/bin/kairosapp"
}
EOF

(
  cd "${ARCH_BUILD_DIR}"
  PKGDEST="${ROOT_DIR}" makepkg -f --nodeps --cleanbuild
)

echo "${PKG_VERSION}" > "${VERSION_FILE}"

ARCH_FILE="$(ls -1 "${ROOT_DIR}/${PKG_NAME}-${PKG_VERSION}-${PKG_REL}-"*.pkg.tar.zst | head -n 1)"

echo
echo "Package created:"
echo "  ${ARCH_FILE}"
echo
echo "Install with:"
echo "  sudo pacman -U \"${ARCH_FILE}\""
echo "Maintainer: ${MAINTAINER}"
