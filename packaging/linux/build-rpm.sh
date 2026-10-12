#!/usr/bin/env bash
set -euo pipefail

PKG_NAME="${PKG_NAME:-kairos}"
PKG_ARCH="${PKG_ARCH:-$(uname -m)}"
RPM_RELEASE="${RPM_RELEASE:-1}"
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
require rpmbuild "Install rpm-build (Fedora: sudo dnf install rpm-build)."
if [[ "${MISSING}" -ne 0 ]]; then
  echo "Missing requirements: the build does not start."; exit 1
fi
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
Provides:       kairosapp = %{version}-%{release}
Obsoletes:      kairosapp < %{version}-%{release}

%description
${DESCRIPTION}

%install
mkdir -p %{buildroot}/opt/kairos
mkdir -p %{buildroot}/usr/local/lib/kairos
mkdir -p %{buildroot}/usr/local/bin
install -m 755 ${APP_SRC} %{buildroot}/opt/kairos/kairos
install -m 644 ${DAP_SRC} %{buildroot}/usr/local/lib/kairos/dap.so
install -m 644 ${VM_SRC} %{buildroot}/usr/local/lib/kairos/libvm.so
ln -sf /opt/kairos/kairos %{buildroot}/usr/local/bin/kairos

%post
if [ ! -f /etc/ld.so.conf.d/kairos.conf ]; then
  echo "/usr/local/lib/kairos" > /etc/ld.so.conf.d/kairos.conf
fi
/sbin/ldconfig

%preun
:

%postun
if [ \$1 -eq 0 ]; then
  rm -f /etc/ld.so.conf.d/kairos.conf
fi
/sbin/ldconfig

%files
/opt/kairos/kairos
/usr/local/lib/kairos/dap.so
/usr/local/lib/kairos/libvm.so
/usr/local/bin/kairos

%changelog
* Thu Apr 09 2026 ${MAINTAINER} - ${PKG_VERSION}-${RPM_RELEASE}
- Automated kairos RPM package build.
EOF

rpmbuild --define "_topdir ${RPM_TOPDIR}" -bb "${SPEC_FILE}"

RPM_FILE="$(ls -1 "${RPM_TOPDIR}/RPMS/${PKG_ARCH}/"*.rpm | head -n 1)"
cp "${RPM_FILE}" "${ROOT_DIR}/"
if [[ "${BUMP_VERSION_FILE}" -eq 1 ]]; then echo "${PKG_VERSION}" > "${VERSION_FILE}"; fi

echo
echo "Package created:"
echo "  ${ROOT_DIR}/$(basename "${RPM_FILE}")"
echo
echo "Install with:"
echo "  sudo rpm -Uvh \"${ROOT_DIR}/$(basename "${RPM_FILE}")\""
