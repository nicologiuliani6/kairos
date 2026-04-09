# ============================================================
#  Kairos — Makefile (semplificato)
# ============================================================

PYTHON      := ./venv/bin/python
PYINSTALLER := $(abspath ./venv/bin/pyinstaller)

SRC_DIR     := src
VM_DIR      := $(SRC_DIR)/vm
LIBVM       := build/libvm.so
LIBVM_DAP   := build/libvm_dap.so
DIST_DIR    := build/dist
VM_SOURCES  := $(VM_DIR)/Janus.c $(VM_DIR)/Janus_dap.c

CC          := gcc
CFLAGS      := -shared -fPIC -Wall -Wextra -pthread
CFLAGS_REL  := $(CFLAGS) -O2 -DNDEBUG -Wno-stringop-truncation
CFLAGS_DAP  := $(CFLAGS) -g -DDEBUG -DDAP_MODE
VERSCRIPT   := $(VM_DIR)/libvm.map

RED    := \033[0;31m
GREEN  := \033[0;32m
YELLOW := \033[1;33m
CYAN   := \033[0;36m
RESET  := \033[0m

.PHONY: all build-release build-dap run test release install-deps check-deps clean help

# Default: build ottimizzata
all: build-release

build-release: check-deps $(VERSCRIPT)
	@mkdir -p build
	@echo "$(CYAN)Build release (-O2)...$(RESET)"
	$(CC) $(CFLAGS_REL) \
	    -Wl,--version-script=$(VERSCRIPT) \
	    -o $(LIBVM) $(VM_SOURCES) -I$(VM_DIR)
	@echo "$(GREEN)Build release OK$(RESET)"

build-dap: check-deps
	@mkdir -p build
	@echo "$(CYAN)Build DAP...$(RESET)"
	$(CC) $(CFLAGS_DAP) -o $(LIBVM_DAP) $(VM_SOURCES) -I$(VM_DIR)
	@echo "$(GREEN)Build DAP OK: $(LIBVM_DAP)$(RESET)"

# Genera il version-script con i simboli pubblici del debugger
$(VERSCRIPT): $(VM_SOURCES)
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

run: build-release
ifndef FILE
	$(error Specifica il file: make run FILE=tests/test.kairos)
endif
	@echo "$(CYAN)Esecuzione: $(FILE)$(RESET)"
	$(PYTHON) -m src.kairos $(FILE) --dump-bytecode

KAIROS_FILES := $(wildcard tests/*.kairos examples/*.kairos)

test: build-release
	@echo "$(CYAN)=== Test suite ===$(RESET)"
	@passed=0; failed=0; errors=""; \
	for f in $(KAIROS_FILES); do \
		name=$$(basename $$f); \
		output=$$(timeout 5s $(PYTHON) -m src.kairos $$f --dump-bytecode 2>&1); \
		status=$$?; \
		if [ $$status -eq 124 ]; then \
			echo "  $(RED)TIMEOUT$(RESET)  $$name"; \
			errors="$$errors\n--- $$name ---\nTest interrotto per timeout (5s)\n"; \
			failed=$$((failed+1)); \
		elif echo "$$output" | grep -qiE "error|DELOCAL.*errato|stack overflow|assertion"; then \
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

release: build-release
	@echo "$(CYAN)Build release KairosApp...$(RESET)"
	@mkdir -p $(DIST_DIR)
	$(PYINSTALLER) --onefile \
		--log-level ERROR \
		--distpath $(DIST_DIR) \
		--workpath build/pyinstaller-work \
		--specpath build \
		--name KairosApp \
	    $(SRC_DIR)/kairos.py
	@echo "$(GREEN)Build release OK: $(DIST_DIR)/KairosApp$(RESET)"

install-deps:
	@echo "$(CYAN)Creazione venv e installazione dipendenze...$(RESET)"
	python3 -m venv venv
	./venv/bin/pip install --upgrade pip
	./venv/bin/pip install ply pyinstaller
	@echo "$(GREEN)Dipendenze installate$(RESET)"

check-deps:
	@test -f $(PYTHON) || (echo "$(RED)venv non trovato. Esegui: make install-deps$(RESET)" && exit 1)
	@$(PYTHON) -c "import ply" 2>/dev/null || \
		(echo "$(RED)ply non trovato. Esegui: make install-deps$(RESET)" && exit 1)

clean:
	@echo "$(YELLOW)Pulizia...$(RESET)"
	rm -f $(LIBVM) $(LIBVM_DAP) $(VERSCRIPT)
	rm -rf build/pyinstaller-work build/dist
	rm -rf $(SRC_DIR)/build $(SRC_DIR)/dist $(SRC_DIR)/__pycache__ $(SRC_DIR)/*.spec
	rm -f $(SRC_DIR)/parser.out $(SRC_DIR)/parsetab.py
	find . -name "*.pyc" -delete
	find . -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null; true
	@echo "$(GREEN)Pulizia completata$(RESET)"

help:
	@echo ""
	@echo "$(CYAN)Kairos — Toolchain (semplificata)$(RESET)"
	@echo ""
	@echo "  $(GREEN)make$(RESET)                         Build release (default)"
	@echo "  $(GREEN)make build-release$(RESET)           Compila libvm.so (-O2)"
	@echo "  $(GREEN)make build-dap$(RESET)               Compila libvm_dap.so"
	@echo "  $(GREEN)make run FILE=<f.kairos>$(RESET)      Esegue un singolo programma"
	@echo "  $(GREEN)make test$(RESET)                    Esegue tutti i test .kairos"
	@echo "  $(GREEN)make release$(RESET)                 Build KairosApp con PyInstaller"
	@echo "  $(GREEN)make install-deps$(RESET)            Crea venv e installa dipendenze"
	@echo "  $(GREEN)make clean$(RESET)                   Rimuove artefatti generati"
	@echo ""