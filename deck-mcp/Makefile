# SPDX-License-Identifier: GPL-3.0-only
MOD_ID := deck-mcp
AZ_SDK ?= ../../az-mods-sdk
AZ_SDK_INCLUDE ?= $(AZ_SDK)/include
DEPS ?= /deps
CMCP ?= $(DEPS)/cmcp
TLS_PREFIX ?= $(DEPS)/arm
EVENT_PREFIX ?= $(TLS_PREFIX)
HOST_CC ?= clang
OUT ?= out
TEST_FLAGS ?=
TEST_CURL_FLAGS ?= -lcurl

CMCP_CORE := json rpc schema types era sha256 seal http_modern schema_check schema_format xheader \
             http_parser transport_stdio worker mrtr subscriptions
CMCP_SOURCES := $(addprefix $(CMCP)/src/,$(addsuffix .c,$(CMCP_CORE)))
SOURCES := mod.c commands.c command-tools.c cue-tools.c state.c tools.c settings-tools.c data-tools.c analysis-tools.c stored-tools.c library-tools.c live-analysis.c panel-tools.c config.c http.c protocol.c cmcp-adapter.c providers.c provider/wire.c token-screen.c setup.c setup-tls.c
HEADERS := deck-mcp.h commands.h settings-tools.h data-tools.h library-tools.h panel-tools.h http.h providers.h provider/wire.h provider/az-mcp-provider.h \
           $(addprefix $(AZ_SDK_INCLUDE)/,azmod-mod.h azmod-data-helpers.h azmod-timestamps.h)
INCLUDES := -I. -I$(AZ_SDK_INCLUDE) -I$(CMCP)/include -I$(CMCP)/src \
            -I$(EVENT_PREFIX)/include -I$(TLS_PREFIX)/include
LIBRARIES := $(EVENT_PREFIX)/lib/libevent_openssl.a $(EVENT_PREFIX)/lib/libevent.a \
             $(TLS_PREFIX)/lib/libssl.a $(TLS_PREFIX)/lib/libcrypto.a
FLAGS := -std=c11 -Wall -Wextra -Werror=implicit-function-declaration -D_GNU_SOURCE $(INCLUDES)
SOURCES += pairing.c vendor/qrcodegen/qrcodegen.c
HEADERS += vendor/qrcodegen/qrcodegen.h
ARM_FLAGS := -O2 -fPIC -fvisibility=hidden -fno-omit-frame-pointer -fno-optimize-sibling-calls \
             -fexceptions -fasynchronous-unwind-tables -ffunction-sections -fdata-sections
ARM_LINK := -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu \
            -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--version-script=exports.map

.PHONY: all unit
all: $(OUT)/$(MOD_ID).so

$(OUT):
	mkdir -p $@

$(OUT)/$(MOD_ID).so: $(SOURCES) $(HEADERS) $(CMCP_SOURCES) $(CMCP)/src/server.c $(LIBRARIES) exports.map | $(OUT)
	$(CC) $(FLAGS) $(ARM_FLAGS) $(ARM_LINK) -o $@ $(SOURCES) $(CMCP_SOURCES) $(LIBRARIES) -lm -pthread -ldl

$(OUT)/test_https: tests/test_https.c tests/sdk_tools.inc provider/provider.c provider/client-wire.c $(SOURCES) $(HEADERS) $(CMCP_SOURCES) $(CMCP)/src/server.c $(LIBRARIES) | $(OUT)
	$(HOST_CC) $(FLAGS) -O1 -g $(TEST_FLAGS) -o $@ tests/test_https.c provider/provider.c provider/client-wire.c $(SOURCES) $(CMCP_SOURCES) $(LIBRARIES) -lm -pthread $(TEST_CURL_FLAGS)

unit: $(OUT)/test_https
	$(OUT)/test_https "$(FIXTURE)"
