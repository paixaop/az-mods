# SPDX-License-Identifier: GPL-3.0-only
MOD_ID := set-planner
SRCS := mod.c menu.c review.c dashboard.c chart.c arc-edit.c library.c energy.c cache.c job.c planner.c
AZ_SDK ?= ../../az-mods-sdk
AZ_SDK_INCLUDE ?= $(AZ_SDK)/include
OUT ?= out
TEST_CC ?= clang
SAN_FLAGS ?=
HEADERS := mod.h planner.h ui.h $(addprefix $(AZ_SDK_INCLUDE)/,azmod-mod.h azmod-phrases.h azmod-timestamps.h)
ifeq ($(origin CC),default)
CC := gcc
endif
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -I$(AZ_SDK_INCLUDE)
CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden \
          -fno-omit-frame-pointer -fno-optimize-sibling-calls -fexceptions -fasynchronous-unwind-tables
LDFLAGS := -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu -Wl,--strip-all
LDLIBS := -lm
NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' | $(CC) -mno-outline-atomics -x c -o /dev/null 2>/dev/null && echo -mno-outline-atomics)
HOST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -I. $(SAN_FLAGS)

.PHONY: all unit
all: $(OUT)/$(MOD_ID).so

$(OUT):
	mkdir -p "$@"

$(OUT)/$(MOD_ID).so: $(SRCS) $(HEADERS) Makefile | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(NO_OUTLINE_ATOMICS) $(LDFLAGS) -o "$@" $(SRCS) $(LDLIBS)

$(OUT)/test-planner: tests/test_planner.c $(SRCS) $(HEADERS) | $(OUT)
	$(TEST_CC) $(CPPFLAGS) $(HOST_FLAGS) -o "$@" tests/test_planner.c $(SRCS) $(LDLIBS) -pthread

unit: $(OUT)/test-planner
	TMPDIR="$(abspath $(OUT))" "$(OUT)/test-planner"
