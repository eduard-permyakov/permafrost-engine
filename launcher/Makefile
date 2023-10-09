ifeq ($(PLAT),WINDOWS)
CROSS_COMPILE = x86_64-w64-mingw32-
else
CROSS_COMPILE = 
endif

.PHONY: clean launcher

clean:
	rm -rf obj

ifeq ($(PLAT),WINDOWS)

obj/res.o: pf.rc
	mkdir -p obj
ifeq ($(OS),Windows_NT)
	$(shell which windres) $^ -o $@
else
	$(CROSS_COMPILE)windres $^ -o $@
endif

launcher: obj/res.o
	mkdir -p obj
	$(CROSS_COMPILE)gcc main.c obj/res.o -DBIN_PATH=$(BIN_PATH) -DSCRIPT_PATH=$(SCRIPT_PATH) -Wl,-subsystem,windows,-ldbghelp -o $(BIN)

else #PLAT != WINDOWS

launcher:
	mkdir -p obj
	$(CROSS_COMPILE)gcc main.c -DBIN_PATH=$(BIN_PATH) -DSCRIPT_PATH=$(SCRIPT_PATH) -o $(BIN)

endif

