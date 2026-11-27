# SPDX-License-Identifier: GPL-3.0-only
# anchor: build the mod inside the SDK container.
#
#   ../../az-mods-sdk/tools/azmod-pack build .   builds in the SDK container
#   make                              only inside the container (or another aarch64
#                                     Ubuntu 18.04 toolchain); azmod-pack runs this
#   make unit                         host tests for deck events and timing
#
# The flags are the rules from the SDK's docs/GUIDELINES.md in Makefile form.

MOD_ID          := anchor
SRCS            := anchor.c deck.c guide.c settings.c display.c
AZ_SDK          ?= ../../az-mods-sdk
AZ_SDK_INCLUDE  ?= $(AZ_SDK)/include
HEADERS         := deck.h guide.h settings.h display.h mod.h \
                   $(addprefix $(AZ_SDK_INCLUDE)/,azmod-mod.h azmod-data-helpers.h azmod-timestamps.h azmod-phrases.h)
MAX_REDRAWS     ?= 6
SAN             ?= plain asan tsan
TEST_CC         ?= clang
OUT             ?= out
DEBUG           ?= 0
UNIT_SAN_FLAGS  ?=

ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
           -fPIC -fvisibility=hidden -fno-omit-frame-pointer -fno-optimize-sibling-calls \
           -fexceptions -fasynchronous-unwind-tables \
           -D_GNU_SOURCE -I$(AZ_SDK_INCLUDE)
LDFLAGS := -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu
LDLIBS  := -lm

NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' | \
    $(CC) -mno-outline-atomics -x c -o /dev/null - 2>/dev/null && echo -mno-outline-atomics)
CFLAGS += $(NO_OUTLINE_ATOMICS)
ifneq ($(MCP_PROVIDER_DIR),)
CMCP ?= /deps/cmcp
SRCS += $(addprefix $(MCP_PROVIDER_DIR)/,provider.c client-wire.c wire.c) $(CMCP)/src/json.c
CFLAGS += -DANCHOR_MCP -I$(MCP_PROVIDER_DIR) -I$(CMCP)/include
HEADERS += mcp-tools.c
LDLIBS += -pthread
endif
ifeq ($(strip $(DEBUG)),1)
CFLAGS += -g
else
LDFLAGS += -Wl,--strip-all
endif

.PHONY: all clean unit ui-test test previews
all: $(OUT)/$(MOD_ID).so

$(OUT)/$(MOD_ID).so: $(SRCS) $(HEADERS) Makefile | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(OUT):
	mkdir -p $(OUT)

unit: tests/test_anchor.c tests/test_display.c tests/test_lifecycle.c $(SRCS) $(HEADERS)
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_anchor tests/test_anchor.c guide.c -lm
	./tests/test_anchor
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_display tests/test_display.c display.c -lm
	./tests/test_display
	$(CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o tests/test_lifecycle tests/test_lifecycle.c $(filter-out anchor.c,$(SRCS)) -lm
	./tests/test_lifecycle

$(OUT)/render_ui: tools/render_strip.c $(SRCS) $(HEADERS) | $(OUT)
	$(TEST_CC) -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) \
	    -o $@ tools/render_strip.c $(filter-out anchor.c,$(SRCS)) -lm

$(OUT)/test_settings: tests/test_settings.c settings.c $(HEADERS) | $(OUT)
	$(TEST_CC) -O1 -g -Wall -Wextra -Werror $(UNIT_SAN_FLAGS) -I. -I$(AZ_SDK_INCLUDE) -o $@ tests/test_settings.c

$(OUT)/render_settings: tools/render_settings.c settings.c $(HEADERS) | $(OUT)
	$(TEST_CC) -O1 -g -Wall -Wextra -Werror -I. -I$(AZ_SDK_INCLUDE) -o $@ tools/render_settings.c settings.c

ui-test: $(OUT)/render_ui $(OUT)/test_settings
	"$(OUT)/render_ui" /dev/null verify
	"$(OUT)/test_settings"

test:
	@test -f $(AZ_SDK)/tools/harness/sanitize.sh || \
	    { echo "make test: set AZ_SDK to your az-mods-sdk checkout" >&2; exit 2; }
	CC=$(TEST_CC) sh $(AZ_SDK)/tools/harness/sanitize.sh --passes "$(SAN)" \
	    $(MOD_ID):$(MAX_REDRAWS):anchor.c,deck.c,guide.c,settings.c,display.c

previews:
	python3 tools/preview_displays.py
	python3 tools/preview_settings.py

clean:
	rm -rf $(OUT) tests/test_anchor tests/test_display tests/test_lifecycle

.PHONY: mcp-unit
mcp-unit: | $(OUT)
	$(TEST_CC) -std=c11 -D_GNU_SOURCE -O1 -g -I. -I$(AZ_SDK_INCLUDE) $(UNIT_SAN_FLAGS) \
		-I$(MCP_PROVIDER_DIR) -I$(CMCP)/include -o $(OUT)/test_anchor_mcp \
		tests/test_mcp.c deck.c guide.c settings.c display.c $(MCP_PROVIDER_DIR)/wire.c $(CMCP)/src/json.c -lm
	$(OUT)/test_anchor_mcp
