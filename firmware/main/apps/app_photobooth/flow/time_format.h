/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 写真の期限 (photo_ready の expires_at) を画面用の "HH:MM" にする (docs/design/testable-logic-step1.md §5 F)。
// flow/flow.cpp から切り出した純粋な関数 (firmware/tests/time_format_test.cpp でホストでテストする)。
#pragma once

#include <cstddef>

namespace photobooth::flow {

// "2026-09-30T22:00:00+09:00" → "22:00"。形が違えば (nullptr を含む) str::kExpiresUnknown ("--:--")。
// out は len バイト (NUL を含む) に収まるよう切り詰める。
void formatExpires(const char* iso, char* out, size_t len);

}  // namespace photobooth::flow
