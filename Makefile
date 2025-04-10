# ------------------------------------------------------------------------------
# Options 
# ------------------------------------------------------------------------------

PLAT ?= LINUX
TYPE ?= DEBUG
ASAN ?= 0
TSAN ?= 0
LTO  ?= 0
GIT_VERSION := "$(shell git describe --always)"

# ------------------------------------------------------------------------------
# Sources 
# ------------------------------------------------------------------------------

PF_DIRS = $(sort $(dir $(wildcard ./src/*/), ./src/))
PF_SRCS = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)*.c))
PF_SRC_OBJS = $(PF_SRCS:./src/%.c=./obj/%.o)
PF_ASM = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)*.S))
PF_ASM_OBJS = $(PF_ASM:./src/%.S=./obj/%.o)
PF_OBJS = $(PF_SRC_OBJS) $(PF_ASM_OBJS)
PF_DEPS = $(PF_SRC_OBJS:%.o=%.d)

# ------------------------------------------------------------------------------
# Library Dependencies
# ------------------------------------------------------------------------------

GLEW_SRC = ./deps/GLEW
SDL2_SRC = ./deps/SDL2
PYTHON_SRC = ./deps/Python
OPENAL_SRC = ./deps/openal-soft
MIMALLOC_SRC = ./deps/mimalloc

# ------------------------------------------------------------------------------
# Linux
# ------------------------------------------------------------------------------

LINUX_GLEW_LIB = libGLEW.so.2.2
LINUX_SDL2_LIB = libSDL2-2.0.so.0
LINUX_PYTHON_LIB = libpython2.7.so.1.0
LINUX_PYTHON_TARGET = libpython2.7.so
LINUX_OPENAL_LIB = libopenal.so.1

LINUX_MIMALLOC_DEBUG_LIB = libmimalloc-debug.so.2
LINUX_MIMALLOC_RELEASE_LIB = libmimalloc.so.2
LINUX_MIMALLOC_LIB = $(LINUX_MIMALLOC_$(TYPE)_LIB)

LINUX_SDL2_CONFIG = --host=x86_64-pc-linux-gnu
LINUX_PYTHON_CONFIG = --host=x86_64-pc-linux-gnu
LINUX_OPENAL_OPTS = "-DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF"

LINUX_CC = gcc
LINUX_BIN = ./bin/pf
LINUX_LDFLAGS = \
	-l:$(MIMALLOC_LIB) \
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

WINDOWS_MIMALLOC_DEBUG_LIB = mimalloc-debug.dll
WINDOWS_MIMALLOC_RELEASE_LIB = mimalloc.dll
WINDOWS_MIMALLOC_LIB = $(WINDOWS_MIMALLOC_$(TYPE)_LIB)

WINDOWS_MIMALLOC_DEBUG_LINKER_FLAG = -lmimalloc-redirect -lmimalloc-debug
WINDOWS_MIMALLOC_RELEASE_LINKER_FLAG = -lmimalloc-redirect -lmimalloc
WINDOWS_MIMALLOC_LINKER_FLAG = $(WINDOWS_MIMALLOC_$(TYPE)_LINKER_FLAG)

ifneq ($(OS),Windows_NT)
WINDOWS_GLEW_OPTS = "SYSTEM=linux-mingw64"
endif

WINDOWS_SDL2_CONFIG = --host=x86_64-w64-mingw32
WINDOWS_PYTHON_CONFIG = --host=x86_64-w64-mingw32
WINDOWS_PYTHON_DEFS = "-D__USE_MINGW_ANSI_STDIO=1 -D__MINGW32__"
WINDOWS_PYTHON_TARGET = libpython2.7.dll
WINDOWS_OPENAL_OPTS = -DCMAKE_TOOLCHAIN_FILE=XCompile.txt -DHOST=x86_64-w64-mingw32 -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF
WINDOWS_MIMALLOC_OPTS = \
	-DCMAKE_SYSTEM_NAME=Windows \
	-DCMAKE_SYSTEM_PROCESSOR=x86_64 \
	-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
	-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
	-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres

WINDOWS_CC = x86_64-w64-mingw32-gcc
WINDOWS_BIN = ./lib/pf.exe
WINDOWS_LDFLAGS = \
	$(WINDOWS_MIMALLOC_LINKER_FLAG) \
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
DEFS = $($(PLAT)_DEFS) -DGIT_VERSION=\"$(GIT_VERSION)\"

GLEW_LIB = $($(PLAT)_GLEW_LIB)
SDL2_LIB = $($(PLAT)_SDL2_LIB)
PYTHON_LIB = $($(PLAT)_PYTHON_LIB)
OPENAL_LIB = $($(PLAT)_OPENAL_LIB)
MIMALLOC_LIB = $($(PLAT)_MIMALLOC_LIB)

SDL2_CONFIG = $($(PLAT)_SDL2_CONFIG)
PYTHON_CONFIG = $($(PLAT)_PYTHON_CONFIG)
PYTHON_DEFS = $($(PLAT)_PYTHON_DEFS)
PYTHON_TARGET = $($(PLAT)_PYTHON_TARGET)
GLEW_OPTS = $($(PLAT)_GLEW_OPTS)
OPENAL_OPTS = $($(PLAT)_OPENAL_OPTS)

MIMALLOC_DEBUG_OPTS = -DCMAKE_BUILD_TYPE=Debug
MIMALLOC_RELEASE_OPTS = -DCMAKE_BUILD_TYPE=Release
MIMALLOC_OPTS = $(MIMALLOC_$(TYPE)_OPTS) $($(PLAT)_MIMALLOC_OPTS)

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

ifneq ($(TSAN),0)
TSAN_CFLAGS = -fsanitize=thread -static-libtsan
TSAN_LDFLAGS = -fsanitize=thread -static-libtsan
endif

ifneq ($(LTO),0)
LTO_CFLAGS = -flto
LTO_LDFLAGS = -flto
endif

CFLAGS = \
	-I$(GLEW_SRC)/include \
	-I$(SDL2_SRC)/include \
	-I$(PYTHON_SRC)/Include \
	-I$(OPENAL_SRC)/include \
	-I$(MIMALLOC_SRC)/include \
	-std=c99 \
	-O3 \
	-fno-strict-aliasing \
	-fwrapv \
	$(ASAN_CFLAGS) \
	$(TSAN_CFLAGS) \
	$(WARNING_FLAGS) \
	$(LTO_CFLAGS) \
	$(EXTRA_FLAGS)

LDFLAGS = \
	-L./lib/ \
	-lm \
	-lpthread \
	$(ASAN_LDFLAGS) \
	$(TSAN_LDFLAGS) \
	$(LTO_LDFLAGS) \
	$(PLAT_LDFLAGS)

DEPS = \
	./lib/$(GLEW_LIB) \
	./lib/$(SDL2_LIB) \
	./lib/$(PYTHON_LIB) \
	./lib/$(OPENAL_LIB) \
	./lib/$(MIMALLOC_LIB)

ifeq ($(PLAT),WINDOWS)
DEPS += ./lib/mimalloc-redirect.dll
endif

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
		--disable-ipv6 \
		--without-threads \
		--without-signal-module \
	&& cp ./pyconfig.h ../Include/. \
	&& make $(PYTHON_TARGET) CFLAGS=$(PYTHON_DEFS)
	cp $(PYTHON_SRC)/build/$(PYTHON_TARGET) $@

./lib/$(OPENAL_LIB):
	mkdir -p $(OPENAL_SRC)/build
ifeq ($(OS),Windows_NT)
	ln -sf $(shell which windres) $(OPENAL_SRC)/build/x86_64-w64-mingw32-windres
endif
	cd $(OPENAL_SRC)/build \
		&& export PATH="$(shell pwd)/$(OPENAL_SRC)/build:$$PATH" \
		&& echo $$PATH \
		&& cmake .. $(OPENAL_OPTS) \
		-DCMAKE_CXX_FLAGS="-I. -I.. -I../include -I../alc -I../common" \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		&& make 
	cp $(OPENAL_SRC)/build/$(OPENAL_LIB) $@

./lib/$(MIMALLOC_LIB):
	mkdir -p $(MIMALLOC_SRC)/build
	cd $(MIMALLOC_SRC)/build \
		&& export CFLAGS="-DERROR_COMMITMENT_MINIMUM=0x0000027B" \
		&& cmake .. $(MIMALLOC_OPTS) \
		&& make
	cp $(MIMALLOC_SRC)/build/$(MIMALLOC_LIB) $@

./lib/mimalloc-redirect.dll: ./lib/$(MIMALLOC_LIB)
	cp $(MIMALLOC_SRC)/build/$(notdir $@) $@

deps: $(DEPS)

$(PF_SRC_OBJS): ./obj/%.o: ./src/%.c
	@mkdir -p $(dir $@)
	@printf "%-8s %s\n" "[CC]" $@
	@$(CC) -MT $@ -MMD -MP -MF ./obj/$*.d $(CFLAGS) $(DEFS) -c $< -o $@

$(PF_ASM_OBJS): ./obj/%.o: ./src/%.S
	@mkdir -p $(dir $@)
	@printf "%-8s %s\n" "[AS]" $@
	@$(CC) $(CFLAGS) $(DEFS) -c $< -o $@

$(BIN): $(PF_OBJS)
	@mkdir -p ./bin
	@printf "%-8s %s\n" "[LD]" $@
	@$(CC) $^ -o $(BIN) $(LDFLAGS)

./obj/version.o: .FORCE
.FORCE:

-include $(PF_DEPS)

.PHONY: pf clean run run_editor clean_deps launchers .FORCE

pf: $(BIN)

clean_deps:
	cd deps/GLEW && git clean -f -d
	cd deps/SDL2 && git clean -f -d
	cd deps/Python && git clean -f -d
	cd deps/openal-soft && git clean -f -d
	rm -rf deps/SDL2/build
	rm -rf deps/Python/build
	rm -rf deps/openal-soft/build
	rm -rf deps/mimalloc/build
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

