#!/usr/bin/env bash
set -euo pipefail

# Build a Debian package that installs:
# - KairosApp binary in /opt/kairosapp
# - dap.so in /usr/local/lib/kairosapp
# - symlink /usr/local/bin/kairosapp -> /opt/kairosapp/KairosApp

PKG_NAME="${PKG_NAME:-kairosapp}"
PKG_ARCH="${PKG_ARCH:-$(dpkg --print-architecture)}"
MAINTAINER="${MAINTAINER:-Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>}"
DESCRIPTION="${DESCRIPTION:-KairosApp with dap shared library}"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
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
PKG_VERSION="${MAJOR}.${MINOR}.$((PATCH + 1))"
STAGE_DIR="${BUILD_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}"
DEBIAN_DIR="${STAGE_DIR}/DEBIAN"

# make release: PyInstaller --onefile → build/dist/KairosApp (file eseguibile).
# Se passi a --onedir, l'eseguibile è build/dist/KairosApp/KairosApp.
DAP_SRC="${PROJECT_ROOT}/build/libvm_dap.so"

echo "Project root: ${PROJECT_ROOT}"
echo "Version bump: ${CURRENT_VERSION} -> ${PKG_VERSION}"
echo "Running build pipeline..."
(cd "${PROJECT_ROOT}" && make release && make build-dap)

APP_ONEFILE="${PROJECT_ROOT}/build/dist/KairosApp"
APP_ONEDIR="${PROJECT_ROOT}/build/dist/KairosApp/KairosApp"
if [[ -f "${APP_ONEFILE}" && -x "${APP_ONEFILE}" ]]; then
  APP_SRC="${APP_ONEFILE}"
elif [[ -f "${APP_ONEDIR}" && -x "${APP_ONEDIR}" ]]; then
  APP_SRC="${APP_ONEDIR}"
else
  echo "Missing KairosApp after make release. Expected one of:"
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
mkdir -p "${STAGE_DIR}/opt/kairosapp"
mkdir -p "${STAGE_DIR}/usr/local/lib/kairosapp"
mkdir -p "${STAGE_DIR}/usr/local/bin"
cat > "${DEBIAN_DIR}/control" <<EOF
Package: ${PKG_NAME}
Version: ${PKG_VERSION}
Section: utils
Priority: optional
Architecture: ${PKG_ARCH}
Maintainer: ${MAINTAINER}
Depends: libc6
Description: ${DESCRIPTION}
EOF

cp "${ROOT_DIR}/debian/postinst" "${DEBIAN_DIR}/postinst"
cp "${ROOT_DIR}/debian/prerm" "${DEBIAN_DIR}/prerm"
cp "${ROOT_DIR}/debian/postrm" "${DEBIAN_DIR}/postrm"
chmod 755 "${DEBIAN_DIR}/postinst" "${DEBIAN_DIR}/prerm" "${DEBIAN_DIR}/postrm"

cp "${APP_SRC}" "${STAGE_DIR}/opt/kairosapp/KairosApp"
chmod 755 "${STAGE_DIR}/opt/kairosapp/KairosApp"

cp "${DAP_SRC}" "${STAGE_DIR}/usr/local/lib/kairosapp/dap.so"
chmod 644 "${STAGE_DIR}/usr/local/lib/kairosapp/dap.so"

ln -s "/opt/kairosapp/KairosApp" "${STAGE_DIR}/usr/local/bin/kairosapp"

PKG_FILE="${ROOT_DIR}/${PKG_NAME}_${PKG_VERSION}_${PKG_ARCH}.deb"
dpkg-deb --build "${STAGE_DIR}" "${PKG_FILE}"
echo "${PKG_VERSION}" > "${VERSION_FILE}"

echo
echo "Package created:"
echo "  ${PKG_FILE}"
echo
echo "Install with:"
echo "  sudo dpkg -i \"${PKG_FILE}\""
