#ifndef ROMX_IO_POSIX_INTERNAL_H
#define ROMX_IO_POSIX_INTERNAL_H

#include <limits.h>
#include <sys/types.h>
#include <unistd.h>

static inline size_t romx_posix_io_count(uint64_t remaining)
{
#if defined(SSIZE_MAX)
    const size_t maximum = (size_t)SSIZE_MAX;
#else
    const size_t maximum = SIZE_MAX >> 1U;
#endif
    return remaining > (uint64_t)maximum ? maximum : (size_t)remaining;
}

/* Without pread/pwrite the caller must serialize operations on each handle. */
static inline ssize_t romx_posix_pread(int fd, void *buffer, size_t size,
    off_t offset)
{
#if defined(ROMX_NO_PREAD)
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return (ssize_t)-1;
    return read(fd, buffer, size);
#else
    return pread(fd, buffer, size, offset);
#endif
}

static inline ssize_t romx_posix_pwrite(int fd, const void *buffer, size_t size,
    off_t offset)
{
#if defined(ROMX_NO_PREAD)
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) return (ssize_t)-1;
    return write(fd, buffer, size);
#else
    return pwrite(fd, buffer, size, offset);
#endif
}

#endif
