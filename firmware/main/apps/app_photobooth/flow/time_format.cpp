/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "time_format.h"

#include <cstdio>
#include <cstring>

#include "../view/strings.h"

namespace photobooth::flow {

void formatExpires(const char* iso, char* out, size_t len)
{
    if (iso == nullptr || strlen(iso) < 16 || iso[10] != 'T' || iso[13] != ':') {
        snprintf(out, len, "%s", str::kExpiresUnknown);
        return;
    }
    snprintf(out, len, "%.5s", iso + 11);
}

}  // namespace photobooth::flow
