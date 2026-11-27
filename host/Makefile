# Linux debug host (docker image mew-dev:1). From the repo root:
#   make -C host -j16            -> build/host/mewhost
ROOT   := ..
GEN    ?= $(ROOT)/gen
OUT    ?= $(ROOT)/build/host
CC     := clang
SDL3   ?= $(ROOT)/build/sdl3-host/install

GFLAGS := -O1 -w -fwrapv -fno-strict-aliasing -ffp-contract=off -g0 \
          -fno-unwind-tables -fno-asynchronous-unwind-tables \
          -I$(ROOT)/runtime/include -I$(GEN)
RFLAGS := -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -fno-strict-aliasing \
          -ffp-contract=off -I$(ROOT)/runtime/include -I$(ROOT)/runtime/src -I$(GEN) -I$(SDL3)/include

GEN_SRC := $(sort $(wildcard $(GEN)/recomp_funcs_*.c)) $(GEN)/recomp_dispatch.c $(GEN)/recomp_imports.c
GEN_OBJ := $(patsubst $(GEN)/%.c,$(OUT)/gen/%.o,$(GEN_SRC))
RT_SRC  := $(sort $(wildcard $(ROOT)/runtime/src/*.c)) $(ROOT)/runtime/include/eh64.c $(ROOT)/host/main.c
RT_OBJ  := $(patsubst $(ROOT)/%.c,$(OUT)/rt/%.o,$(RT_SRC))

all: $(OUT)/mewhost

gen: $(OUT)/libgen.a

$(OUT)/gen/%.o: $(GEN)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(GFLAGS) -c $< -o $@

$(OUT)/libgen.a: $(GEN_OBJ)
	rm -f $@; ar rcs $@ $^

$(OUT)/rt/%.o: $(ROOT)/%.c $(wildcard $(ROOT)/runtime/src/*.h) $(ROOT)/runtime/include/cpu64.h
	@mkdir -p $(dir $@)
	$(CC) $(RFLAGS) -c $< -o $@

$(OUT)/mewhost: $(RT_OBJ) $(OUT)/libgen.a
	$(CC) -o $@ $(RT_OBJ) $(OUT)/libgen.a -L$(SDL3)/lib -Wl,-rpath,$(abspath $(SDL3)/lib) -lSDL3 -lGL -lpthread -ldl -lm

clean:
	rm -rf $(OUT)
.PHONY: all gen clean
