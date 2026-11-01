#pragma once

#include <string>

namespace pm::audio::dynlib {

bool probe(const std::string& name) noexcept;
void* load(const std::string& name, const std::string& search_dir = {}) noexcept;
void* sym(void* handle, const std::string& symbol) noexcept;
void unload(void* handle) noexcept;

} // namespace pm::audio::dynlib
