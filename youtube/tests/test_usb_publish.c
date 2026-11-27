/* SPDX-License-Identifier: GPL-3.0-only */
#include "usb-publish.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

static int rename_error;
static int link_error;

long __real_syscall(long number, ...);
long __wrap_syscall(long number, ...)
{
    assert(number == SYS_renameat2);
    if (rename_error) {
        errno = rename_error;
        return -1;
    }
    va_list args;
    va_start(args, number);
    int source_fd = va_arg(args, int);
    const char *source = va_arg(args, const char *);
    int destination_fd = va_arg(args, int);
    const char *destination = va_arg(args, const char *);
    int flags = va_arg(args, int);
    va_end(args);
    return __real_syscall(number, source_fd, source, destination_fd, destination, flags);
}

int __real_link(const char *from, const char *to)
{
    if (link_error) {
        errno = link_error;
        return -1;
    }
    return link(from, to);
}

int __wrap_link(const char *from, const char *to);

static void put(const char *path, char value)
{
    FILE *file = fopen(path, "wx");
    assert(file && fputc(value, file) == value && fclose(file) == 0);
}

static char get(const char *path)
{
    FILE *file = fopen(path, "r");
    assert(file);
    int value = fgetc(file);
    assert(value != EOF && fclose(file) == 0);
    return (char)value;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char directory[PATH_MAX], part[PATH_MAX], output[PATH_MAX], unrelated[PATH_MAX];
    assert(snprintf(directory, sizeof directory, "%s/usb.XXXXXX", argv[1]) < PATH_MAX);
    assert(mkdtemp(directory));
    assert(snprintf(output, sizeof output, "%s/source.m4a", directory) < PATH_MAX);
    assert(snprintf(part, sizeof part, "%s/source.m4a.part", directory) < PATH_MAX);
    assert(snprintf(unrelated, sizeof unrelated, "%s/unrelated", directory) < PATH_MAX);

    put(part, 'a');
    assert(__wrap_link(part, output) == 0 && get(output) == 'a');
    assert(unlink(output) == 0);
    link_error = EPERM;
    assert(__wrap_link(part, unrelated) == -1 && errno == EPERM);
    assert(yt_publish_usb(part, output) == 0 && get(output) == 'a');

    put(part, 'b');
    rename_error = EINVAL;
    assert(__wrap_link(part, output) == -1 && errno == EEXIST && get(output) == 'a');
    assert(unlink(output) == 0);
    assert(__wrap_link(part, output) == 0 && get(output) == 'b' && access(part, F_OK) < 0);

    assert(yt_publish_usb(part, unrelated) == -1 && errno == ENOENT);
    assert(access(unrelated, F_OK) < 0);
    assert(unlink(output) == 0 && rmdir(directory) == 0);
    puts("USB publication preserves existing files and supports unflagged rename");
    return 0;
}
