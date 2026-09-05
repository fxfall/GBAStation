#ifndef ROMX_PATH_INTERNAL_H
#define ROMX_PATH_INTERNAL_H

#include "json_internal.h"
#include <string.h>

static inline int romx_ascii_fold_compare(const char *left, const char *right)
{
    for (;;) {
        unsigned char a = (unsigned char)*left++;
        unsigned char b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32U);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32U);
        if (a != b) return a < b ? -1 : 1;
        if (a == 0U) return 0;
    }
}

static inline int romx_ascii_fold_equal(const char *left, const char *right)
{
    return romx_ascii_fold_compare(left, right) == 0;
}

/* Validate the full wire length, including embedded NUL bytes. */
static inline int romx_path_bytes_valid(const uint8_t *path, size_t size)
{
    size_t component = 0U, index, bad = 0U;
    if (path == NULL || size == 0U || path[0] == '/' ||
        path[size - 1U] == '/' || !romx_utf8_validate(path, size, &bad))
        return 0;
    for (index = 0U; index <= size; ++index) {
        if (index < size && path[index] != '/') {
            if (path[index] == 0U || path[index] == '\\') return 0;
            continue;
        }
        if (index == component ||
            (index - component == 1U && path[component] == '.') ||
            (index - component == 2U && path[component] == '.' &&
                path[component + 1U] == '.')) return 0;
        component = index + 1U;
    }
    return 1;
}

static inline int romx_path_valid(const char *path, size_t capacity)
{
    size_t size;
    if (path == NULL) return 0;
    size = strlen(path);
    return size <= capacity &&
        romx_path_bytes_valid((const uint8_t *)path, size);
}

static inline int romx_hex_string(const char *value, size_t expected_size)
{
    size_t index;
    if (value == NULL || strlen(value) != expected_size) return 0;
    for (index = 0U; index < expected_size; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'A' && byte <= 'F') ||
              (byte >= 'a' && byte <= 'f'))) return 0;
    }
    return 1;
}

static inline void romx_copy_hex_upper(const char *value, size_t size,
    char *output)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        unsigned char byte = (unsigned char)value[index];
        if (byte >= 'a' && byte <= 'f')
            byte = (unsigned char)(byte - 'a' + 'A');
        output[index] = (char)byte;
    }
    output[size] = '\0';
}

#endif
