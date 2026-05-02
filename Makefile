# ------------------------------------------------------------------------------
# Options 
# ------------------------------------------------------------------------------

PLAT ?= LINUX
TYPE ?= DEBUG
ASAN ?= 0
TSAN ?= 0
LTO  ?= 0

ifeq ($(PLAT),MACOS_ARM64)
RENDER_BACKEND ?= METAL
else
RENDER_BACKEND ?= OPENGL
endif

GIT_VERSION := "$(shell git describe --always 2>/dev/null || echo archive)"

# ------------------------------------------------------------------------------
# Sources 
# ------------------------------------------------------------------------------

PF_DIRS = $(sort $(dir $(wildcard ./src/*/), ./src/))
PF_SRCS = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)*.c))
PF_ASM = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)*.S))
PF_OBJC = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)*.m))
ifeq ($(RENDER_BACKEND),METAL)
PF_SRCS := $(filter-out ./src/render/backend_gl.c,$(PF_SRCS))
endif
OBJ_DIR = ./obj/$(PLAT)-$(RENDER_BACKEND)
PF_SRC_OBJS = $(PF_SRCS:./src/%.c=$(OBJ_DIR)/%.o)
PF_ASM_OBJS = $(PF_ASM:./src/%.S=$(OBJ_DIR)/%.o)
PF_OBJC_OBJS = $(PF_OBJC:./src/%.m=$(OBJ_DIR)/%.o)
ifeq ($(RENDER_BACKEND),METAL)
PF_BACKEND_OBJS = $(PF_OBJC_OBJS)
else
PF_BACKEND_OBJS =
endif
PF_OBJS = $(PF_SRC_OBJS) $(PF_ASM_OBJS) $(PF_BACKEND_OBJS)
PF_DEPS = $(PF_SRC_OBJS:%.o=%.d) $(PF_OBJC_OBJS:%.o=%.d)
BACKEND_STAMP = $(OBJ_DIR)/.backend-stamp

# ------------------------------------------------------------------------------
# Library Dependencies
# ------------------------------------------------------------------------------

GLEW_SRC = ./deps/GLEW
SDL2_SRC = ./deps/SDL2
PYTHON_SRC = ./deps/Python
OPENAL_SRC = ./deps/openal-soft
MIMALLOC_SRC = ./deps/mimalloc
MACOS_HELPER_DIR = ./scripts/macos
HOMEBREW_X86_64_PREFIX ?= /usr/local
HOMEBREW_ARM64_PREFIX ?= /opt/homebrew
HOMEBREW_X86_64_PKGCONFIG_DIRS = $(HOMEBREW_X86_64_PREFIX)/lib/pkgconfig:$(HOMEBREW_X86_64_PREFIX)/share/pkgconfig:$(HOMEBREW_X86_64_PREFIX)/opt/openal-soft/lib/pkgconfig
HOMEBREW_ARM64_PKGCONFIG_DIRS = $(HOMEBREW_ARM64_PREFIX)/lib/pkgconfig:$(HOMEBREW_ARM64_PREFIX)/share/pkgconfig:$(HOMEBREW_ARM64_PREFIX)/opt/openal-soft/lib/pkgconfig
VENDORED_INCLUDE_FLAGS = \
	-I$(GLEW_SRC)/include \
	-I$(SDL2_SRC)/include \
	-I$(PYTHON_SRC)/Include \
	-I$(OPENAL_SRC)/include \
	-I$(MIMALLOC_SRC)/include

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
LINUX_INCLUDE_FLAGS = $(VENDORED_INCLUDE_FLAGS)
LINUX_CSTD = c99
LINUX_BUILD_READY = 1
LINUX_RUN_PREFIX =
LINUX_DEPS = \
	./lib/$(GLEW_LIB) \
	./lib/$(SDL2_LIB) \
	./lib/$(PYTHON_LIB) \
	./lib/$(OPENAL_LIB) \
	./lib/$(MIMALLOC_LIB)

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
WINDOWS_GLEW_OPTS += LDFLAGS.EXTRA="-mcrtdll=ucrt -lucrt -nostdlib"

WINDOWS_SDL2_CONFIG = --host=x86_64-w64-mingw32
WINDOWS_PYTHON_CONFIG = --host=x86_64-w64-mingw32
WINDOWS_PYTHON_DEFS = "-D__USE_MINGW_ANSI_STDIO=1 -D__MINGW32__"
WINDOWS_PYTHON_LDFLAGS = "-mcrtdll=ucrt -lucrt"
WINDOWS_PYTHON_TARGET = libpython2.7.dll
WINDOWS_OPENAL_OPTS = \
	-DCMAKE_TOOLCHAIN_FILE=XCompile.txt \
	-DHOST=x86_64-w64-mingw32 \
	-DALSOFT_UTILS=OFF \
	-DALSOFT_EXAMPLES=OFF \
	-DCMAKE_SHARED_LINKER_FLAGS="-mcrtdll=ucrt -lucrt"
WINDOWS_MIMALLOC_OPTS = \
	-DCMAKE_SYSTEM_NAME=Windows \
	-DCMAKE_SYSTEM_PROCESSOR=x86_64 \
	-DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
	-DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
	-DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
	-DCMAKE_LINKER=x86_64-w64-wingw32-ld \
	-DCMAKE_SHARED_LINKER_FLAGS="-mcrtdll=ucrt -lucrt"

WINDOWS_CC = x86_64-w64-mingw32-gcc
WINDOWS_BIN = ./lib/pf.exe
WINDOWS_LDFLAGS = \
	$(WINDOWS_MIMALLOC_LINKER_FLAG) \
	-mcrtdll=ucrt \
	-lucrt \
	-lSDL2 \
	-lglew32 \
	-llibpython2.7 \
	-lOpenAL32 \
	-lopengl32 \
	-luuid

WINDOWS_DEFS = -DMS_WIN64
WINDOWS_INCLUDE_FLAGS = $(VENDORED_INCLUDE_FLAGS)
WINDOWS_CSTD = c99
WINDOWS_BUILD_READY = 1
WINDOWS_RUN_PREFIX =
WINDOWS_DEPS = \
	./lib/$(GLEW_LIB) \
	./lib/$(SDL2_LIB) \
	./lib/$(PYTHON_LIB) \
	./lib/$(OPENAL_LIB) \
	./lib/$(MIMALLOC_LIB) \
	./lib/mimalloc-redirect.dll

# ------------------------------------------------------------------------------
# macOS
# ------------------------------------------------------------------------------

MACOS_X86_64_PKG_CONFIG = env PKG_CONFIG_LIBDIR=$(HOMEBREW_X86_64_PKGCONFIG_DIRS) pkg-config
MACOS_X86_64_PYTHON_CONFIG = $(shell $(MACOS_HELPER_DIR)/find_python_config.sh MACOS_X86_64)
MACOS_X86_64_CC = /usr/bin/arch -x86_64 clang
MACOS_X86_64_CXX = /usr/bin/arch -x86_64 clang++
MACOS_X86_64_BIN = ./bin/pf
MACOS_X86_64_RUN_PREFIX = /usr/bin/arch -x86_64
MACOS_X86_64_LDFLAGS = \
	$(shell $(MACOS_X86_64_PKG_CONFIG) --libs sdl2 openal 2>/dev/null) \
	$(shell if test -n "$(MACOS_X86_64_PYTHON_CONFIG)"; then $(MACOS_X86_64_PYTHON_CONFIG) --ldflags 2>/dev/null; fi) \
	-L$(HOMEBREW_X86_64_PREFIX)/lib \
	-lmimalloc \
	-Wl,-rpath,$(HOMEBREW_X86_64_PREFIX)/lib
MACOS_X86_64_DEFS = -D_DEFAULT_SOURCE -DGL_SILENCE_DEPRECATION
MACOS_X86_64_INCLUDE_FLAGS = \
	$(shell $(MACOS_X86_64_PKG_CONFIG) --cflags sdl2 openal 2>/dev/null) \
	$(shell if test -n "$(MACOS_X86_64_PYTHON_CONFIG)"; then $(MACOS_X86_64_PYTHON_CONFIG) --includes 2>/dev/null; fi) \
	-I$(HOMEBREW_X86_64_PREFIX)/include
MACOS_X86_64_CSTD = c99
MACOS_X86_64_BUILD_READY = 1
MACOS_X86_64_DEPS =
MACOS_X86_64_DEPS_CMD = $(MACOS_HELPER_DIR)/check_deps.sh MACOS_X86_64

MACOS_ARM64_PKG_CONFIG = env PKG_CONFIG_LIBDIR=$(HOMEBREW_ARM64_PKGCONFIG_DIRS) pkg-config
MACOS_ARM64_PYTHON_CONFIG = $(shell $(MACOS_HELPER_DIR)/find_python_config.sh MACOS_ARM64)
MACOS_ARM64_CC = clang
MACOS_ARM64_CXX = clang++
MACOS_ARM64_BIN = ./bin/pf-arm64
MACOS_ARM64_RUN_PREFIX =
MACOS_ARM64_LDFLAGS = \
	$(shell $(MACOS_ARM64_PKG_CONFIG) --libs sdl2 openal 2>/dev/null) \
	$(shell if test -n "$(MACOS_ARM64_PYTHON_CONFIG)"; then $(MACOS_ARM64_PYTHON_CONFIG) --ldflags --embed 2>/dev/null || $(MACOS_ARM64_PYTHON_CONFIG) --ldflags 2>/dev/null; fi) \
	-L$(HOMEBREW_ARM64_PREFIX)/lib \
	-lmimalloc \
	-Wl,-rpath,$(HOMEBREW_ARM64_PREFIX)/lib
MACOS_ARM64_DEFS = -D_DEFAULT_SOURCE -DGL_SILENCE_DEPRECATION
MACOS_ARM64_INCLUDE_FLAGS = \
	$(shell $(MACOS_ARM64_PKG_CONFIG) --cflags sdl2 openal 2>/dev/null) \
	$(shell if test -n "$(MACOS_ARM64_PYTHON_CONFIG)"; then $(MACOS_ARM64_PYTHON_CONFIG) --includes 2>/dev/null; fi) \
	-I$(HOMEBREW_ARM64_PREFIX)/include
MACOS_ARM64_CSTD = gnu11
MACOS_ARM64_BUILD_READY = 0
MACOS_ARM64_DEPS =
MACOS_ARM64_DEPS_CMD = $(MACOS_HELPER_DIR)/check_deps.sh MACOS_ARM64

# ------------------------------------------------------------------------------
# Platform-Agnostic
# ------------------------------------------------------------------------------

CC = $($(PLAT)_CC)
CXX = $(if $($(PLAT)_CXX),$($(PLAT)_CXX),clang++)
BIN = $($(PLAT)_BIN)
PLAT_LDFLAGS = $($(PLAT)_LDFLAGS)
DEFS = $($(PLAT)_DEFS) -DGIT_VERSION=\"$(GIT_VERSION)\"
INCLUDE_FLAGS = $($(PLAT)_INCLUDE_FLAGS)
CSTD = $($(PLAT)_CSTD)
RUN_PREFIX = $($(PLAT)_RUN_PREFIX)
DEPS = $($(PLAT)_DEPS)
PLAT_DEPS_CMD = $($(PLAT)_DEPS_CMD)
BUILD_READY = $($(PLAT)_BUILD_READY)

GLEW_LIB = $($(PLAT)_GLEW_LIB)
SDL2_LIB = $($(PLAT)_SDL2_LIB)
PYTHON_LIB = $($(PLAT)_PYTHON_LIB)
OPENAL_LIB = $($(PLAT)_OPENAL_LIB)
MIMALLOC_LIB = $($(PLAT)_MIMALLOC_LIB)

SDL2_CONFIG = $($(PLAT)_SDL2_CONFIG)
PYTHON_CONFIG = $($(PLAT)_PYTHON_CONFIG)
PYTHON_DEFS = $($(PLAT)_PYTHON_DEFS)
PYTHON_LDFLAGS = $($(PLAT)_PYTHON_LDFLAGS)
PYTHON_TARGET = $($(PLAT)_PYTHON_TARGET)
GLEW_OPTS = $($(PLAT)_GLEW_OPTS)
OPENAL_OPTS = $($(PLAT)_OPENAL_OPTS)

BACKEND_OPENGL_DEF = 0
BACKEND_METAL_DEF = 0
BACKEND_PLAT_LDFLAGS =

ifeq ($(RENDER_BACKEND),OPENGL)
BACKEND_OPENGL_DEF = 1
ifeq ($(PLAT),MACOS_X86_64)
BACKEND_PLAT_LDFLAGS = -framework OpenGL
endif
ifeq ($(PLAT),MACOS_ARM64)
BACKEND_PLAT_LDFLAGS = -framework OpenGL
endif
endif

ifeq ($(RENDER_BACKEND),METAL)
BACKEND_METAL_DEF = 1
ifeq ($(PLAT),MACOS_X86_64)
BACKEND_PLAT_LDFLAGS = -framework Metal -framework QuartzCore -framework Foundation -Wl,-dead_strip
endif
ifeq ($(PLAT),MACOS_ARM64)
BACKEND_PLAT_LDFLAGS = -framework Metal -framework QuartzCore -framework Foundation -Wl,-dead_strip
endif
endif

DEFS += \
	-DPF_RENDER_BACKEND_OPENGL=$(BACKEND_OPENGL_DEF) \
	-DPF_RENDER_BACKEND_METAL=$(BACKEND_METAL_DEF)

MIMALLOC_DEBUG_OPTS = -DCMAKE_BUILD_TYPE=Debug
MIMALLOC_RELEASE_OPTS = -DCMAKE_BUILD_TYPE=Release
MIMALLOC_OPTS = $(MIMALLOC_$(TYPE)_OPTS) $($(PLAT)_MIMALLOC_OPTS) -DCMAKE_C_FLAGS=-I../include

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
	$(INCLUDE_FLAGS) \
	-std=$(CSTD) \
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
	$(BACKEND_PLAT_LDFLAGS) \
	$(PLAT_LDFLAGS)

OBJCFLAGS = \
	$(INCLUDE_FLAGS) \
	-x objective-c \
	-fno-strict-aliasing \
	-fwrapv \
	-fobjc-arc \
	$(WARNING_FLAGS) \
	$(LTO_CFLAGS) \
	$(EXTRA_FLAGS)

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------

ifneq ($(strip $(GLEW_LIB)),)
./lib/$(GLEW_LIB):
	mkdir -p ./lib
	make -C $(GLEW_SRC) extensions 
	make -C $(GLEW_SRC) $(GLEW_OPTS) glew.lib.shared
	cp $(GLEW_SRC)/lib/$(GLEW_LIB) $@
endif

ifneq ($(strip $(SDL2_LIB)),)
./lib/$(SDL2_LIB):
	mkdir -p ./lib
	mkdir -p $(SDL2_SRC)/build
	cd $(SDL2_SRC)/build \
		&& ../configure $(SDL2_CONFIG) \
		&& make
	cp $(SDL2_SRC)/build/build/.libs/$(SDL2_LIB) $@
endif

ifneq ($(strip $(PYTHON_LIB)),)
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
	&& make $(PYTHON_TARGET) CFLAGS=$(PYTHON_DEFS) LDFLAGS=$(PYTHON_LDFLAGS)
	cp $(PYTHON_SRC)/build/$(PYTHON_TARGET) $@
endif

ifneq ($(strip $(OPENAL_LIB)),)
./lib/$(OPENAL_LIB):
	mkdir -p $(OPENAL_SRC)/build
ifeq ($(OS),Windows_NT)
	ln -sf $(shell which windres) $(OPENAL_SRC)/build/x86_64-w64-mingw32-windres
endif
	cd $(OPENAL_SRC)/build \
		&& export PATH="$(shell pwd)/$(OPENAL_SRC)/build:$$PATH" \
		&& cmake .. $(OPENAL_OPTS) \
		-DCMAKE_CXX_FLAGS="-I. -I.. -I../include -I../alc -I../common" \
		-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
		&& make 
	cp $(OPENAL_SRC)/build/$(OPENAL_LIB) $@
endif

ifneq ($(strip $(MIMALLOC_LIB)),)
./lib/$(MIMALLOC_LIB):
	mkdir -p $(MIMALLOC_SRC)/build
	cd $(MIMALLOC_SRC)/build \
		&& export CFLAGS="-DERROR_COMMITMENT_MINIMUM=0x0000027B" \
		&& cmake .. $(MIMALLOC_OPTS) \
		&& make
	cp $(MIMALLOC_SRC)/build/$(MIMALLOC_LIB) $@
endif

./lib/mimalloc-redirect.dll: ./lib/$(MIMALLOC_LIB)
	cp $(MIMALLOC_SRC)/bin/$(notdir $@) $@

deps: $(DEPS)
ifneq ($(strip $(PLAT_DEPS_CMD)),)
	@$(PLAT_DEPS_CMD)
endif

guard_build_ready:
ifeq ($(BUILD_READY),0)
	@printf "%s\n" "$(PLAT) $(RENDER_BACKEND) build support is planned but not implemented yet. See ./macos_port_plan.md for the current status."
	@false
endif

$(BACKEND_STAMP):
	@mkdir -p $(OBJ_DIR)
	@touch $@

$(PF_SRC_OBJS): $(OBJ_DIR)/%.o: ./src/%.c $(BACKEND_STAMP)
	@mkdir -p $(dir $@)
	@printf "%-8s %s\n" "[CC]" $@
	@$(CC) -MT $@ -MMD -MP -MF $(OBJ_DIR)/$*.d $(CFLAGS) $(DEFS) -c $< -o $@

$(PF_ASM_OBJS): $(OBJ_DIR)/%.o: ./src/%.S $(BACKEND_STAMP)
	@mkdir -p $(dir $@)
	@printf "%-8s %s\n" "[AS]" $@
	@$(CC) $(CFLAGS) $(DEFS) -c $< -o $@

$(PF_OBJC_OBJS): $(OBJ_DIR)/%.o: ./src/%.m $(BACKEND_STAMP)
	@mkdir -p $(dir $@)
	@printf "%-8s %s\n" "[OBJC]" $@
	@$(CC) -MT $@ -MMD -MP -MF $(OBJ_DIR)/$*.d $(OBJCFLAGS) $(DEFS) -c $< -o $@

$(BIN): .FORCE $(DEPS) $(PF_OBJS)
	@mkdir -p ./bin
	@printf "%-8s %s\n" "[LD]" $@
	@$(CC) $(filter-out .FORCE,$^) -o $(BIN) $(LDFLAGS)
ifeq ($(OS),Windows_NT)
	@./deps/mimalloc/bin/minject.exe -f $@
	@mv ./lib/pf-mi.exe $@
endif

./obj/version.o: .FORCE
.FORCE:

-include $(PF_DEPS)

.PHONY: deps guard_build_ready pf clean run run_hfmp run_editor editor_app run_editor_app clean_deps launchers .FORCE

pf: guard_build_ready $(BIN)

clean_deps:
	cd deps/GLEW && git clean -f -d
	cd deps/SDL2 && git clean -f -d
	cd deps/Python && git clean -f -d
	cd deps/openal-soft && git clean -f -d
	cd deps/mimalloc && git clean -f -d
	rm -rf deps/SDL2/build
	rm -rf deps/Python/build
	rm -rf deps/openal-soft/build
	rm -rf deps/mimalloc/build
	rm -rf ./lib/*

clean:
	rm -rf $(PF_OBJS) $(PF_DEPS) $(BIN) 

run: pf
ifneq ($(PLAT),MACOS_ARM64)
	@$(RUN_PREFIX) $(BIN) ./ ./scripts/rts/main.py
else
	@$(RUN_PREFIX) $(BIN) ./ ./scripts/rts/main.py
endif

run_hfmp: pf
	@$(RUN_PREFIX) $(BIN) ./ ./scripts/hfmp_s2/main.py

run_editor:
ifeq ($(PLAT),MACOS_X86_64)
	@printf "%s\n" "run_editor is not supported on macOS during the current bring-up phase."
	@false
else
	@$(BIN) ./ ./scripts/editor/main.py
endif

editor_app:
ifeq ($(PLAT),MACOS_ARM64)
	@scripts/macos/build_editor_app_bundle.sh --backend $(RENDER_BACKEND)
else
	@printf "%s\n" "editor_app is only supported for PLAT=MACOS_ARM64."
	@false
endif

run_editor_app:
ifeq ($(PLAT),MACOS_ARM64)
	@scripts/macos/build_editor_app_bundle.sh --backend $(RENDER_BACKEND) --launch
else
	@printf "%s\n" "run_editor_app is only supported for PLAT=MACOS_ARM64."
	@false
endif

launchers:
ifeq ($(PLAT),WINDOWS)
	make -C launcher BIN_PATH='.\\\\lib\\\\pf.exe' SCRIPT_PATH="./scripts/rts/main.py" BIN="../demo.exe" launcher
	make -C launcher BIN_PATH='.\\\\lib\\\\pf.exe' SCRIPT_PATH="./scripts/editor/main.py" BIN="../editor.exe" launcher
else
	make -C launcher BIN_PATH=$(BIN) SCRIPT_PATH="./scripts/rts/main.py" BIN="../demo" launcher
	make -C launcher BIN_PATH=$(BIN) SCRIPT_PATH="./scripts/editor/main.py" BIN="../editor" launcher
endif
