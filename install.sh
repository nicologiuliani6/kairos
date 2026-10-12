#!/usr/bin/env bash
# Installa (o aggiorna) Kairos dal pacchetto dell'ultima release (o di quella
# indicata), per la distribuzione in uso: .deb (apt), .rpm (dnf/zypper) o Arch
# (pacman). Chiede la password di sudo per installare.
#
#   curl -fsSL https://github.com/nicologiuliani6/kairos/releases/latest/download/install.sh | bash
#   ./install.sh            # ultima release
#   ./install.sh v1.2.1     # una versione precisa
set -euo pipefail

REPO="nicologiuliani6/kairos"
PKG="kairos"
TAG="${1:-latest}"

case "$(uname -m)" in
  x86_64) DEB_ARCH=amd64; RPM_ARCH=x86_64 ;;
  *) echo "Le release hanno pacchetti solo per x86_64: compila dai sorgenti (make install-deps && make)."; exit 1 ;;
esac

if command -v apt-get >/dev/null 2>&1; then
  FMT=deb; PATTERN="${PKG}_[^\"/]*_${DEB_ARCH}\\.deb"
elif command -v dnf >/dev/null 2>&1 || command -v zypper >/dev/null 2>&1; then
  FMT=rpm; PATTERN="${PKG}-[0-9][^\"/]*\\.${RPM_ARCH}\\.rpm"
elif command -v pacman >/dev/null 2>&1; then
  FMT=arch; PATTERN="${PKG}-[0-9][^\"/]*-${RPM_ARCH}\\.pkg\\.tar\\.zst"
else
  echo "Distribuzione non riconosciuta (servono apt, dnf, zypper o pacman)."; exit 1
fi

if [[ "${TAG}" == latest ]]; then API="https://api.github.com/repos/${REPO}/releases/latest"
else API="https://api.github.com/repos/${REPO}/releases/tags/${TAG}"; fi
URL="$(curl -fsSL "${API}" | grep -o "\"browser_download_url\": *\"[^\"]*/${PATTERN}\"" | head -1 | sed 's/.*"\(http[^"]*\)"/\1/')"
[[ -n "${URL}" ]] || { echo "Nessun pacchetto ${FMT} di ${PKG} nella release ${TAG}."; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "${TMP}"' EXIT
FILE="${TMP}/$(basename "${URL}")"
echo "Scarico ${URL}"
curl -fsSL -o "${FILE}" "${URL}"
chmod 644 "${FILE}"; chmod 755 "${TMP}"   # apt legge il file come utente _apt

SUDO=""; [[ "${EUID}" -ne 0 ]] && SUDO="sudo"
case "${FMT}" in
  deb)  ${SUDO} apt-get install -y "${FILE}" ;;
  rpm)  if command -v dnf >/dev/null 2>&1; then ${SUDO} dnf install -y "${FILE}"; else ${SUDO} zypper --non-interactive install --allow-unsigned-rpm "${FILE}"; fi ;;
  arch) ${SUDO} pacman -U --noconfirm "${FILE}" ;;
esac
echo "Installato: $(command -v ${PKG} || echo /usr/local/bin/${PKG})"
