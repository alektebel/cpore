# spore — Creature Stage prototype (SDL2 + OpenGL)
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -Iinclude $(shell pkg-config --cflags sdl2)
LDFLAGS ?=
LIBS    := $(shell pkg-config --libs sdl2) -lepoxy -lm -lGL

SRCS := src/main.c src/render.c src/terrain.c src/creature.c src/world.c src/genome.c src/save.c src/desc.c src/bodymesh.c
OBJS := $(SRCS:.c=.o)
CORE := src/world.c src/creature.c src/terrain.c src/genome.c src/save.c src/desc.c src/bodymesh.c

CREATOR_SRCS := src/creator_main.c src/bodymesh.c src/creature.c src/genome.c src/desc.c src/terrain.c
CREATOR_OBJS := $(CREATOR_SRCS:.c=.o)

.PHONY: all clean run test creator run-creator

all: spore creator

spore: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

creator: $(CREATOR_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(CREATOR_OBJS) $(LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: spore
	./spore

run-creator: creator
	./creator

test:
	$(CC) -std=c11 -Wall -Wextra -O2 -Iinclude tests/smoke_logic.c $(CORE) -lm -o /tmp/spore_smoke
	/tmp/spore_smoke

clean:
	rm -f $(OBJS) $(CREATOR_OBJS) spore creator
