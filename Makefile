# ------------------------------------------------------------------------------
# Options 
# ------------------------------------------------------------------------------

PLAT ?= LINUX
TYPE ?= DEBUG
ASAN ?= 0

# ------------------------------------------------------------------------------
# Sources 
# ------------------------------------------------------------------------------

PF_DIRS = $(sort $(dir $(wildcard ./src/*/), ./src/))
PF_SRCS = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)*.c))
PF_OBJS = $(PF_SRCS:./src/%.c=./obj/%.o)
PF_DEPS = $(PF_OBJS:%.o=%.d)

# ------------------------------------------------------------------------------
# Library Dependencies
# ------------------------------------------------------------------------------

GLEW_SRC = ./deps/GLEW
SDL2_SRC = ./deps/SDL2
PYTHON_SRC = ./deps/Python
OPENAL_SRC = ./deps/openal-soft

# ------------------------------------------------------------------------------
# Linux
# ------------------------------------------------------------------------------

LINUX_GLEW_LIB = libGLEW.so.2.2
LINUX_SDL2_LIB = libSDL2-2.0.so.0
LINUX_PYTHON_LIB = libpython2.7.so.1.0
LINUX_PYTHON_TARGET = libpython2.7.so
LINUX_OPENAL_LIB = libopenal.so.1.21.1

LINUX_SDL2_CONFIG = --host=x86_64-pc-linux-gnu
LINUX_PYTHON_CONFIG = --host=x86_64-pc-linux-gnu
LINUX_OPENAL_OPTS = "-DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF"

LINUX_CC = gcc
LINUX_BIN = ./bin/pf
LINUX_LDFLAGS = \
	-l:$(SDL2_LIB) \
	-l:$(GLEW_LIB) \
	-l:$(PYTHON_LIB) \
	-l:$(OPENAL_LIB) \
	-lGL \
	-ldl \
	-lutil \
	-Xlinker -export-dynamic \
	-Xlinker -rpath='$$ORIGIN/../lib'

LINUX_DEFS = -D_DEFAULT_SOURCE

# ------------------------------------------------------------------------------
# Windows
# ------------------------------------------------------------------------------

WINDOWS_GLEW_LIB = glew32.dll
WINDOWS_SDL2_LIB = SDL2.dll
WINDOWS_PYTHON_LIB = libpython2.7.dll
WINDOWS_OPENAL_LIB = OpenAL32.dll

WINDOWS_SDL2_CONFIG = --host=x86_64-w64-mingw32
WINDOWS_PYTHON_CONFIG = --host=x86_64-w64-mingw32
WINDOWS_PYTHON_DEFS = -D__USE_MINGW_ANSI_STDIO=1
WINDOWS_PYTHON_TARGET = libpython2.7.dll
WINDOWS_GLEW_OPTS = "SYSTEM=linux-mingw64"
WINDOWS_OPENAL_OPTS = -DCMAKE_TOOLCHAIN_FILE=XCompile.txt -DHOST=x86_64-w64-mingw32 -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF

WINDOWS_CC = x86_64-w64-mingw32-gcc
WINDOWS_BIN = ./lib/pf.exe
WINDOWS_LDFLAGS = \
	-lmingw32 \
	-lSDL2 \
	-lglew32 \
	-llibpython2.7 \
	-lOpenAL32 \
	-lopengl32 \
	-luuid

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
OPENAL_LIB = $($(PLAT)_OPENAL_LIB)

SDL2_CONFIG = $($(PLAT)_SDL2_CONFIG)
PYTHON_CONFIG = $($(PLAT)_PYTHON_CONFIG)
PYTHON_DEFS = $($(PLAT)_PYTHON_DEFS)
PYTHON_TARGET = $($(PLAT)_PYTHON_TARGET)
GLEW_OPTS = $($(PLAT)_GLEW_OPTS)
OPENAL_OPTS = $($(PLAT)_OPENAL_OPTS)

WARNING_FLAGS = \
	-Wall \
	-Wno-missing-braces \
	-Wno-unused-function \
	-Wno-unused-variable \
	-Werror

EXTRA_DEBUG_FLAGS = -g
EXTRA_RELEASE_FLAGS = -DNDEBUG
EXTRA_FLAGS = $(EXTRA_$(TYPE)_FLAGS)

ifneq ($(ASAN),0)
ASAN_CFLAGS = -fsanitize=address -static-libasan
ASAN_LDFLAGS = -fsanitize=address -static-libasan
endif

CFLAGS = \
	-I$(GLEW_SRC)/include \
	-I$(SDL2_SRC)/include \
	-I$(PYTHON_SRC)/Include \
	-I$(OPENAL_SRC)/include \
	-std=c99 \
	-O2 \
	-fno-strict-aliasing \
	-fwrapv \
	$(ASAN_CFLAGS) \
	$(WARNING_FLAGS) \
	$(EXTRA_FLAGS)

LDFLAGS = \
	-L./lib/ \
	-lm \
	-lpthread \
	$(ASAN_LDFLAGS) \
	$(PLAT_LDFLAGS)

DEPS = \
	./lib/$(GLEW_LIB) \
	./lib/$(SDL2_LIB) \
	./lib/$(PYTHON_LIB) \
	./lib/$(OPENAL_LIB)

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------

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
	mkdir -p $(PYTHON_SRC)/build
	cd $(PYTHON_SRC)/build \
	&& ../configure \
		$(PYTHON_CONFIG) \
		--build=x86_64-pc-linux-gnu \
		--enable-shared \
		--without-threads \
		--without-signal-module \
	&& cp ./pyconfig.h ../Include/. \
	&& make $(PYTHON_TARGET) CFLAGS=$(PYTHON_DEFS)
	cp $(PYTHON_SRC)/build/$(PYTHON_TARGET) $@

./lib/$(OPENAL_LIB):
	mkdir -p $(OPENAL_SRC)/build
	cd $(OPENAL_SRC)/build \
		&& cmake .. $(OPENAL_OPTS) \
		&& make 
	cp $(OPENAL_SRC)/build/$(OPENAL_LIB) $@

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
	rm -rf deps/GLEW/lib
	rm -rf deps/SDL2/build	
	rm -rf deps/Python/build	
	rm -rf deps/openal-soft/build
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

