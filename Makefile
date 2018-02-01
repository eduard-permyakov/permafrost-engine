CC		= gcc
BIN		= ./bin/pf

PF_DIRS = $(sort $(dir $(wildcard ./src/*/)))
PF_SRCS = $(foreach dir,$(PF_DIRS),$(wildcard $(dir)/*.c)) 
PF_OBJS = $(PF_SRCS:./src/%.c=./obj/%.o)
PF_DEPS = $(PF_OBJS:%.o=%.d)

CFLAGS  = -std=c99 -I./deps/glew-2.1.0/include -I./deps/SDL2-2.0.7/include -g $(shell python2.7-config --cflags)
DEFS  	=
LDFLAGS = -L./lib/ -lGL -lm $(shell python2.7-config --ldflags) -Xlinker -rpath='$$ORIGIN/../lib'

./lib/libGLEW.so.2.1: 
	mkdir -p ./lib
	make -C ./deps/glew-2.1.0 glew.lib.shared
	cp ./deps/glew-2.1.0/lib/libGLEW.so.2.1.0 $@

./lib/libSDL2-2.0.so.0:
	mkdir -p ./lib
	mkdir -p ./deps/SDL2-2.0.7/build
	cd ./deps/SDL2-2.0.7/build && ../configure && make
	cp ./deps/SDL2-2.0.7/build/build/.libs/libSDL2-2.0.so.0.7.0 $@

./obj/%.o: ./src/%.c
	mkdir -p $(dir $@)
	$(CC) -MT $@ -MMD -MP -MF ./obj/$*.d $(CFLAGS) $(DEFS) -c $< -o $@

pf: $(PF_OBJS) ./lib/libGLEW.so.2.1 ./lib/libSDL2-2.0.so.0
	mkdir -p ./bin
	$(CC) $? -o $(BIN) $(LDFLAGS)

-include $(PF_DEPS)

.PHONY: clean run

clean:
	rm -rf $(PF_OBJS) $(PF_DEPS) $(BIN) 

run:
	./bin/pf ./

