CC		= gcc
BIN		= ./bin/pf

PF_DIRS = $(sort $(dir $(wildcard ./src/*/)))
PF_SRCS = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)/*.c)) 
PF_OBJS = $(PF_SRCS:./src/%.c=./obj/%.o)
PF_DEPS = $(PF_OBJS:%.o=%.d)

GLEW_SRC = ./deps/glew-2.1.0
GLEW_LIB = libGLEW.so.2.1
GLEW_VER = 2.1.0

SDL2_SRC = ./deps/SDL2-2.0.7
SDL2_LIB = libSDL2-2.0.so.0
SDL2_VER_MAJOR = 2.0
SDL2_VER_MINOR = 0.7.0

PYTHON_SRC = ./deps/Python-2.7.13
PYTHON_LIB = libpython2.7.so.1.0
PYTHON_VER_MAJOR = 2.7

CFLAGS  = -std=c99 -I$(GLEW_SRC)/include -I$(SDL2_SRC)/include -I$(PYTHON_SRC)/Include -I$(PYTHON_SRC)/build \
		   -fno-strict-aliasing -march=native -O2 -pipe -fwrapv -DNDEBUG -g
DEFS  	=
LDFLAGS = -L./lib/ -lGL -lm -lpthread -ldl -lutil -lm -Xlinker -export-dynamic -Xlinker -rpath='$$ORIGIN/../lib'

DEPS = ./lib/$(GLEW_LIB) ./lib/$(SDL2_LIB) ./lib/$(PYTHON_LIB)

./lib/$(GLEW_LIB): 
	mkdir -p ./lib
	make -C $(GLEW_SRC) glew.lib.shared
	cp $(GLEW_SRC)/lib/libGLEW.so.$(GLEW_VER) $@

./lib/$(SDL2_LIB):
	mkdir -p ./lib
	mkdir -p $(SDL2_SRC)/build
	cd $(SDL2_SRC)/build \
		&& ../configure \
		&& make
	cp $(SDL2_SRC)/build/build/.libs/libSDL2-$(SDL2_VER_MAJOR).so.$(SDL2_VER_MINOR) $@

./lib/$(PYTHON_LIB):
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

./obj/%.o: ./src/%.c
	mkdir -p $(dir $@)
	$(CC) -MT $@ -MMD -MP -MF ./obj/$*.d $(CFLAGS) $(DEFS) -c $< -o $@

pf: $(PF_OBJS) $(DEPS)
	mkdir -p ./bin
	$(CC) $? -o $(BIN) $(LDFLAGS)

-include $(PF_DEPS)

.PHONY: clean run clean_deps

clean_deps:
	rm -rf ./lib/*
	cd $(GLEW_SRC) && make clean
	cd $(SDL2_SRC)/build && make clean
	cd $(PYTHON_SRC)/build  && make clean

clean:
	rm -rf $(PF_OBJS) $(PF_DEPS) $(BIN) 

run:
	./bin/pf ./

