#ifndef ROMX_SAVE_INTERNAL_H
#define ROMX_SAVE_INTERNAL_H

#include "path_internal.h"

/* The host scanner and RMBL reader must agree on SaveDataFiler identity.
 * Feed root files and directory names; the editable outer label is excluded. */
typedef struct romx_savedatafiler_shape {
    char low[9];
    unsigned int members;
} romx_savedatafiler_shape_t;

static inline int romx_savedatafiler_add(romx_savedatafiler_shape_t *shape,
    const char *name, int is_directory)
{
    char id[9];
    unsigned int member;
    size_t size = strlen(name);
    if (is_directory) {
        if (!romx_hex_string(name, 8U)) return 0;
        member = 1U;
    } else if (romx_ascii_fold_equal(name, "export.log")) {
        shape->members |= 8U;
        return 1;
    } else {
        if ((size != 12U && size != 13U) ||
            (size == 13U && name[8] != '_') ||
            !romx_ascii_fold_equal(name + size - 4U, ".dat")) return 0;
        memcpy(id, name, 8U);
        id[8] = '\0';
        if (!romx_hex_string(id, 8U)) return 0;
        member = size == 12U ? 2U : 4U;
    }
    romx_copy_hex_upper(name, 8U, id);
    if (shape->low[0] != '\0' && strcmp(shape->low, id) != 0) return 0;
    memcpy(shape->low, id, sizeof(shape->low));
    shape->members |= member;
    return 1;
}

static inline int romx_savedatafiler_finish(
    const romx_savedatafiler_shape_t *shape, char low[9])
{
    if (shape->members != 15U) return 0;
    memcpy(low, shape->low, sizeof(shape->low));
    return 1;
}

#endif
