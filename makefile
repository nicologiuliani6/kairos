# ============================================================
#  Janus — Makefile
#  Targets: all, run, test, test-one, release, clean, help
# ============================================================

PYTHON      := ./venv/bin/python
PYINSTALLER := $(abspath ./venv/bin/pyinstaller)

SRC_DIR     := src
VM_DIR      := $(SRC_DIR)/vm
LIBVM       := build/libvm.so
LIBVM_DAP   := build/libvm_dap.so
DIST_DIR    := build/dist
JPROGRAMS   := tests

# Flags di compilazione
CC          := gcc
CFLAGS      := -shared -fPIC -Wall -Wextra -pthread
CFLAGS_DBG  := $(CFLAGS) -g -fsanitize=address,undefined -DDEBUG
CFLAGS_REL  := $(CFLAGS) -O2 -DNDEBUG -Wno-stringop-truncation
CFLAGS_DAP  := $(CFLAGS) -g -DDEBUG -DDAP_MODE

# Version-script con i simboli pubblici del debugger
# Nota: --version-script non è compatibile con ASan, quindi solo in release.
VERSCRIPT   := $(VM_DIR)/libvm.map

# Colori per output
RED    := \033[0;31m
GREEN  := \033[0;32m
YELLOW := \033[1;33m
CYAN   := \033[0;36m
RESET  := \033[0m

.PHONY: all run test test-one release clean help \
        build-debug build-release build-dap install-deps check-deps

# ============================================================
#  Default: compila la VM in modalità debug
# ============================================================
all: check-deps $(LIBVM)

$(LIBVM): $(VM_DIR)/Janus.c $(wildcard $(VM_DIR)/*.h)
	@mkdir -p build
	@echo "$(CYAN)Compilazione VM (debug)...$(RESET)"
	$(CC) $(CFLAGS_DBG) -o $(LIBVM) $(VM_DIR)/Janus.c -I$(VM_DIR)
	@echo "$(GREEN)VM compilata: $(LIBVM)$(RESET)"

# ============================================================
#  build-debug / build-release
# ============================================================
build-debug: check-deps
	@mkdir -p build
	@echo "$(CYAN)Build debug con AddressSanitizer...$(RESET)"
	$(CC) $(CFLAGS_DBG) -o $(LIBVM) $(VM_DIR)/Janus.c -I$(VM_DIR)
	@echo "$(GREEN)Build debug OK$(RESET)"

build-release: check-deps $(VERSCRIPT)
	@mkdir -p build
	@echo "$(CYAN)Build release (-O2)...$(RESET)"
	$(CC) $(CFLAGS_REL) \
	    -Wl,--version-script=$(VERSCRIPT) \
	    -o $(LIBVM) $(VM_DIR)/Janus.c -I$(VM_DIR)
	@echo "$(GREEN)Build release OK$(RESET)"

build-dap: check-deps
	@mkdir -p build
	@echo "$(CYAN)Build DAP (senza ASan)...$(RESET)"
	$(CC) $(CFLAGS_DAP) -o $(LIBVM_DAP) $(VM_DIR)/Janus.c -I$(VM_DIR)
	@echo "$(GREEN)Build DAP OK: $(LIBVM_DAP)$(RESET)"
# Genera il version-script con i simboli pubblici del debugger
$(VERSCRIPT): $(VM_DIR)/Janus.c
	@printf 'LIBVM_1.0 {\n  global:\n' > $@
	@for sym in \
	    vm_run_from_string \
	    vm_debug_new \
	    vm_debug_free \
	    vm_debug_start \
	    vm_debug_stop \
	    vm_debug_step \
	    vm_debug_step_back \
	    vm_debug_continue \
	    vm_debug_continue_inverse \
	    vm_debug_goto_line \
	    vm_debug_set_breakpoint \
	    vm_debug_clear_breakpoint \
	    vm_debug_clear_all_breakpoints \
	    vm_debug_dump_json_ext \
		vm_debug_vars_json_ext \
    	vm_debug_output_ext; do \
	        printf '    %s;\n' $$sym >> $@; \
	done
	@printf '  local:\n    *;\n};\n' >> $@

# ============================================================
#  run — esegue un singolo file .janus
#  Uso: make run FILE=tests/test.janus
# ============================================================
run: $(LIBVM)
ifndef FILE
	$(error Specifica il file: make run FILE=tests/test.janus)
endif
	@echo "$(CYAN)Esecuzione: $(FILE)$(RESET)"
	# Verifica se la VM è stata compilata con ASan (DEBUG)
ifneq ($(findstring -fsanitize=address,$(CFLAGS_DBG)),)
	# Precarica libasan
	LD_PRELOAD=$(shell gcc -print-file-name=libasan.so) $(PYTHON) -m src.janus $(FILE) --dump-bytecode
else
	$(PYTHON) -m src.janus $(FILE) --dump-bytecode
endif

# ============================================================
#  test — esegue tutti i .janus in tests/
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
#  Uso: make test-one FILE=tests/fib.janus
# ============================================================
test-one: $(LIBVM)
ifndef FILE
	$(error Specifica il file: make test-one FILE=tests/fib.janus)
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
		--add-binary "vm/libvm.so:vm" \
		--hidden-import=ply \
		--hidden-import=ply.yacc \
		--hidden-import=ply.lex \
		janus.py
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
	rm -f $(LIBVM) $(VERSCRIPT)
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
	@echo "  $(GREEN)make$(RESET)                         Compila la VM (debug + ASan)"
	@echo "  $(GREEN)make build-debug$(RESET)             Compila con -g e AddressSanitizer"
	@echo "  $(GREEN)make build-release$(RESET)           Compila con -O2 per la produzione"
	@echo "  $(GREEN)make run FILE=<f.janus>$(RESET)      Esegue un singolo programma"
	@echo "  $(GREEN)make test$(RESET)                    Esegue tutti i .janus in tests/"
	@echo "  $(GREEN)make test-one FILE=<f.janus>$(RESET) Esegue un test con output verboso"
	@echo "  $(GREEN)make release$(RESET)                 Build ottimizzata + pacchetto standalone"
	@echo "  $(GREEN)make install-deps$(RESET)            Crea venv e installa ply/pyinstaller"
	@echo "  $(GREEN)make clean$(RESET)                   Rimuove tutti gli artefatti"
	@echo ""

# ============================================================
#  test-debug — linka con libvm (ASan) per sviluppo VM
#  Uso: make test-debug FILE=tests/fib.janus [BP=12]
# ============================================================
TEST_DEBUG_BIN     := build/test_debug
TEST_DEBUG_DAP_BIN := build/test_debug_dap

$(TEST_DEBUG_BIN): test_debug.c $(LIBVM)
	@echo "$(CYAN)Compilazione test_debug (ASan)...$(RESET)"
	$(CC) -O0 -g -fsanitize=address,undefined -o $@ test_debug.c \
	    -L./build -lvm -Wl,-rpath,./build -pthread
	@echo "$(GREEN)test_debug compilato$(RESET)"

test-debug: build-debug $(TEST_DEBUG_BIN)
ifndef FILE
	$(error Specifica il file: make test-debug FILE=tests/fib.janus)
endif
	@echo "$(CYAN)=== Debug (ASan): $(FILE) ===$(RESET)"
	LD_PRELOAD=$(shell gcc -print-file-name=libasan.so) $(TEST_DEBUG_BIN) $(FILE) $(BP)

# ============================================================
#  test-debug-dap — linka con libvm_dap (senza ASan), stesso
#                   binario usato dal DAP adapter Node.js
#  Uso: make test-debug-dap FILE=tests/fib.janus [BP=12]
# ============================================================
$(TEST_DEBUG_DAP_BIN): test_debug.c $(LIBVM_DAP)
	@echo "$(CYAN)Compilazione test_debug_dap (no ASan)...$(RESET)"
	$(CC) -O0 -g -o $@ test_debug.c \
	    -L./build -lvm_dap -Wl,-rpath,./build -pthread
	@echo "$(GREEN)test_debug_dap compilato$(RESET)"

test-debug-dap: build-dap $(TEST_DEBUG_DAP_BIN)
ifndef FILE
	$(error Specifica il file: make test-debug-dap FILE=tests/fib.janus)
endif
	@echo "$(CYAN)=== Debug DAP (no ASan): $(FILE) ===$(RESET)"
	$(TEST_DEBUG_DAP_BIN) $(FILE) $(BP)