#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
task_root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
task_sdk=${SWIFT_SDKS_PATH:-"$HOME/Library/org.swift.swiftpm/swift-sdks"}
task_image=${SWIFT_IMAGE:-swift:6.4.2-jammy@sha256:e86bcb3d1aa6e2bcd54251bad4b82b1f9d825757081c058840f7483c0d164744}
test -d "$task_sdk" || { echo 'Set SWIFT_SDKS_PATH to the directory containing Swift 6.4.2 Static Linux SDK.' >&2; exit 1; }
# The SDK's mimalloc archive uses ARMv8.1 LSE atomics. The deck's Cortex-A53/A72
# cores are ARMv8.0: rebuild the same allocator version and link its object first.
# Do not alter the installed SDK or substitute a newer emulator CPU.
task_allocator="$task_root/out/allocator"
task_archive="$task_allocator/mimalloc-2.2.4.tar.gz"
mkdir -p "$task_allocator"
if [ ! -f "$task_archive" ]; then
    curl -fL --retry 3 https://codeload.github.com/microsoft/mimalloc/tar.gz/refs/tags/v2.2.4 -o "$task_archive.tmp"
    mv "$task_archive.tmp" "$task_archive"
fi
task_digest=$(shasum -a 256 "$task_archive" | cut -d ' ' -f 1)
test "$task_digest" = 754a98de5e2912fddbeaf24830f982b4540992f1bab4a0a8796ee118e0752bda || {
    echo "mimalloc archive checksum mismatch: $task_archive" >&2; exit 1;
}
tar -xzf "$task_archive" -C "$task_allocator"
docker run --rm --platform linux/arm64 -v "$task_root:/mod" -v "$task_sdk:/sdk:ro" \
    "$task_image" sh -eu -c '
    task_sysroot=/sdk/swift-6.4.2-RELEASE_static-linux-0.1.0.artifactbundle/swift-6.4.2-RELEASE_static-linux-0.1.0/swift-linux-musl/musl-1.2.5.sdk/aarch64
    clang --target=aarch64-linux-musl --sysroot="$task_sysroot" -march=armv8-a -mno-outline-atomics \
        -O2 -DNDEBUG -DMI_STATIC_LIB -DMI_MALLOC_OVERRIDE -I/mod/out/allocator/mimalloc-2.2.4/include \
        -c /mod/out/allocator/mimalloc-2.2.4/src/static.c -o /mod/out/allocator/mimalloc.o'
docker run --rm --platform linux/arm64 -v "$task_root:/mod" -v "$task_sdk:/sdk:ro" -w /mod/worker \
    "$task_image" swift build --scratch-path /mod/out/worker -c release --swift-sdks-path /sdk --swift-sdk aarch64-swift-linux-musl \
    -Xswiftc -target-cpu -Xswiftc cortex-a53 -Xcc -march=armv8-a -Xlinker /mod/out/allocator/mimalloc.o
task_binary=$(docker run --rm --platform linux/arm64 -v "$task_root:/mod" -v "$task_sdk:/sdk:ro" -w /mod/worker \
    "$task_image" swift build --scratch-path /mod/out/worker -c release --swift-sdks-path /sdk --swift-sdk aarch64-swift-linux-musl --show-bin-path)
task_binary="$task_root${task_binary#/mod}"
cp "$task_binary/track-analysis-worker" "$task_root/out/track-analysis-worker"
docker run --rm --platform linux/arm64 -v "$task_root:/mod" azmod-build:18.04 \
    strip --strip-unneeded /mod/out/track-analysis-worker
mkdir -p "$task_root/assets"
gzip -n -9 -c "$task_root/out/track-analysis-worker" > "$task_root/assets/track-analysis-worker.gz"
cp -R "$task_root/worker/Sources/Analysis/Resources/NativeBeatModels" "$task_root/assets/"
cp "$task_allocator/mimalloc-2.2.4/LICENSE" "$task_root/assets/MIMALLOC-LICENSE.txt"
cp "$task_root/worker/Sources/FAAD/COPYING" "$task_root/assets/FAAD2-LICENSE.txt"
cp "$task_root/worker/Sources/PortableDSP/MINIMP4-LICENSE" "$task_root/assets/MINIMP4-LICENSE.txt"
