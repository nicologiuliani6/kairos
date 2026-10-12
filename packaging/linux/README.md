# Kairos Linux packaging

Scripts that build the Linux packages of Kairos (package name `kairos`). Releases get them automatically
from `.github/workflows/release.yml` when a tag `vX.Y.Z` is pushed.

The packages install:

- the standalone app (frontend + VM, no Python needed) in `/opt/kairos/kairos`
- `dap.so` (debugger) and `libvm.so` (VM, used by `mnemo compile`) in `/usr/local/lib/kairos/`
- the CLI link `/usr/local/bin/kairos`

The package replaces the old `kairosapp` package.

On installation `ldconfig` runs and `/etc/ld.so.conf.d/kairos.conf` is
created with the library path.

## 1) Clean

```bash
./clean.sh
```

## 2) Build a package

Prerequisite for all three: `make install-deps` in the repository root (venv
with `ply` and `pyinstaller`).

### Debian/Ubuntu (`.deb`)

```bash
./build-deb.sh
```

The script runs `make release-app` and `make build-dap`, then packages:

- `build/dist/kairos`
- `build/libvm_dap.so` (shipped as `dap.so`)
- `build/libvm.so`

Output: `./kairos_<version>_<arch>.deb`, e.g. `kairos_1.0.4_amd64.deb`.

### Fedora/RHEL (`.rpm`)

```bash
./build-rpm.sh
```

Output: `./kairos-<version>-1.<dist>.<arch>.rpm`. Requires `rpmbuild`
(`rpm-build`).

### Arch Linux (`.pkg.tar.zst`)

```bash
./build-arch.sh
```

Output: `./kairos-<version>-1-<arch>.pkg.tar.zst`. Requires `makepkg`
(`base-devel`); `makepkg` refuses to run as root.

### Version

By default every build bumps the patch number stored in `VERSION`. Setting
`PKG_VERSION` uses that version instead and leaves `VERSION` untouched (this is
what the release workflow does with the tag):

```bash
PKG_VERSION=1.2.0 ./build-deb.sh
```

## 3) Install

```bash
sudo apt install ./kairos_<version>_amd64.deb
sudo dnf install ./kairos-<version>-1.<dist>.x86_64.rpm
sudo pacman -U ./kairos-<version>-1-x86_64.pkg.tar.zst
```

## 4) Check

```bash
kairos            # prints the usage line
ldconfig -p | grep dap
ls -l /usr/local/bin/kairos /opt/kairos/kairos /usr/local/lib/kairos/dap.so
```

## 5) Remove

```bash
sudo dpkg -r kairos      # or: sudo dpkg -P kairos (also config files)
sudo dnf remove kairos
sudo pacman -R kairos
```

## Metadata overrides

```bash
PKG_NAME=kairos \
PKG_ARCH=amd64 \
MAINTAINER="Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>" \
DESCRIPTION="kairos with dap shared library" \
./build-deb.sh
```
