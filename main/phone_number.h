#pragma once

#include <stddef.h>
#include <string.h>
#include "cellular_protocol.h"

// Strips common display formatting (spaces, dashes, brackets, dots) and
// checks the result with the SMS protocol's number rules. `in` and `out` may
// be the same buffer. On failure `out` is left unchanged.
inline bool normalize_phone(const char *in, char *out, size_t capacity)
{
    if (!in || !out) return false;
    char normalized[cellular::PHONE_CAPACITY] = {};
    size_t used = 0;
    for (const char *p = in; *p; ++p) {
        if (*p == ' ' || *p == '-' || *p == '(' || *p == ')' || *p == '.') continue;
        if (used + 1 >= sizeof(normalized)) return false;
        normalized[used++] = *p;
    }
    if (!cellular::valid_number(normalized) || used + 1 > capacity) return false;
    memcpy(out, normalized, used + 1);
    return true;
}
