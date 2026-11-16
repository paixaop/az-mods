#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
inc=${1:-../../az-mods-sdk/include}
passes=${2:-plain asan tsan}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
for pass in $passes; do
    case "$pass" in
        plain) flags='-O2' ;;
        asan) flags='-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all' ;;
        tsan) flags='-g -O1 -fsanitize=thread' ;;
        *) echo "unknown sanitizer pass: $pass" >&2; exit 2 ;;
    esac
    echo "== $pass"
    # Intentional word splitting for the compiler's separate flags.
    ${CC:-clang} -std=c11 -Wall -Wextra -Werror -fno-omit-frame-pointer $flags \
        -I"$inc" tests/test_beatport_remember.c -o "$build/test-$pass"
    "$build/test-$pass"
done
