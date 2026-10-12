#!/usr/bin/env bash
set -euo pipefail

PKG_NAME="${PKG_NAME:-kairos}"
PKG_REL="${PKG_REL:-1}"
MAINTAINER="${MAINTAINER:-Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>}"
DESCRIPTION="${DESCRIPTION:-Kairos, reversible concurrent language: compiler, VM and debugger library}"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"

# Requirements, checked BEFORE starting the build.
require() {
  command -v "$1" >/dev/null 2>&1 || { echo "Missing '$1'. $2"; MISSING=1; }
}
MISSING=0
require make "Install the build tools (Debian/Ubuntu: sudo apt install build-essential)."
require gcc "Install a C compiler (Debian/Ubuntu: sudo apt install build-essential)."
require python3 "Install Python 3."
if [[ ! -x "${PROJECT_ROOT}/venv/bin/python" ]]; then
  echo "Missing the virtualenv in ${PROJECT_ROOT}/venv. Run: (cd ${PROJECT_ROOT} && make install-deps)"; MISSING=1
elif [[ ! -x "${PROJECT_ROOT}/venv/bin/pyinstaller" ]]; then
  echo "Missing pyinstaller in the virtualenv. Run: (cd ${PROJECT_ROOT} && make install-deps)"; MISSING=1
fi
require makepkg "Serve un sistema Arch Linux (makepkg)."
if [[ "${MISSING}" -ne 0 ]]; then
  echo "Missing requirements: the build does not start."; exit 1
fi
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
# PKG_VERSION from the environment (e.g. the release CI, from the vX.Y.Z tag) wins
# over the VERSION file, which is then left untouched.
BUMP_VERSION_FILE=0
if [[ -z "${PKG_VERSION:-}" ]]; then
  PKG_VERSION="${MAJOR}.${MINOR}.$((PATCH + 1))"
  BUMP_VERSION_FILE=1
fi

APP_SRC="${PROJECT_ROOT}/build/dist/kairos"
DAP_SRC="${PROJECT_ROOT}/build/libvm_dap.so"
VM_SRC="${PROJECT_ROOT}/build/libvm.so"

echo "Project root: ${PROJECT_ROOT}"
echo "Version bump: ${CURRENT_VERSION} -> ${PKG_VERSION}"
echo "Running build pipeline..."
# The .so runs on CPUs other than the build one: no -march=native.
case "$(uname -m)" in x86_64) PKG_MARCH="${PKG_MARCH--march=x86-64}" ;; *) PKG_MARCH="${PKG_MARCH-}" ;; esac
rm -f "${PROJECT_ROOT}/build/libvm.so"
(cd "${PROJECT_ROOT}" && make release-app MARCH="${PKG_MARCH}" && make build-dap)

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
provides=('kairosapp')
conflicts=('kairosapp')
replaces=('kairosapp')
options=('!debug' '!strip')
source=()
sha256sums=()
pkgdir_ref="${ARCH_BUILD_DIR}/pkgdir"
app_src="${APP_SRC}"
dap_src="${DAP_SRC}"
vm_src="${VM_SRC}"

package() {
  install -d "\${pkgdir}/opt/kairos"
  install -d "\${pkgdir}/usr/local/lib/kairos"
  install -d "\${pkgdir}/usr/local/bin"
  install -m 755 "\${app_src}" "\${pkgdir}/opt/kairos/kairos"
  install -m 644 "\${dap_src}" "\${pkgdir}/usr/local/lib/kairos/dap.so"
  install -m 644 "\${vm_src}" "\${pkgdir}/usr/local/lib/kairos/libvm.so"
  ln -sf /opt/kairos/kairos "\${pkgdir}/usr/local/bin/kairos"
}
EOF

(
  cd "${ARCH_BUILD_DIR}"
  PKGDEST="${ROOT_DIR}" makepkg -f --nodeps --cleanbuild
)

if [[ "${BUMP_VERSION_FILE}" -eq 1 ]]; then echo "${PKG_VERSION}" > "${VERSION_FILE}"; fi

ARCH_FILE="$(ls -1 "${ROOT_DIR}/${PKG_NAME}-${PKG_VERSION}-${PKG_REL}-"*.pkg.tar.zst | head -n 1)"

echo
echo "Package created:"
echo "  ${ARCH_FILE}"
echo
echo "Install with:"
echo "  sudo pacman -U \"${ARCH_FILE}\""
echo "Maintainer: ${MAINTAINER}"
