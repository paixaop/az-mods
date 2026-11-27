# SPDX-License-Identifier: GPL-3.0-only
MOD_ID := youtube
AZ_SDK ?= ../../az-mods-sdk
AZ_SDK_INCLUDE ?= $(AZ_SDK)/include
OUT ?= out
DIST ?= dist
TEST_CC ?= clang
SAN_FLAGS ?=
HELPER_BUNDLE ?= ../../temp/ytcui-dl-deck/ytcui-dl-xdj-az-arm64.tar.gz
YTCUI_SOURCE ?= ../../temp/ytcui-dl-deck/ytcui-dl-0.6.1
YTCUI_DL ?= $(OUT)/ytcui-dl
VENDOR := vendor/cmcp/json.c
COMMON := model.c files.c $(VENDOR)
MOD_SOURCES := mod.c job.c $(COMMON)
HEADERS := model.h files.h job.h source-icon.h $(AZ_SDK_INCLUDE)/azmod-mod.h vendor/cmcp/cmcp_json.h
HELPER_SOURCES := helper.c usb-publish.c $(COMMON)
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -I$(AZ_SDK_INCLUDE) -I. -Ivendor/cmcp
WARN := -std=c11 -Wall -Wextra -Werror
ARM_FLAGS := -O2 -fPIC -fvisibility=hidden -fno-omit-frame-pointer -fno-optimize-sibling-calls \
             -fexceptions -fasynchronous-unwind-tables
ARM_LINK := -shared -static-libgcc -Wl,--no-undefined -Wl,--hash-style=gnu -Wl,--strip-all
LDLIBS := -lm -pthread
NO_OUTLINE_ATOMICS := $(shell printf 'int main(void){return 0;}\n' | $(CC) -mno-outline-atomics -x c -o /dev/null 2>/dev/null && echo -mno-outline-atomics)

.PHONY: all unit unit-helper downloader stage package
all: $(OUT)/youtube.so $(OUT)/youtube-helper

$(OUT):
	mkdir -p "$@"

$(OUT)/youtube.so: $(MOD_SOURCES) $(HEADERS) Makefile | $(OUT)
	$(CC) $(CPPFLAGS) $(WARN) $(ARM_FLAGS) $(NO_OUTLINE_ATOMICS) $(ARM_LINK) -o "$@" $(MOD_SOURCES) $(LDLIBS)

$(OUT)/youtube-helper: $(HELPER_SOURCES) usb-publish.h $(HEADERS) Makefile | $(OUT)
	$(CC) $(CPPFLAGS) $(WARN) -Os -s -o "$@" $(HELPER_SOURCES) $(LDLIBS)

$(OUT)/test_youtube: tests/test_youtube.c $(MOD_SOURCES) $(HEADERS) | $(OUT)
	$(TEST_CC) $(CPPFLAGS) $(WARN) -O1 -g $(SAN_FLAGS) -o "$@" tests/test_youtube.c $(MOD_SOURCES) $(LDLIBS)

unit: $(OUT)/test_youtube
	"$(OUT)/test_youtube"

$(OUT)/helper-test/youtube-helper: $(HELPER_SOURCES) usb-publish.h $(HEADERS) | $(OUT)
	mkdir -p "$(OUT)/helper-test"
	$(TEST_CC) $(CPPFLAGS) $(WARN) -O1 -g $(SAN_FLAGS) -o "$@" $(HELPER_SOURCES) $(LDLIBS)

$(OUT)/helper-test/libexec/ytcui-dl: tests/fake_helper.c | $(OUT)
	mkdir -p "$(OUT)/helper-test/libexec"
	$(TEST_CC) $(CPPFLAGS) $(WARN) -O1 -g $(SAN_FLAGS) -o "$@" tests/fake_helper.c
	ln -sf ytcui-dl "$(OUT)/helper-test/libexec/ffmpeg"

$(OUT)/helper-test/test_helper: tests/test_helper.c | $(OUT)
	mkdir -p "$(OUT)/helper-test"
	$(TEST_CC) $(CPPFLAGS) $(WARN) -O1 -g $(SAN_FLAGS) -o "$@" tests/test_helper.c

$(OUT)/helper-test/test_usb_publish: tests/test_usb_publish.c usb-publish.c usb-publish.h | $(OUT)
	mkdir -p "$(OUT)/helper-test"
	$(TEST_CC) $(CPPFLAGS) $(WARN) -O1 -g $(SAN_FLAGS) -DYT_WRAP_LINK -Wl,--wrap=syscall -o "$@" tests/test_usb_publish.c usb-publish.c

unit-helper: $(OUT)/helper-test/youtube-helper $(OUT)/helper-test/libexec/ytcui-dl $(OUT)/helper-test/test_helper $(OUT)/helper-test/test_usb_publish
	"$(OUT)/helper-test/test_helper" "$(OUT)/helper-test/youtube-helper" "$(OUT)/helper-test"
	"$(OUT)/helper-test/test_usb_publish" "$(OUT)/helper-test"

downloader: | $(OUT)
	$(CC) $(WARN) -Os -DYT_WRAP_LINK -c usb-publish.c -o "$(OUT)/usb-publish.o"
	cmake -S "$(YTCUI_SOURCE)" -B "$(OUT)/ytcui-build" -G Ninja \
		-DCMAKE_BUILD_TYPE=MinSizeRel -DOPENSSL_USE_STATIC_LIBS=TRUE -DZLIB_USE_STATIC_LIBS=ON \
		-DYTDL_WERROR=ON -DCMAKE_EXE_LINKER_FLAGS="-static -Wl,--wrap=link $(abspath $(OUT))/usb-publish.o"
	cmake --build "$(OUT)/ytcui-build" --target ytcui-dl
	strip -o "$(YTCUI_DL)" "$(OUT)/ytcui-build/ytcui-dl"

stage: $(OUT)/youtube.so $(OUT)/youtube-helper
	mkdir -p "$(OUT)/package/assets/libexec" "$(OUT)/package/assets/licenses"
	cp azmod.json icon.png LICENSE "$(OUT)/youtube.so" "$(OUT)/package/"
	cp "$(OUT)/youtube-helper" "$(OUT)/package/assets/"
	cp "$(YTCUI_DL)" "$(OUT)/package/assets/libexec/ytcui-dl"
	tar -xf "$(HELPER_BUNDLE)" -C "$(OUT)/package/assets" libexec/ffmpeg LICENSE.ytcui-dl LICENSE.FFmpeg
	mv "$(OUT)/package/assets/LICENSE.ytcui-dl" "$(OUT)/package/assets/LICENSE.FFmpeg" "$(OUT)/package/assets/licenses/"
	cp vendor/cmcp/LICENSE "$(OUT)/package/assets/licenses/cMCP.txt"

package: stage
	$(AZ_SDK)/tools/azmod-pack package "$(OUT)/package" --out "$(DIST)"
