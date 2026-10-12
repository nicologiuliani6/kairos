#!/usr/bin/env bash
# Installs (or updates) Kairos from the package of the latest release (or of the
# given one) for this distribution: .deb (apt), .rpm (dnf/zypper) or Arch
# (pacman). Asks for the sudo password to install.
#
#   curl -fsSL https://github.com/nicologiuliani6/kairos/releases/latest/download/install.sh | bash
#   ./install.sh            # latest release
#   ./install.sh v1.2.1     # a specific version
set -euo pipefail

REPO="nicologiuliani6/kairos"
PKG="kairos"
TAG="${1:-latest}"

case "$(uname -m)" in
  x86_64) DEB_ARCH=amd64; RPM_ARCH=x86_64 ;;
  *) echo "Releases only have x86_64 packages: build from source (make install-deps && make)."; exit 1 ;;
esac

if command -v apt-get >/dev/null 2>&1; then
  FMT=deb; PATTERN="${PKG}_[^\"/]*_${DEB_ARCH}\\.deb"
elif command -v dnf >/dev/null 2>&1 || command -v zypper >/dev/null 2>&1; then
  FMT=rpm; PATTERN="${PKG}-[0-9][^\"/]*\\.${RPM_ARCH}\\.rpm"
elif command -v pacman >/dev/null 2>&1; then
  FMT=arch; PATTERN="${PKG}-[0-9][^\"/]*-${RPM_ARCH}\\.pkg\\.tar\\.zst"
else
  echo "Unknown distribution (apt, dnf, zypper or pacman is needed)."; exit 1
fi

if [[ "${TAG}" == latest ]]; then API="https://api.github.com/repos/${REPO}/releases/latest"
else API="https://api.github.com/repos/${REPO}/releases/tags/${TAG}"; fi
URL="$(curl -fsSL "${API}" | grep -o "\"browser_download_url\": *\"[^\"]*/${PATTERN}\"" | head -1 | sed 's/.*"\(http[^"]*\)"/\1/')"
[[ -n "${URL}" ]] || { echo "No ${FMT} package of ${PKG} in release ${TAG}."; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "${TMP}"' EXIT
FILE="${TMP}/$(basename "${URL}")"
echo "Downloading ${URL}"
curl -fsSL -o "${FILE}" "${URL}"
chmod 644 "${FILE}"; chmod 755 "${TMP}"   # apt reads the file as user _apt

SUDO=""; [[ "${EUID}" -ne 0 ]] && SUDO="sudo"
case "${FMT}" in
  deb)  ${SUDO} apt-get install -y "${FILE}" ;;
  rpm)  if command -v dnf >/dev/null 2>&1; then ${SUDO} dnf install -y "${FILE}"; else ${SUDO} zypper --non-interactive install --allow-unsigned-rpm "${FILE}"; fi ;;
  arch) ${SUDO} pacman -U --noconfirm "${FILE}" ;;
esac
echo "Installed: $(command -v ${PKG} || echo /usr/local/bin/${PKG})"
