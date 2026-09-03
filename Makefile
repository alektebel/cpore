CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lm

BUILD := build
LIB_SRC := src/rng.c src/genome.c src/world.c src/policy.c src/env.c \
           src/aqua_genome.c src/aqua.c src/aqua_env.c \
           src/land_genome.c src/land.c src/land_env.c \
           src/civ.c src/civ_env.c \
           src/tribe.c src/vec.c src/genome_codec.c src/codex.c
VIS_SRC := src/render.c src/render3d.c src/render_land.c src/render_civ.c src/png.c
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD)/%.o)
VIS_OBJ := $(VIS_SRC:%.c=$(BUILD)/%.o)

.PHONY: all clean test bench shot aqua land civ tribe play game lib wasm web
all: $(BUILD)/cpore_shot $(BUILD)/cpore_aqua $(BUILD)/cpore_land \
     $(BUILD)/cpore_civ $(BUILD)/cpore_tribe $(BUILD)/cpore_play $(BUILD)/cpore_game \
     $(BUILD)/cpore_bench $(BUILD)/cpore_test $(BUILD)/libcpore.so

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

$(BUILD)/cpore_tribe: apps/tribe_shot.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

$(BUILD)/cpore_play: apps/play.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) $< $(BUILD)/libcpore.a -o $@ $(LDLIBS)

# ---- the native game: X11 + GLX + fixed-function GL present path ----
# glview.c is NOT part of libcpore: the sim core stays libc+libm only, and
# only the game shell links the GPU/windowing system. Needs the stock system
# headers (X11/Xlib.h, GL/glx.h) - no SDL, no extra packages.
$(BUILD)/cpore_game: apps/cpore_game.c src/glview.c $(HDRS) $(BUILD)/libcpore.a
	$(CC) $(CFLAGS) apps/cpore_game.c src/glview.c $(BUILD)/libcpore.a -o $@ $(LDLIBS) -lGL -lX11

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
tribe: $(BUILD)/cpore_tribe ; @./$(BUILD)/cpore_tribe --seed 5 --out $(BUILD)/tribe.png
play: $(BUILD)/cpore_play ; @./$(BUILD)/cpore_play --seed 23
game: $(BUILD)/cpore_game ; @./$(BUILD)/cpore_game --seed 7 --stage land
game-tribe: $(BUILD)/cpore_game ; @./$(BUILD)/cpore_game --seed 7 --stage tribe

# ---- the browser editor ----
# Needs emscripten on PATH (source ~/emsdk/emsdk_env.sh). Single-threaded on
# purpose: threads would need COOP/COEP headers and the whole point of this
# build is that it is a static directory anyone can host.
EMCC ?= emcc
web/cpore.js: apps/web.c $(LIB_SRC) $(VIS_SRC) $(HDRS)
	$(EMCC) -O3 -std=c99 -Iinclude apps/web.c $(LIB_SRC) $(VIS_SRC) -o $@ \
	  -sMODULARIZE=1 -sEXPORT_NAME=Cpore -sALLOW_MEMORY_GROWTH=1 \
	  -sENVIRONMENT=web -lm \
	  -sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,HEAPF32

wasm: web/cpore.js
web: wasm ; @cd web && python3 -m http.server 8123

clean: ; rm -rf $(BUILD) web/cpore.js web/cpore.wasm
