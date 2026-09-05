#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#endif

#include "romx_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

romx_result_t romx_sync_parent_directory(const char *utf8_path,
    romx_error_t *error)
{
#if defined(_WIN32) || defined(ROMX_NO_DIRECTORY_SYNC)
    /* MoveFileEx(..., MOVEFILE_WRITE_THROUGH) and FlushFileBuffers on the
     * temporary file are the strongest portable Win32 guarantees available
     * without opening a volume-specific directory handle. Platforms without
     * directory descriptors still sync file contents at each data commit. */
    if (utf8_path == NULL || *utf8_path == '\0')
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "publish path must not be empty");
    romx_error_clear(error);
    return ROMX_OK;
#else
    const char *separator;
    const char *parent_name = ".";
    size_t parent_size = 1U;
    char *parent = NULL;
    int descriptor;
    int result;

    if (utf8_path == NULL || *utf8_path == '\0')
        return romx_error_set(error, ROMX_E_INVALID_ARGUMENT, 0,
            ROMX_OFFSET_UNKNOWN, "publish path must not be empty");
    separator = strrchr(utf8_path, '/');
    if (separator != NULL) {
        if (separator == utf8_path) {
            parent_name = "/";
            parent_size = 1U;
        } else {
            parent_name = utf8_path;
            parent_size = (size_t)(separator - utf8_path);
        }
    }
    parent = (char *)malloc(parent_size + 1U);
    if (parent == NULL)
        return romx_error_set(error, ROMX_E_OUT_OF_MEMORY, 0,
            ROMX_OFFSET_UNKNOWN, "failed to allocate publish parent path");
    memcpy(parent, parent_name, parent_size);
    parent[parent_size] = '\0';
#if defined(O_DIRECTORY)
    descriptor = open(parent, O_RDONLY | O_DIRECTORY);
#else
    descriptor = open(parent, O_RDONLY);
#endif
    if (descriptor < 0) {
        int code = errno;
        free(parent);
        return romx_error_set(error, ROMX_E_WRITE, code,
            ROMX_OFFSET_UNKNOWN, "failed to open publish parent directory");
    }
    result = fsync(descriptor);
    if (result != 0) {
        int code = errno;
        (void)close(descriptor);
        free(parent);
        return romx_error_set(error, ROMX_E_WRITE, code,
            ROMX_OFFSET_UNKNOWN, "failed to sync publish parent directory");
    }
    if (close(descriptor) != 0) {
        int code = errno;
        free(parent);
        return romx_error_set(error, ROMX_E_WRITE, code,
            ROMX_OFFSET_UNKNOWN, "failed to close publish parent directory");
    }
    free(parent);
    romx_error_clear(error);
    return ROMX_OK;
#endif
}
