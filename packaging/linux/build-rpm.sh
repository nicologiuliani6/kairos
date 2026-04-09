#!/usr/bin/env bash
set -euo pipefail

PKG_NAME="${PKG_NAME:-kairosapp}"
PKG_ARCH="${PKG_ARCH:-$(uname -m)}"
RPM_RELEASE="${RPM_RELEASE:-1}"
MAINTAINER="${MAINTAINER:-Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>}"
DESCRIPTION="${DESCRIPTION:-KairosApp with dap shared library}"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
BUILD_DIR="${ROOT_DIR}/.build"
VERSION_FILE="${ROOT_DIR}/VERSION"
RPM_TOPDIR="${BUILD_DIR}/rpmbuild"

if ! command -v rpmbuild >/dev/null 2>&1; then
  echo "rpmbuild not found. Install rpm-build package first."
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

rm -rf "${RPM_TOPDIR}"
mkdir -p "${RPM_TOPDIR}/"{BUILD,RPMS,SOURCES,SPECS,SRPMS}
rm -f "${ROOT_DIR}/${PKG_NAME}-${PKG_VERSION}-${RPM_RELEASE}."*.rpm

SPEC_FILE="${RPM_TOPDIR}/SPECS/${PKG_NAME}.spec"
cat > "${SPEC_FILE}" <<EOF
Name:           ${PKG_NAME}
Version:        ${PKG_VERSION}
Release:        ${RPM_RELEASE}%{?dist}
Summary:        ${DESCRIPTION}
License:        Proprietary
BuildArch:      ${PKG_ARCH}
Requires:       glibc

%description
${DESCRIPTION}

%install
mkdir -p %{buildroot}/opt/kairosapp
mkdir -p %{buildroot}/usr/local/lib/kairosapp
mkdir -p %{buildroot}/usr/local/bin
install -m 755 ${APP_SRC} %{buildroot}/opt/kairosapp/KairosApp
install -m 644 ${DAP_SRC} %{buildroot}/usr/local/lib/kairosapp/dap.so
ln -sf /opt/kairosapp/KairosApp %{buildroot}/usr/local/bin/kairosapp

%post
if [ ! -f /etc/ld.so.conf.d/kairosapp.conf ]; then
  echo "/usr/local/lib/kairosapp" > /etc/ld.so.conf.d/kairosapp.conf
fi
/sbin/ldconfig

%preun
:

%postun
if [ \$1 -eq 0 ]; then
  rm -f /etc/ld.so.conf.d/kairosapp.conf
fi
/sbin/ldconfig

%files
/opt/kairosapp/KairosApp
/usr/local/lib/kairosapp/dap.so
/usr/local/bin/kairosapp

%changelog
* Thu Apr 09 2026 ${MAINTAINER} - ${PKG_VERSION}-${RPM_RELEASE}
- Automated KairosApp RPM package build.
EOF

rpmbuild --define "_topdir ${RPM_TOPDIR}" -bb "${SPEC_FILE}"

RPM_FILE="$(ls -1 "${RPM_TOPDIR}/RPMS/${PKG_ARCH}/"*.rpm | head -n 1)"
cp "${RPM_FILE}" "${ROOT_DIR}/"
echo "${PKG_VERSION}" > "${VERSION_FILE}"

echo
echo "Package created:"
echo "  ${ROOT_DIR}/$(basename "${RPM_FILE}")"
echo
echo "Install with:"
echo "  sudo rpm -Uvh \"${ROOT_DIR}/$(basename "${RPM_FILE}")\""
