# ============================================================
#  Janus — Makefile
#  Targets: all, run, test, test-one, release, clean, help
# ============================================================

PYTHON      := ./venv/bin/python
PYINSTALLER := $(abspath ./venv/bin/pyinstaller)

SRC_DIR     := src
VM_DIR      := $(SRC_DIR)/vm
LIBVM       := build/libvm.so
DIST_DIR    := build/dist
JPROGRAMS   := tests

# Flags di compilazione
CC          := gcc
CFLAGS      := -shared -fPIC -Wall -Wextra
CFLAGS_DBG  := $(CFLAGS) -g -fsanitize=address,undefined -DDEBUG
CFLAGS_REL  := $(CFLAGS) -O2 -DNDEBUG -Wno-stringop-truncation

# Colori per output
RED    := \033[0;31m
GREEN  := \033[0;32m
YELLOW := \033[1;33m
CYAN   := \033[0;36m
RESET  := \033[0m

.PHONY: all run test test-one release clean help \
        build-debug build-release install-deps check-deps

# ============================================================
#  Default: compila la VM in modalità debug
# ============================================================
all: check-deps $(LIBVM)

$(LIBVM): $(VM_DIR)/Janus.c $(wildcard $(VM_DIR)/*.h)
	@echo "$(CYAN)Compilazione VM (debug)...$(RESET)"
	$(CC) $(CFLAGS_DBG) -o $(LIBVM) $(VM_DIR)/Janus.c -I$(VM_DIR)
	@echo "$(GREEN)VM compilata: $(LIBVM)$(RESET)"

# ============================================================
#  build-debug / build-release
# ============================================================
build-debug: check-deps
	@echo "$(CYAN)Build debug con AddressSanitizer...$(RESET)"
	$(CC) $(CFLAGS_DBG) -o $(LIBVM) $(VM_DIR)/Janus.c -I$(VM_DIR)
	@echo "$(GREEN)Build debug OK$(RESET)"

build-release: check-deps
	@echo "$(CYAN)Build release (-O2)...$(RESET)"
	$(CC) $(CFLAGS_REL) -o $(LIBVM) $(VM_DIR)/Janus.c -I$(VM_DIR)
	@echo "$(GREEN)Build release OK$(RESET)"

# ============================================================
#  run — esegue un singolo file .janus
#  Uso: make run FILE=Jprograms/test.janus
# ============================================================
run: $(LIBVM)
ifndef FILE
	$(error Specifica il file: make run FILE=Jprograms/test.janus)
endif
	@echo "$(CYAN)Esecuzione: $(FILE)$(RESET)"
	$(PYTHON) -m src.janus $(FILE) --dump-bytecode

# ============================================================
#  test — esegue tutti i .janus in Jprograms/
#  Stampa PASS / FAIL per ciascuno.
# ============================================================
JANUS_FILES := $(wildcard $(JPROGRAMS)/*.janus)

test: build-release
	@echo "$(CYAN)=== Test suite ===$(RESET)"
	@passed=0; failed=0; errors=""; \
	for f in $(JANUS_FILES); do \
		name=$$(basename $$f); \
		output=$$($(PYTHON) -m src.janus $$f --dump-bytecode 2>&1); \
		if echo "$$output" | grep -qiE "error|DELOCAL.*errato|stack overflow|assertion"; then \
			echo "  $(RED)FAIL$(RESET)  $$name"; \
			errors="$$errors\n--- $$name ---\n$$output\n"; \
			failed=$$((failed+1)); \
		else \
			echo "  $(GREEN)PASS$(RESET)  $$name"; \
			passed=$$((passed+1)); \
		fi; \
	done; \
	echo ""; \
	echo "$(CYAN)Risultati: $(GREEN)$$passed PASS$(RESET) / $(RED)$$failed FAIL$(RESET)"; \
	if [ $$failed -gt 0 ]; then \
		echo ""; \
		echo "$(RED)Dettaglio errori:$(RESET)"; \
		printf "$$errors"; \
		exit 1; \
	fi

# ============================================================
#  test-one — esegue un singolo test con output verboso
#  Uso: make test-one FILE=Jprograms/fib.janus
# ============================================================
test-one: $(LIBVM)
ifndef FILE
	$(error Specifica il file: make test-one FILE=Jprograms/fib.janus)
endif
	@echo "$(CYAN)=== Test: $(FILE) ===$(RESET)"
	@output=$$($(PYTHON) -m src.janus $(FILE) --dump-bytecode 2>&1); \
	echo "$$output"; \
	echo ""; \
	if echo "$$output" | grep -qiE "error|DELOCAL.*errato|stack overflow"; then \
		echo "$(RED)FAIL$(RESET)"; exit 1; \
	else \
		echo "$(GREEN)PASS$(RESET)"; \
	fi

# ============================================================
#  release — build ottimizzata + pacchetto PyInstaller
# ============================================================
release: build-release
	@echo "$(CYAN)Packaging con PyInstaller...$(RESET)"
	cd $(SRC_DIR) && $(PYINSTALLER) --onefile \
		--name JanusApp \
		--add-binary "VM/libvm.so:VM" \
		--hidden-import=ply \
		--hidden-import=ply.yacc \
		--hidden-import=ply.lex \
		Janus.py
	@echo "$(GREEN)Release pronta: $(DIST_DIR)/JanusApp$(RESET)"
	@echo "$(YELLOW)SHA256: $$(sha256sum $(DIST_DIR)/JanusApp | cut -d' ' -f1)$(RESET)"

# ============================================================
#  install-deps — crea venv e installa dipendenze
# ============================================================
install-deps:
	@echo "$(CYAN)Creazione venv e installazione dipendenze...$(RESET)"
	python3 -m venv venv
	./venv/bin/pip install --upgrade pip
	./venv/bin/pip install ply pyinstaller
	@echo "$(GREEN)Dipendenze installate$(RESET)"

# ============================================================
#  check-deps — verifica che venv e ply siano presenti
# ============================================================
check-deps:
	@test -f $(PYTHON) || (echo "$(RED)venv non trovato. Esegui: make install-deps$(RESET)" && exit 1)
	@$(PYTHON) -c "import ply" 2>/dev/null || \
		(echo "$(RED)ply non trovato. Esegui: make install-deps$(RESET)" && exit 1)

# ============================================================
#  clean — rimuove tutti gli artefatti generati
# ============================================================
clean:
	@echo "$(YELLOW)Pulizia...$(RESET)"
	rm -f $(LIBVM)
	rm -rf $(SRC_DIR)/build $(SRC_DIR)/dist $(SRC_DIR)/__pycache__ $(SRC_DIR)/*.spec
	rm -f $(SRC_DIR)/parser.out $(SRC_DIR)/parsetab.py
	find . -name "*.pyc" -delete
	find . -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null; true
	@echo "$(GREEN)Pulizia completata$(RESET)"

# ============================================================
#  help
# ============================================================
help:
	@echo ""
	@echo "$(CYAN)Janus — Toolchain$(RESET)"
	@echo ""
	@echo "  $(GREEN)make$(RESET)                        Compila la VM (debug + ASan)"
	@echo "  $(GREEN)make build-debug$(RESET)            Compila con -g e AddressSanitizer"
	@echo "  $(GREEN)make build-release$(RESET)          Compila con -O2 per la produzione"
	@echo "  $(GREEN)make run FILE=<f.janus>$(RESET)     Esegue un singolo programma"
	@echo "  $(GREEN)make test$(RESET)                   Esegue tutti i .janus in Jprograms/"
	@echo "  $(GREEN)make test-one FILE=<f.janus>$(RESET) Esegue un test con output verboso"
	@echo "  $(GREEN)make release$(RESET)                Build ottimizzata + pacchetto standalone"
	@echo "  $(GREEN)make install-deps$(RESET)           Crea venv e installa ply/pyinstaller"
	@echo "  $(GREEN)make clean$(RESET)                  Rimuove tutti gli artefatti"
	@echo ""