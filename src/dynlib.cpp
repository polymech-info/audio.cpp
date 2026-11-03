#include "pm/audio/dynlib.hpp"
#include "fs/path_sanitizer.hpp"

#include <filesystem>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif !defined(__wasm__)
#  include <dlfcn.h>
#endif

namespace pm::audio::dynlib {

bool probe(const std::string& name) noexcept
{
#if defined(_WIN32)
    if (name.empty())
        return false;
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        return false;
    std::wstring wname(static_cast<size_t>(wlen - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname.data(), wlen);
    HMODULE module = ::LoadLibraryExW(wname.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!module)
        return false;
    ::FreeLibrary(module);
    return true;
#elif !defined(__wasm__)
    if (name.empty())
        return false;
    void* handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (handle) {
        ::dlclose(handle);
        return true;
    }
    handle = ::dlopen(name.c_str(), RTLD_LAZY);
    if (!handle)
        return false;
    ::dlclose(handle);
    return true;
#else
    (void)name;
    return false;
#endif
}

void* load(const std::string& name, const std::string& search_dir) noexcept
{
#if defined(_WIN32)
    if (name.empty())
        return nullptr;
    auto to_wide = [](const std::string& text) -> std::wstring {
        if (text.empty())
            return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (n <= 0)
            return {};
        std::wstring wide(static_cast<size_t>(n - 1), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), n);
        return wide;
    };
    if (!search_dir.empty()) {
        const std::filesystem::path full = media::path::path_from_utf8(search_dir) / name;
        HMODULE module = ::LoadLibraryExW(full.wstring().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module)
            return static_cast<void*>(module);
    }
    const std::wstring wname = to_wide(name);
    if (wname.empty())
        return nullptr;
    return static_cast<void*>(::LoadLibraryExW(
        wname.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS));
#elif !defined(__wasm__)
    if (name.empty())
        return nullptr;
    if (!search_dir.empty()) {
        const std::filesystem::path full = media::path::path_from_utf8(search_dir) / name;
        if (void* handle = ::dlopen(full.c_str(), RTLD_LAZY))
            return handle;
    }
    return ::dlopen(name.c_str(), RTLD_LAZY);
#else
    (void)name;
    (void)search_dir;
    return nullptr;
#endif
}

void* sym(void* handle, const std::string& symbol) noexcept
{
#if defined(_WIN32)
    if (!handle || symbol.empty())
        return nullptr;
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), symbol.c_str()));
#elif !defined(__wasm__)
    if (!handle || symbol.empty())
        return nullptr;
    return ::dlsym(handle, symbol.c_str());
#else
    (void)handle;
    (void)symbol;
    return nullptr;
#endif
}

void unload(void* handle) noexcept
{
#if defined(_WIN32)
    if (handle)
        ::FreeLibrary(static_cast<HMODULE>(handle));
#elif !defined(__wasm__)
    if (handle)
        ::dlclose(handle);
#else
    (void)handle;
#endif
}

} // namespace pm::audio::dynlib
