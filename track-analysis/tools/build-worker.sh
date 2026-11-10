#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
task_root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
task_sdk=${SWIFT_SDKS_PATH:-"$HOME/Library/org.swift.swiftpm/swift-sdks"}
task_image=${SWIFT_IMAGE:-swift:6.4.2-jammy@sha256:e86bcb3d1aa6e2bcd54251bad4b82b1f9d825757081c058840f7483c0d164744}
test -d "$task_sdk" || { echo 'Set SWIFT_SDKS_PATH to the directory containing Swift 6.4.2 Static Linux SDK.' >&2; exit 1; }
docker run --rm --platform linux/arm64 -v "$task_root:/mod" -v "$task_sdk:/sdk:ro" -w /mod/worker \
    "$task_image" swift build --scratch-path /mod/out/worker -c release --swift-sdks-path /sdk --swift-sdk aarch64-swift-linux-musl
task_binary=$(docker run --rm --platform linux/arm64 -v "$task_root:/mod" -v "$task_sdk:/sdk:ro" -w /mod/worker \
    "$task_image" swift build --scratch-path /mod/out/worker -c release --swift-sdks-path /sdk --swift-sdk aarch64-swift-linux-musl --show-bin-path)
task_binary="$task_root${task_binary#/mod}"
cp "$task_binary/track-analysis-worker" "$task_root/out/track-analysis-worker"
docker run --rm --platform linux/arm64 -v "$task_root:/mod" azmod-build:18.04 \
    strip --strip-unneeded /mod/out/track-analysis-worker
mkdir -p "$task_root/assets"
gzip -n -9 -c "$task_root/out/track-analysis-worker" > "$task_root/assets/track-analysis-worker.gz"
cp -R "$task_root/worker/Sources/Analysis/Resources/NativeBeatModels" "$task_root/assets/"
