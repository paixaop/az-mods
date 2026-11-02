# SPDX-License-Identifier: GPL-3.0-only
# anchor: build the mod inside the SDK container.
#
#   ../../sdk/tools/azmod-pack build .   builds in the container (recommended)
#   make                              only inside the container (or another aarch64
#                                     Ubuntu 18.04 toolchain); azmod-pack runs this
#   make unit                         the host tests (parser + timing maths), no deck needed
#
# The flags are the rules from the SDK's docs/GUIDELINES.md in Makefile form.

MOD_ID          := anchor
SRCS            := anchor.c anlz.c resolve.c guide.c loaded.c settings.c display.c
AZ_SDK_INCLUDE  ?= ../../sdk/include
AZ_SDK          ?= ../../sdk
MAX_REDRAWS     ?= 6
SAN             ?= plain asan tsan
TEST_CC         ?= clang
TEST_CXX        ?= clang++
OUT             ?= out
DEBUG           ?= 0
UNIT_SAN_FLAGS  ?=

ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS  := -O2 -Wall -Wextra -Werror=implicit-function-declaration \
           -fPIC -fvisibility=hidden -fno-omit-frame-pointer \
           -fexceptions -fasynchronous-unwind-tables \
           -D_GNU_SOURCE -I$(AZ_SDK_INCLUDE)
LDFLAGS := -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu
LDLIBS  := -lpthread -lm

NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' | \
    $(CC) -mno-outline-atomics -x c -o /dev/null - 2>/dev/null && echo -mno-outline-atomics)
CFLAGS += $(NO_OUTLINE_ATOMICS)
ifeq ($(strip $(DEBUG)),1)
CFLAGS += -g
endif

.PHONY: all clean unit ui-test test previews
all: $(OUT)/$(MOD_ID).so

$(OUT)/$(MOD_ID).so: $(SRCS) anlz.h resolve.h guide.h loaded.h settings.h display.h mod.h $(AZ_SDK_INCLUDE)/azmod-mod.h | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(OUT):
	mkdir -p $(OUT)

# Host tests. They use the same parser and maths sources as the deck build, with a small
# synthetic ANLZ writer, so they run on the development machine.
unit: tests/test_b_target.c tests/test_anchor.c tests/test_display.c tests/test_lifecycle.c tests/test_runtime.c tests/test_loaded.c tests/test_resolve.c $(SRCS) anlz.h guide.h resolve.h display.h loaded.h settings.h mod.h
	$(CC) -O1 -g -Wall -Wextra $(UNIT_SAN_FLAGS) -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_anchor tests/test_anchor.c anlz.c guide.c resolve.c -lm
	./tests/test_anchor
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_display tests/test_display.c display.c -lm
	./tests/test_display
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_lifecycle tests/test_lifecycle.c $(filter-out anchor.c,$(SRCS)) -lpthread -lm
	./tests/test_lifecycle
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_runtime tests/test_runtime.c $(filter-out anchor.c,$(SRCS)) -lpthread -lm
	./tests/test_runtime
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_b_target tests/test_b_target.c $(filter-out anchor.c,$(SRCS)) -lpthread -lm
	./tests/test_b_target
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_loaded tests/test_loaded.c anlz.c -lm
	./tests/test_loaded
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_resolve tests/test_resolve.c anlz.c -lm
	./tests/test_resolve

$(OUT)/render_ui: tools/render_strip.c $(SRCS) display.h mod.h guide.h settings.h | $(OUT)
	$(TEST_CC) -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o $@ tools/render_strip.c $(filter-out anchor.c,$(SRCS)) -lpthread -lm

$(OUT)/test_settings: tests/test_settings.c settings.c settings.h mod.h | $(OUT)
	$(TEST_CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -I. -I$(AZ_SDK_INCLUDE) -o $@ tests/test_settings.c

$(OUT)/render_settings: tools/render_settings.c settings.c settings.h mod.h | $(OUT)
	$(TEST_CC) -O1 -g -Wall -Wextra -Werror -I. -I$(AZ_SDK_INCLUDE) -o $@ tools/render_settings.c settings.c

ui-test: $(OUT)/render_ui $(OUT)/test_settings
	./$(OUT)/render_ui /dev/null verify
	./$(OUT)/test_settings

test:
	@test -f $(AZ_SDK)/tools/harness/sanitize.sh || \
	    { echo "make test: set AZ_SDK to your az-mods-sdk checkout" >&2; exit 2; }
	CC=$(TEST_CC) CXX=$(TEST_CXX) sh $(AZ_SDK)/tools/harness/sanitize.sh --passes "$(SAN)" \
	    $(MOD_ID):$(MAX_REDRAWS):anchor.c,anlz.c,resolve.c,guide.c,loaded.c,settings.c,display.c

previews:
	python3 tools/preview_displays.py
	python3 tools/preview_settings.py

clean:
	rm -rf $(OUT) tests/test_anchor tests/test_display tests/test_lifecycle tests/test_runtime tests/test_b_target tests/test_loaded tests/test_resolve
