CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -Iinclude -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lm

BUILD := build
LIB_SRC := src/rng.c src/genome.c src/world.c src/policy.c src/env.c \
           src/aqua_genome.c src/aqua.c src/aqua_env.c \
           src/land_genome.c src/land.c src/land_env.c \
           src/civ.c src/civ_env.c
# The editor session lives here and not in LIB_SRC: it drives the studio,
# so it belongs to the half of the project a training build drops.
VIS_SRC := src/render.c src/render_cell.c src/render3d.c src/render_land.c \
           src/render_terra.c src/render_civ.c src/land_edit.c src/png.c
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

# ---- WebAssembly ----
#
# No Emscripten. clang has had a wasm32 target for years and wasm-ld ships
# with lld, so the only thing missing is a C runtime - and the parts of one
# this program actually uses are an allocator, five memory functions and the
# transcendentals. wasm/shim.c is the first two; the third are left undefined
# so the linker turns them into imports and the browser's own Math supplies
# them. Taking on a toolchain that brings its own libc, in order to prove the
# project does not need one, would have been a strange trade.
WASM_SRC := src/rng.c src/genome.c src/land_genome.c src/land.c \
            src/land_edit.c src/render_terra.c wasm/shim.c
WASM_OBJ := $(WASM_SRC:%.c=$(BUILD)/wasm/%.o)
WASM_CF  := --target=wasm32 -O2 -std=c99 -nostdlib -ffunction-sections \
            -fdata-sections -Wall -Wextra -Wno-unused-parameter \
            -Iwasm/include -Iinclude -D_POSIX_C_SOURCE=200809L
WASM_EXPORTS := $(shell grep -oE '\bcp4_edit_[a-z_]+' include/cpore/land.h \
                        | sort -u | sed 's/^/--export=/')

$(BUILD)/wasm/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	clang $(WASM_CF) -c $< -o $@

wasm/cpore.wasm: $(WASM_OBJ)
	wasm-ld --no-entry --gc-sections --import-undefined \
	  --export=cp_wasm_alloc --export=cp_wasm_free --export=__heap_base \
	  --initial-memory=33554432 --max-memory=536870912 \
	  $(WASM_EXPORTS) -o $@ $(WASM_OBJ)
	@ls -l $@ | awk '{print "  " $$5 " bytes"}'

.PHONY: wasm serve
wasm: wasm/cpore.wasm
# the page is ES modules and fetches the .wasm, so it needs an origin
serve: wasm ; @echo "http://127.0.0.1:8731/editor.html" && cd wasm && python3 -m http.server 8731

clean: ; rm -rf $(BUILD) wasm/cpore.wasm
