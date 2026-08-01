CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lm

BUILD := build
LIB_SRC := src/rng.c src/genome.c src/world.c src/policy.c src/env.c
VIS_SRC := src/render.c src/png.c
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
VIS_OBJ := $(VIS_SRC:%.c=$(BUILD)/%.o)

.PHONY: all clean test bench shot shots lib
all: $(BUILD)/cpore_shot $(BUILD)/cpore_bench $(BUILD)/cpore_test $(BUILD)/libcpore.so

$(BUILD)/%.o: %.c include/cpore/cpore.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libcpore.a: $(LIB_OBJ) $(VIS_OBJ)
	ar rcs $@ $^

# shared object is what the python/ctypes binding loads
$(BUILD)/libcpore.so: $(LIB_OBJ) $(VIS_OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

$(BUILD)/cpore_shot: apps/shot.c $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_bench: apps/bench.c $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_test: tests/test_core.c $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

lib: $(BUILD)/libcpore.so
test: $(BUILD)/cpore_test ; @./$(BUILD)/cpore_test
bench: $(BUILD)/cpore_bench ; @./$(BUILD)/cpore_bench
shot: $(BUILD)/cpore_shot ; @./$(BUILD)/cpore_shot --seed 7 --steps 900 --out $(BUILD)/shot.png

clean: ; rm -rf $(BUILD)
