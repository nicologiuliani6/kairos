# ============================================================
#  Kairos VSCode Extension — Makefile
# ============================================================

VSCE        := $(shell npm bin)/vsce
EXT_NAME    := kairos-debugger
VERSION     := $(shell node -p "require('./package.json').version")
VSIX        := $(EXT_NAME)-$(VERSION).vsix

.PHONY: all compile watch package install uninstall reinstall clean help

# ============================================================
#  Default: compila
# ============================================================
all: compile

# ============================================================
#  compile — transpila TypeScript → JavaScript
# ============================================================
compile:
	@echo "Compilazione TypeScript..."
	npm run compile
	@echo "Compilazione OK"

# ============================================================
#  watch — ricompila automaticamente al salvataggio
# ============================================================
watch:
	npm run watch

# ============================================================
#  package — genera il .vsix
# ============================================================
package: compile
	@echo "Generazione $(VSIX)..."
	npx @vscode/vsce package --allow-missing-repository --skip-license
	@echo "Pacchetto OK: $(VSIX)"

# ============================================================
#  install — installa l'estensione in VS Code
# ============================================================
install: package
	@echo "Installazione $(VSIX)..."
	code --install-extension $(VSIX)
	@echo "Installazione OK"

# ============================================================
#  uninstall — disinstalla l'estensione
# ============================================================
uninstall:
	@echo "Disinstallazione $(EXT_NAME)..."
	code --uninstall-extension $(EXT_NAME) || true
	@echo "Disinstallazione OK"

# ============================================================
#  reinstall — disinstalla, ricompila e reinstalla
# ============================================================
reinstall: uninstall install

# ============================================================
#  clean — rimuove artefatti generati
# ============================================================
clean:
	@echo "Pulizia..."
	rm -rf out *.vsix
	@echo "Pulizia OK"

# ============================================================
#  help
# ============================================================
help:
	@echo ""
	@echo "Kairos VSCode Extension — Comandi"
	@echo ""
	@echo "  make              Compila TypeScript"
	@echo "  make watch        Ricompila automaticamente"
	@echo "  make package      Genera il .vsix"
	@echo "  make install      Compila + pacchetta + installa in VS Code"
	@echo "  make reinstall    Disinstalla + installa da zero"
	@echo "  make uninstall    Disinstalla l'estensione"
	@echo "  make clean        Rimuove out/ e *.vsix"
	@echo ""