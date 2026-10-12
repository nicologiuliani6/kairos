#!/usr/bin/env bash
set -euo pipefail

# Build a Debian package that installs:
# - kairos binary in /opt/kairos
# - dap.so and libvm.so in /usr/local/lib/kairos
# - symlink /usr/local/bin/kairos -> /opt/kairos/kairos

PKG_NAME="${PKG_NAME:-kairos}"
PKG_ARCH="${PKG_ARCH:-$(dpkg --print-architecture)}"
MAINTAINER="${MAINTAINER:-Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>}"
DESCRIPTION="${DESCRIPTION:-Kairos, reversible concurrent language: compiler, VM and debugger library}"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"

# Requisiti, controllati PRIMA di avviare la build.
require() {
  command -v "$1" >/dev/null 2>&1 || { echo "Manca '$1'. $2"; MISSING=1; }
}
MISSING=0
require make "Installa i tool di build (Debian/Ubuntu: sudo apt install build-essential)."
require gcc "Installa un compilatore C (Debian/Ubuntu: sudo apt install build-essential)."
require python3 "Installa Python 3."
if [[ ! -x "${PROJECT_ROOT}/venv/bin/python" ]]; then
  echo "Manca il virtualenv in ${PROJECT_ROOT}/venv. Esegui: (cd ${PROJECT_ROOT} && make install-deps)"; MISSING=1
elif [[ ! -x "${PROJECT_ROOT}/venv/bin/pyinstaller" ]]; then
  echo "Manca pyinstaller nel virtualenv. Esegui: (cd ${PROJECT_ROOT} && make install-deps)"; MISSING=1
fi
require dpkg-deb "Su Debian/Ubuntu e' nel pacchetto dpkg."
if [[ "${MISSING}" -ne 0 ]]; then
  echo "Requisiti mancanti: la build non parte."; exit 1
fi
BUILD_DIR="${ROOT_DIR}/.build"
VERSION_FILE="${ROOT_DIR}/VERSION"

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
# PKG_VERSION dall'ambiente (es. la CI di release, dal tag vX.Y.Z) vince sul
# file VERSION, che in quel caso non viene toccato.
BUMP_VERSION_FILE=0
if [[ -z "${PKG_VERSION:-}" ]]; then
  PKG_VERSION="${MAJOR}.${MINOR}.$((PATCH + 1))"
  BUMP_VERSION_FILE=1
fi
STAGE_DIR="${BUILD_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"
DEBIAN_DIR="${STAGE_DIR}/DEBIAN"

# make release-app: PyInstaller --onefile → build/dist/kairos (file eseguibile).
# Se passi a --onedir, l'eseguibile è build/dist/kairos/kairos.
DAP_SRC="${PROJECT_ROOT}/build/libvm_dap.so"
VM_SRC="${PROJECT_ROOT}/build/libvm.so"

echo "Project root: ${PROJECT_ROOT}"
echo "Version bump: ${CURRENT_VERSION} -> ${PKG_VERSION}"
echo "Running build pipeline..."
# release-app dipende da release (libvm.so) e produce il bundle kairos.
(cd "${PROJECT_ROOT}" && make release-app && make build-dap)

APP_ONEFILE="${PROJECT_ROOT}/build/dist/kairos"
APP_ONEDIR="${PROJECT_ROOT}/build/dist/kairos/kairos"
if [[ -f "${APP_ONEFILE}" && -x "${APP_ONEFILE}" ]]; then
  APP_SRC="${APP_ONEFILE}"
elif [[ -f "${APP_ONEDIR}" && -x "${APP_ONEDIR}" ]]; then
  APP_SRC="${APP_ONEDIR}"
else
  echo "Missing kairos after make release. Expected one of:"
  echo "  ${APP_ONEFILE}  (PyInstaller --onefile, default makefile)"
  echo "  ${APP_ONEDIR}   (PyInstaller --onedir)"
  exit 1
fi

if [[ ! -f "${DAP_SRC}" ]]; then
  echo "Missing file after build: ${DAP_SRC}"
  echo "Check make build-dap output."
  exit 1
fi

rm -rf "${STAGE_DIR}" "${ROOT_DIR}/${PKG_NAME}_"*"_${PKG_ARCH}.deb"
mkdir -p "${DEBIAN_DIR}"
mkdir -p "${STAGE_DIR}/opt/kairos"
mkdir -p "${STAGE_DIR}/usr/local/lib/kairos"
mkdir -p "${STAGE_DIR}/usr/local/bin"
cat > "${DEBIAN_DIR}/control" <<EOF
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Section: utils
Priority: optional
Architecture: ${PKG_ARCH}
Maintainer: ${MAINTAINER}
Depends: libc6
Provides: kairosapp
Replaces: kairosapp
Conflicts: kairosapp
Description: ${DESCRIPTION}
EOF

cp "${ROOT_DIR}/debian/postinst" "${DEBIAN_DIR}/postinst"
cp "${ROOT_DIR}/debian/prerm" "${DEBIAN_DIR}/prerm"
cp "${ROOT_DIR}/debian/postrm" "${DEBIAN_DIR}/postrm"
chmod 755 "${DEBIAN_DIR}/postinst" "${DEBIAN_DIR}/prerm" "${DEBIAN_DIR}/postrm"

cp "${APP_SRC}" "${STAGE_DIR}/opt/kairos/kairos"
chmod 755 "${STAGE_DIR}/opt/kairos/kairos"

cp "${DAP_SRC}" "${STAGE_DIR}/usr/local/lib/kairos/dap.so"
chmod 644 "${STAGE_DIR}/usr/local/lib/kairos/dap.so"
cp "${VM_SRC}" "${STAGE_DIR}/usr/local/lib/kairos/libvm.so"
chmod 644 "${STAGE_DIR}/usr/local/lib/kairos/libvm.so"

ln -s "/opt/kairos/kairos" "${STAGE_DIR}/usr/local/bin/kairos"

PKG_FILE="${ROOT_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"
dpkg-deb --build "${STAGE_DIR}" "${PKG_FILE}"
if [[ "${BUMP_VERSION_FILE}" -eq 1 ]]; then echo "${PKG_VERSION}" > "${VERSION_FILE}"; fi

echo
echo "Package created:"
echo "  ${PKG_FILE}"
echo
echo "Install with:"
echo "  sudo dpkg -i \"${PKG_FILE}\""
