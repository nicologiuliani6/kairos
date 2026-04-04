.PHONY: all run build_app clean

PYTHON=./venv/bin/python
PYINSTALLER=/home/nico/.local/bin/pyinstaller

SRC_DIR=src
VM_DIR=$(SRC_DIR)/VM
JANUS_SCRIPT=$(SRC_DIR)/Janus.py
LIBVM=$(VM_DIR)/libvm.so
DIST_DIR=$(SRC_DIR)/dist

# -----------------------------
# Default: build VM
# -----------------------------
all: $(LIBVM)

# Compila la VM condivisa
$(LIBVM): $(VM_DIR)/Janus.c
	gcc -shared -fPIC -o $(LIBVM) $(VM_DIR)/Janus.c -I$(VM_DIR) -Wall
	@echo "VM compilata: $(LIBVM)"

# -----------------------------
# Run: esegue file Janus
# -----------------------------
run: $(LIBVM)
ifndef FILE
	$(error Devi passare il file! Es: make run FILE=src/Jprograms/test.janus)
endif
	$(PYTHON) -m src.Janus $(FILE) --dump-bytecode

# -----------------------------
# Build standalone (PyInstaller globale)
# -----------------------------
build_app: $(LIBVM)
	cd $(SRC_DIR) && $(PYINSTALLER) --onefile \
		--name JanusApp \
		--add-binary "VM/libvm.so:VM" \
		--hidden-import=ply \
		--hidden-import=ply.yacc \
		--hidden-import=ply.lex \
		Janus.py
	@echo "Build completata: $(DIST_DIR)/JanusApp"

# -----------------------------
# Pulizia
# -----------------------------
clean:
	rm -f $(LIBVM)
	rm -rf $(SRC_DIR)/build $(SRC_DIR)/dist $(SRC_DIR)/__pycache__ $(SRC_DIR)/*.spec
	rm $(SRC_DIR)/parser.out $(SRC_DIR)/parsetab.py
	@echo "Pulizia completata"