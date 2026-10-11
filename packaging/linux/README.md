# KairosApp Linux packaging

Scripts that build Linux packages of KairosApp. Releases get them automatically
from `.github/workflows/release.yml` when a tag `vX.Y.Z` is pushed.

The packages install:

- `KairosApp` in `/opt/kairosapp/KairosApp`
- `dap.so` in `/usr/local/lib/kairosapp/dap.so`
- the CLI link `/usr/local/bin/kairosapp`

On installation `ldconfig` runs and `/etc/ld.so.conf.d/kairosapp.conf` is
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

- `build/dist/KairosApp`
- `build/libvm_dap.so` (shipped as `dap.so`)

Output: `./kairosapp_<version>_<arch>.deb`, e.g. `kairosapp_1.0.4_amd64.deb`.

### Fedora/RHEL (`.rpm`)

```bash
./build-rpm.sh
```

Output: `./kairosapp-<version>-1.<dist>.<arch>.rpm`. Requires `rpmbuild`
(`rpm-build`).

### Arch Linux (`.pkg.tar.zst`)

```bash
./build-arch.sh
```

Output: `./kairosapp-<version>-1-<arch>.pkg.tar.zst`. Requires `makepkg`
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
sudo apt install ./kairosapp_<version>_amd64.deb
sudo dnf install ./kairosapp-<version>-1.<dist>.x86_64.rpm
sudo pacman -U ./kairosapp-<version>-1-x86_64.pkg.tar.zst
```

## 4) Check

```bash
kairosapp            # prints the usage line
ldconfig -p | grep dap
ls -l /usr/local/bin/kairosapp /opt/kairosapp/KairosApp /usr/local/lib/kairosapp/dap.so
```

## 5) Remove

```bash
sudo dpkg -r kairosapp      # or: sudo dpkg -P kairosapp (also config files)
sudo dnf remove kairosapp
sudo pacman -R kairosapp
```

## Metadata overrides

```bash
PKG_NAME=kairosapp \
PKG_ARCH=amd64 \
MAINTAINER="Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>" \
DESCRIPTION="KairosApp with dap shared library" \
./build-deb.sh
```
