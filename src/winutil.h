// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace live {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);   // UTF-8

std::optional<std::string> readFile(const std::wstring& path);
// Writes next to `path` and renames over it, so a crash never leaves half a file.
bool writeFileAtomic(const std::wstring& path, std::string_view data);
bool writeFile(const std::wstring& path, const void* data, size_t size);
bool fileExists(const std::wstring& path);

} // namespace live
