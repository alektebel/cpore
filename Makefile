# spore — Creature Creator (SDL2 + OpenGL)
#
# One binary: the Lochner-style creature creator (Build / Paint / Test). SDL2
# for the window and input, epoxy for the GL 3.3 core calls, libm for the
# maths. No engine, no asset pipeline, no Emscripten.
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -Iinclude $(shell pkg-config --cflags sdl2)
LDFLAGS ?=
LIBS    := $(shell pkg-config --libs sdl2) -lepoxy -lm -lGL

CREATOR_SRCS := src/creator_main.c src/bodymesh.c src/creature.c \
                src/genome.c src/desc.c src/terrain.c
CREATOR_OBJS := $(CREATOR_SRCS:.c=.o)

.PHONY: all clean run

all: creator

creator: $(CREATOR_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(CREATOR_OBJS) $(LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: creator
	./creator

clean:
	rm -f $(CREATOR_OBJS) creator
