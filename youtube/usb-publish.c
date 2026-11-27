/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "usb-publish.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int unsupported(int error)
{
    return error == EINVAL || error == ENOSYS || error == EOPNOTSUPP;
}

static int reserved_rename(const char *from, const char *to)
{
    /* FUSE exFAT can lack both hard links and flagged rename. Reserve the name
     * exclusively, then replace that reservation with the complete file. */
    int fd = open(to, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    int result = rename(from, to);
    int saved = errno;
    if (result < 0)
        (void)unlink(to);
    (void)close(fd);
    errno = saved;
    return result;
}

int yt_publish_usb(const char *from, const char *to)
{
    int result = (int)syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE);
    if (result < 0 && unsupported(errno))
        return reserved_rename(from, to);
    return result;
}

#ifdef YT_WRAP_LINK
int __real_link(const char *from, const char *to);

static int is_download_stage(const char *from, const char *to)
{
    size_t end = strlen(to);
    return strncmp(from, to, end) == 0 && strcmp(from + end, ".part") == 0;
}

int __wrap_link(const char *from, const char *to)
{
    int result = __real_link(from, to);
    if (result < 0 && (errno == EPERM || unsupported(errno)) && is_download_stage(from, to))
        return yt_publish_usb(from, to);
    return result;
}
#endif
