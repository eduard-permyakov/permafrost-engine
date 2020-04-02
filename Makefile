# ------------------------------------------------------------------------------
# Options 
# ------------------------------------------------------------------------------

PLAT ?= LINUX
TYPE ?= DEBUG

# ------------------------------------------------------------------------------
# Sources 
# ------------------------------------------------------------------------------

PF_DIRS = $(sort $(dir $(wildcard ./src/*/)))
PF_SRCS = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)*.c))
PF_OBJS = $(PF_SRCS:./src/%.c=./obj/%.o)
PF_DEPS = $(PF_OBJS:%.o=%.d)

# ------------------------------------------------------------------------------
# Library Dependencies
# ------------------------------------------------------------------------------

GLEW_SRC = ./deps/GLEW
SDL2_SRC = ./deps/SDL2
PYTHON_SRC = ./deps/Python
PYTHON_VER_MAJOR = 2.7

# ------------------------------------------------------------------------------
# Linux
# ------------------------------------------------------------------------------

LINUX_GLEW_LIB = libGLEW.so.2.2
LINUX_SDL2_LIB = libSDL2-2.0.so.0
LINUX_PYTHON_LIB = libpython2.7.so.1.0
LINUX_SDL2_CONFIG = --host=x86_64-pc-linux-gnu

LINUX_CC = gcc
LINUX_BIN = ./bin/pf
LINUX_LDFLAGS = \
	-l:$(SDL2_LIB) \
	-l:$(GLEW_LIB) \
	-l:$(PYTHON_LIB) \
	-lGL \
	-ldl \
	-lutil \
	-Xlinker -export-dynamic \
	-Xlinker -rpath='$$ORIGIN/../lib'

# ------------------------------------------------------------------------------
# Windows
# ------------------------------------------------------------------------------

WINDOWS_GLEW_LIB = glew32.dll
WINDOWS_SDL2_LIB = SDL2.dll
WINDOWS_PYTHON_LIB = python27.dll

WINDOWS_SDL2_CONFIG = --host=x86_64-w64-mingw32
WINDOWS_GLEW_OPTS = "SYSTEM=linux-mingw64"

WINDOWS_CC = x86_64-w64-mingw32-gcc
WINDOWS_BIN = ./lib/pf.exe
WINDOWS_LDFLAGS = \
	-lmingw32 \
	-lSDL2 \
	-lglew32 \
	-lpython27 \
	-lopengl32

WINDOWS_DEFS = -DMS_WIN64

# ------------------------------------------------------------------------------
# Platform-Agnostic
# ------------------------------------------------------------------------------

CC = $($(PLAT)_CC)
BIN = $($(PLAT)_BIN)
PLAT_LDFLAGS = $($(PLAT)_LDFLAGS)
DEFS = $($(PLAT)_DEFS)

GLEW_LIB = $($(PLAT)_GLEW_LIB)
SDL2_LIB = $($(PLAT)_SDL2_LIB)
PYTHON_LIB = $($(PLAT)_PYTHON_LIB)

SDL2_CONFIG = $($(PLAT)_SDL2_CONFIG)
GLEW_OPTS = $($(PLAT)_GLEW_OPTS)

WARNING_FLAGS = \
	-Wall \
	-Wno-missing-braces \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Werror

EXTRA_DEBUG_FLAGS = -g
EXTRA_RELEASE_FLAGS = -DNDEBUG
EXTRA_FLAGS = $(EXTRA_$(TYPE)_FLAGS)

CFLAGS = \
	-I$(GLEW_SRC)/include \
	-I$(SDL2_SRC)/include \
	-I$(PYTHON_SRC)/Include \
	-std=c99 \
	-O2 \
	-march=native \
	-fno-strict-aliasing \
	-fwrapv \
	$(WARNING_FLAGS) \
	$(EXTRA_FLAGS)

LDFLAGS = \
	-L./lib/ \
	-lm \
	-lpthread \
	$(PLAT_LDFLAGS)

DEPS = \
	./lib/$(GLEW_LIB) \
	./lib/$(SDL2_LIB) \
	./lib/$(PYTHON_LIB)

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------

.PHONY: download_windows_python

download_windows_python:
	wget -O python27.msi https://www.python.org/ftp/python/2.7.17/python-2.7.17.amd64.msi
	msiexec /i python27.msi /qb TARGETDIR=python27
	cp python27/python27.dll ./lib/.
	cp -r python27/Lib ./lib/.
	cp -r python27/DLLs ./lib/.
	cp python27/include/pyconfig.h ./deps/Python/Include/.
	rm -f python27.msi
	rm -rf python27

./lib/$(GLEW_LIB):
	mkdir -p ./lib
	make -C $(GLEW_SRC) extensions
	make -C $(GLEW_SRC) $(GLEW_OPTS) glew.lib.shared
	cp $(GLEW_SRC)/lib/$(GLEW_LIB) $@

./lib/$(SDL2_LIB):
	mkdir -p ./lib
	mkdir -p $(SDL2_SRC)/build
	cd $(SDL2_SRC)/build \
		&& ../configure $(SDL2_CONFIG) \
		&& make
	cp $(SDL2_SRC)/build/build/.libs/$(SDL2_LIB) $@

./lib/$(PYTHON_LIB):
ifeq ($(PLAT),WINDOWS)
	$(error "Python must be built using MSVC build tools.")
endif
	mkdir -p ./lib/pyinstall/lib
	mkdir -p $(PYTHON_SRC)/build
	cd $(PYTHON_SRC)/build \
	&& ../configure \
		--enable-shared \
		--without-threads \
		--without-signal-module \
	&& cp ./pyconfig.h ../Include/. \
	&& make libpython2.7.so
	cp $(PYTHON_SRC)/build/$(PYTHON_LIB) $@

deps: $(DEPS)

./obj/%.o: ./src/%.c
	@mkdir -p $(dir $@)
	@printf "%-8s %s\n" "[CC]" $@
	@$(CC) -MT $@ -MMD -MP -MF ./obj/$*.d $(CFLAGS) $(DEFS) -c $< -o $@

$(BIN): $(PF_OBJS)
	@mkdir -p ./bin
	@printf "%-8s %s\n" "[LD]" $@
	@$(CC) $^ -o $(BIN) $(LDFLAGS)

-include $(PF_DEPS)

.PHONY: pf clean run run_editor clean_deps launchers

pf: $(BIN)

clean_deps:
	git submodule foreach git reset --hard	
	rm -rf ./lib/*

clean:
	rm -rf $(PF_OBJS) $(PF_DEPS) $(BIN) 

run:
	@$(BIN) ./ ./scripts/rts/main.py

run_editor:
	@$(BIN) ./ ./scripts/editor/main.py

launchers:
ifeq ($(PLAT),WINDOWS)
	make -C launcher BIN_PATH='.\\\\lib\\\\pf.exe' SCRIPT_PATH="./scripts/rts/main.py" BIN="../demo.exe" launcher
	make -C launcher BIN_PATH='.\\\\lib\\\\pf.exe' SCRIPT_PATH="./scripts/editor/main.py" BIN="../editor.exe" launcher
else
	make -C launcher BIN_PATH=$(BIN) SCRIPT_PATH="./scripts/rts/main.py" BIN="../demo" launcher
	make -C launcher BIN_PATH=$(BIN) SCRIPT_PATH="./scripts/editor/main.py" BIN="../editor" launcher
endif

