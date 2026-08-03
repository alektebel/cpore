CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lm

BUILD := build
LIB_SRC := src/rng.c src/genome.c src/world.c src/policy.c src/env.c \
           src/aqua_genome.c src/aqua.c src/aqua_env.c \
           src/land_genome.c src/land.c src/land_env.c \
           src/civ.c src/civ_env.c
VIS_SRC := src/render.c src/render3d.c src/render_land.c src/render_civ.c src/png.c
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
VIS_OBJ := $(VIS_SRC:%.c=$(BUILD)/%.o)

.PHONY: all clean test bench shot aqua land civ lib
all: $(BUILD)/cpore_shot $(BUILD)/cpore_aqua $(BUILD)/cpore_land \
     $(BUILD)/cpore_civ $(BUILD)/cpore_bench $(BUILD)/cpore_test $(BUILD)/libcpore.so

HDRS := $(wildcard include/cpore/*.h) $(wildcard src/*.h)

$(BUILD)/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libcpore.a: $(LIB_OBJ) $(VIS_OBJ)
	ar rcs $@ $^

# shared object is what the python/ctypes binding loads
$(BUILD)/libcpore.so: $(LIB_OBJ) $(VIS_OBJ)
	$(CC) -shared -o $@ $^ $(LDLIBS)

$(BUILD)/cpore_shot: apps/shot.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_aqua: apps/aqua_shot.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_land: apps/land_shot.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_civ: apps/civ_shot.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_bench: apps/bench.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_test: tests/test_core.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

lib: $(BUILD)/libcpore.so
test: $(BUILD)/cpore_test ; @./$(BUILD)/cpore_test
bench: $(BUILD)/cpore_bench ; @./$(BUILD)/cpore_bench
shot: $(BUILD)/cpore_shot ; @./$(BUILD)/cpore_shot --seed 7 --steps 900 --out $(BUILD)/shot.png
aqua: $(BUILD)/cpore_aqua ; @./$(BUILD)/cpore_aqua --seed 3 --steps 2400 --out $(BUILD)/aqua.png
land: $(BUILD)/cpore_land ; @./$(BUILD)/cpore_land --seed 5 --steps 2400 --out $(BUILD)/land.png
civ: $(BUILD)/cpore_civ ; @./$(BUILD)/cpore_civ --seed 4 --out $(BUILD)/civ.png

clean: ; rm -rf $(BUILD)
