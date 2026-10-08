# Usage:  make ROM=path/to/game.gb         (recompile + build)
#         make run ROM=path/to/game.gb
#         make discover ROM=...            (find code the static pass missed, rebuild)
ROM        ?= roms/game.gb
GEN        ?= generated
BUILD      ?= build
CC         ?= cc
CFLAGS     ?= -O2 -Wall
PYTHON     ?= python3
ROOTS      ?= $(GEN)/roots.txt
RECOMP_ARGS ?=

SDL_CFLAGS       ?= $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS         ?= $(shell sdl2-config --libs 2>/dev/null)
SDL_IMAGE_CFLAGS ?= $(shell pkg-config --cflags SDL2_image 2>/dev/null)
SDL_IMAGE_LIBS   ?= $(shell pkg-config --libs SDL2_image 2>/dev/null)
ifneq ($(strip $(SDL_LIBS)),)
  DEFS += -DUSE_SDL
endif

RT_SRC := $(wildcard runtime/*.c)
GEN_SRC := $(GEN)/game.c $(GEN)/interp.c

all: $(BUILD)/PipeClean

$(GEN)/game.c $(GEN)/interp.c $(GEN)/game_info.h: $(ROM) tools/recomp.py tools/sm83.py $(wildcard $(ROOTS))
	@bytes=$$(wc -c < "$(ROM)"); \
	if [ $$bytes -gt 32768 ]; then \
	  echo "$(ROM): banked ROM ($$bytes bytes), generating interpreter-only build"; \
	  $(PYTHON) tools/recomp.py $(ROM) -o $(GEN) --listing --interp-only $(if $(wildcard $(ROOTS)),--roots $(ROOTS),) $(RECOMP_ARGS); \
	else \
	  $(PYTHON) tools/recomp.py $(ROM) -o $(GEN) --listing $(if $(wildcard $(ROOTS)),--roots $(ROOTS),) $(RECOMP_ARGS); \
	fi

$(BUILD)/PipeClean: $(RT_SRC) $(wildcard runtime/*.h) $(GEN_SRC) $(GEN)/game_info.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(SDL_CFLAGS) $(SDL_IMAGE_CFLAGS) -Iruntime -I$(GEN) $(RT_SRC) $(GEN_SRC) -o $@ $(SDL_LIBS) $(SDL_IMAGE_LIBS) -lm $(EXTRA_LIBS)

run: $(BUILD)/PipeClean
	$(BUILD)/PipeClean $(ROM)

discover:
	$(PYTHON) tools/discover.py $(ROM) --gen $(GEN) --build "make ROM=$(ROM) GEN=$(GEN) BUILD=$(BUILD)"

clean:
	rm -rf $(BUILD) $(GEN)

.PHONY: all run discover clean test-sml2-wide test-sml-wide test-cart-mbc1

test-sml2-wide:
	$(CC) $(CFLAGS) -Iruntime tests/test_sml2_wide.c runtime/widescreen.c -o /tmp/test_sml2_wide
	/tmp/test_sml2_wide

test-sml-wide:
	$(CC) $(CFLAGS) -Iruntime tests/test_sml_wide.c runtime/widescreen.c -o /tmp/test_sml_wide
	/tmp/test_sml_wide

test-cart-mbc1:
	$(CC) $(CFLAGS) -Iruntime tests/test_cart_mbc1.c runtime/cart.c runtime/patch.c -o /tmp/test_cart_mbc1
	/tmp/test_cart_mbc1
