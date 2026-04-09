# KairosApp Linux Packaging

Tooling per generare pacchetti Linux di KairosApp.

Output installato dai pacchetti:

- `KairosApp` in `/opt/kairosapp/KairosApp`
- `dap.so` in `/usr/local/lib/kairosapp/dap.so`
- link CLI `/usr/local/bin/kairosapp`

In fase di installazione viene eseguito `ldconfig` e viene creato
`/etc/ld.so.conf.d/kairosapp.conf` con il path della libreria.

## 1) Clean generale

```bash
./clean.sh
```

## 2) Genera il pacchetto

### Debian/Ubuntu (`.deb`)

```bash
./build-deb.sh
```

Lo script esegue automaticamente:

- `make release`
- `make build-dap`
- incremento automatico versione patch da file `VERSION`

e prende i file da:

- `build/dist/KairosApp`
- `build/libvm_dap.so` (impacchettato come `dap.so`)

Output: `./kairosapp_<nuova-versione>_<arch>.deb`  
Esempio: `kairosapp_1.0.4_amd64.deb`

### Fedora/RHEL (`.rpm`)

```bash
./build-rpm.sh
```

Output: `./kairosapp-<nuova-versione>-1.<arch>.rpm`

Prerequisito: `rpmbuild` installato (`rpm-build`/`rpm`)

### Arch Linux (`.pkg.tar.zst`)

```bash
./build-arch.sh
```

Output: `./kairosapp-<nuova-versione>-1-<arch>.pkg.tar.zst`

Prerequisito: `makepkg` installato (`base-devel`)

## 3) Installa

```bash
sudo dpkg -i ./kairosapp_<nuova-versione>_amd64.deb
```

Alternativa con apt:

```bash
sudo apt install ./kairosapp_<nuova-versione>_amd64.deb
```

RPM:

```bash
sudo rpm -Uvh ./kairosapp-<nuova-versione>-1.<arch>.rpm
```

Arch:

```bash
sudo pacman -U ./kairosapp-<nuova-versione>-1-<arch>.pkg.tar.zst
```

## 4) Verifica

```bash
kairosapp --help
ldconfig -p | rg dap
ls -l /usr/local/bin/kairosapp /opt/kairosapp/KairosApp /usr/local/lib/kairosapp/dap.so
```

## 5) Rimozione

```bash
sudo dpkg -r kairosapp
```

Rimozione completa (inclusi file di configurazione):

```bash
sudo dpkg -P kairosapp
```

Per reinstallare subito:

```bash
sudo dpkg -i ./kairosapp_<nuova-versione>_amd64.deb
```

## Opzioni utili

Puoi sovrascrivere metadata durante la build:

```bash
PKG_NAME=kairosapp \
PKG_ARCH=amd64 \
MAINTAINER="Nicolo Giuliani <nicolo.giuliani6@studio.unibo.it>" \
DESCRIPTION="KairosApp with dap shared library" \
./build-deb.sh
```

## Note importanti

- La versione cresce automaticamente (patch) a ogni build tramite file `VERSION`.
- Se cambi host/utente non usare path hardcoded in home dentro tooling o estensioni.
- Per Ubuntu, `.deb` e' il formato nativo consigliato.
