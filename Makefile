CC		= gcc
ifeq ($(OS),Windows_NT)
BIN		= ./lib/pf.exe #EXE must be in the same directory as shared libs
else
BIN		= ./bin/pf
endif

PF_DIRS = $(sort $(dir $(wildcard ./src/*/)))
PF_SRCS = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)/*.c)) 
PF_OBJS = $(PF_SRCS:./src/%.c=./obj/%.o)
PF_DEPS = $(PF_OBJS:%.o=%.d)

GLEW_SRC = ./deps/glew-2.1.0
ifeq ($(OS),Windows_NT)
GLEW_LIB = glew32.dll
else
GLEW_LIB = libGLEW.so.2.1
endif
GLEW_VER = 2.1.0

SDL2_SRC = ./deps/SDL2-2.0.7
ifeq ($(OS),Windows_NT)
SDL2_LIB = SDL2.dll
else
SDL2_LIB = libSDL2-2.0.so.0
endif
SDL2_VER_MAJOR = 2.0
SDL2_VER_MINOR = 0.7.0

PYTHON_SRC = ./deps/Python-2.7.13
ifeq ($(OS),Windows_NT)
PYTHON_LIB = python27.dll
else
PYTHON_LIB = libpython2.7.so.1.0
endif
PYTHON_VER_MAJOR = 2.7

CFLAGS  = -std=c99 -I$(GLEW_SRC)/include -I$(SDL2_SRC)/include -I$(PYTHON_SRC)/Include -I$(PYTHON_SRC)/build \
		   -fno-strict-aliasing -march=native -O2 -pipe -fwrapv -g
DEFS  	=
LDFLAGS = -L./lib/ -lm -lpthread -lm
ifeq ($(OS),Windows_NT)
LDFLAGS += -lmingw32 -lSDL2 -lglew32 -lpython27 -lopengl32
else
LDFLAGS += -l:$(SDL2_LIB) -l:$(GLEW_LIB) -l:$(PYTHON_LIB) -lGL -ldl -lutil -Xlinker -export-dynamic -Xlinker -rpath='$$ORIGIN/../lib'
endif
DEPS = ./lib/$(GLEW_LIB) ./lib/$(SDL2_LIB) ./lib/$(PYTHON_LIB)

deps: $(DEPS)

./lib/$(GLEW_LIB): 
	mkdir -p ./lib
	make -C $(GLEW_SRC) glew.lib.shared
ifeq ($(OS),Windows_NT)
	cp $(GLEW_SRC)/lib/$(GLEW_LIB) $@
else
	cp $(GLEW_SRC)/lib/libGLEW.so.$(GLEW_VER) $@
endif

./lib/$(SDL2_LIB):
	mkdir -p ./lib
	mkdir -p $(SDL2_SRC)/build
	cd $(SDL2_SRC)/build \
		&& ../configure \
		&& make
ifeq ($(OS),Windows_NT)
	cp $(SDL2_SRC)/build/build/.libs/$(SDL2_LIB) $@
else
	cp $(SDL2_SRC)/build/build/.libs/libSDL2-$(SDL2_VER_MAJOR).so.$(SDL2_VER_MINOR) $@
endif

./lib/$(PYTHON_LIB):
ifeq ($(OS),Windows_NT)
	$(error "Python must be built using MSVC build tools.")
else
	mkdir -p ./lib/pyinstall/lib
	mkdir -p $(PYTHON_SRC)/build
	cd $(PYTHON_SRC)/build \
		&& ../configure --enable-shared --enable-optimizations \
		   --prefix=$(shell pwd)/lib/pyinstall \
		&& make \
		&& make install	
	mv ./lib/pyinstall/lib/$(PYTHON_LIB) $@
	mv ./lib/pyinstall/lib/python$(PYTHON_VER_MAJOR) ./lib/.
	rm -rf ./lib/pyinstall
endif

./obj/%.o: ./src/%.c
	mkdir -p $(dir $@)
	$(CC) -MT $@ -MMD -MP -MF ./obj/$*.d $(CFLAGS) $(DEFS) -c $< -o $@

pf: $(PF_OBJS)
	mkdir -p ./bin
	$(CC) $? -o $(BIN) $(LDFLAGS)

-include $(PF_DEPS)

.PHONY: clean run clean_deps

.IGNORE: clean_deps

clean_deps:
	rm -rf ./lib/*
	cd $(GLEW_SRC) && make clean
	cd $(SDL2_SRC)/build && make clean
	cd $(PYTHON_SRC)/build && make clean

clean:
	rm -rf $(PF_OBJS) $(PF_DEPS) $(BIN) 

run:
	@./bin/pf ./ ./scripts/demo/main.py

run_editor:
	@./bin/pf ./ ./scripts/editor/main.py

