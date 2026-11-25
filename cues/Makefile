# SPDX-License-Identifier: GPL-3.0-only
MOD_ID := cues
SRCS := cues.c config.c settings.c store.c plan.c
AZ_SDK ?= ../../az-mods-sdk
AZ_SDK_INCLUDE ?= $(AZ_SDK)/include
OUT ?= out
SAN ?= plain asan tsan
TEST_CC ?= clang
UNIT_SAN_FLAGS ?=
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror=implicit-function-declaration \
          -fPIC -fvisibility=hidden -fno-omit-frame-pointer -fno-optimize-sibling-calls \
          -fexceptions -fasynchronous-unwind-tables -D_GNU_SOURCE -I$(AZ_SDK_INCLUDE)
LDFLAGS := -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu
LDLIBS := -lm -lpthread
NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' | $(CC) -mno-outline-atomics -x c -o /dev/null - 2>/dev/null && echo -mno-outline-atomics)
CFLAGS += $(NO_OUTLINE_ATOMICS)
HEADERS := cues.h store.h $(addprefix $(AZ_SDK_INCLUDE)/,azmod-mod.h azmod-data-helpers.h azmod-timestamps.h azmod-phrases.h)
# The standalone SDK build remains usable. Release builds with provider
# support mount the companion SDK and the pinned cMCP source.
ifneq ($(MCP_PROVIDER_DIR),)
CMCP ?= /deps/cmcp
SRCS += mcp.c $(addprefix $(MCP_PROVIDER_DIR)/,provider.c client-wire.c wire.c) $(CMCP)/src/json.c
CFLAGS += -DCUES_MCP -I$(MCP_PROVIDER_DIR) -I$(CMCP)/include
HEADERS += $(addprefix $(MCP_PROVIDER_DIR)/,az-mcp-provider.h client-wire.h wire.h)
endif
HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I. -I$(AZ_SDK_INCLUDE) $(UNIT_SAN_FLAGS)
.PHONY: all unit test clean
all: $(OUT)/$(MOD_ID).so
$(OUT):
	mkdir -p $@
$(OUT)/$(MOD_ID).so: $(SRCS) $(HEADERS) | $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LDLIBS)
$(OUT)/test_plan: tests/test_plan.c plan.c config.c $(HEADERS) | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -o $@ tests/test_plan.c plan.c config.c -lm
$(OUT)/test_runtime: tests/test_runtime.c cues.c plan.c config.c $(HEADERS) | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -o $@ tests/test_runtime.c plan.c config.c -lm
$(OUT)/test_config: tests/test_config.c config.c $(HEADERS) | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -o $@ tests/test_config.c config.c -lm
$(OUT)/test_store: tests/test_store.c store.c config.c $(HEADERS) | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -o $@ tests/test_store.c config.c -lm
$(OUT)/test_settings: tests/test_settings.c settings.c config.c $(HEADERS) | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -o $@ tests/test_settings.c config.c -lm
$(OUT)/render_settings: tools/render_settings.c settings.c config.c $(HEADERS) | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -o $@ tools/render_settings.c config.c -lm
$(OUT)/render_deck: tools/render_deck.c cues.c $(HEADERS) | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -o $@ tools/render_deck.c -lm
unit: $(OUT)/test_plan $(OUT)/test_runtime $(OUT)/test_config $(OUT)/test_store $(OUT)/test_settings
	./$(OUT)/test_plan
	./$(OUT)/test_runtime
	./$(OUT)/test_config
	./$(OUT)/test_store
	./$(OUT)/test_settings
test:
	CC=$(TEST_CC) sh $(AZ_SDK)/tools/harness/sanitize.sh --passes "$(SAN)" \
	    $(MOD_ID):4:cues.c,config.c,settings.c,store.c,plan.c
clean:
	rm -rf $(OUT)

.PHONY: mcp-unit
mcp-unit: | $(OUT)
	$(TEST_CC) $(HOST_FLAGS) -DCUES_MCP -o $(OUT)/test_mcp_runtime tests/test_runtime.c plan.c config.c -lm
	$(OUT)/test_mcp_runtime
	$(TEST_CC) $(HOST_FLAGS) -DCUES_MCP -I$(MCP_PROVIDER_DIR) -I$(CMCP)/include \
		-o $(OUT)/test_cues_mcp tests/test_mcp.c config.c $(MCP_PROVIDER_DIR)/wire.c $(CMCP)/src/json.c -lm
	$(OUT)/test_cues_mcp
	$(TEST_CC) $(HOST_FLAGS) -o $(OUT)/test_mcp_settings tests/test_mcp_settings.c config.c -lm
	$(OUT)/test_mcp_settings
