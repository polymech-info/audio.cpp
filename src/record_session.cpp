#include "pm/audio/record_session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <vector>
#else
#  include <csignal>
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace pm::audio::record_session {
namespace {

RecordSessionDirFn& dir_slot()
{
    static RecordSessionDirFn fn = nullptr;
    return fn;
}

RecordCliExeFn& cli_slot()
{
    static RecordCliExeFn fn = nullptr;
    return fn;
}

constexpr std::size_t k_file_size = pm::audio::record_session::k_file_size;

using Shared = pm::audio::record_session::SessionShared;

uint64_t now_unix_ms()
{
    namespace chr = std::chrono;
    return static_cast<uint64_t>(chr::duration_cast<chr::milliseconds>(
        chr::system_clock::now().time_since_epoch()).count());
}

void copy_into(char* dst, std::size_t cap, std::string_view src)
{
    if (cap == 0) return;
    const std::size_t n = std::min(cap - 1, src.size());
    if (n > 0)
        std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

std::filesystem::path session_dir()
{
    if (RecordSessionDirFn fn = dir_slot()) {
        std::filesystem::path dir = fn();
        if (!dir.empty())
            return dir;
    }
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
        base = ".";
    return base / "pm-audio";
}

#if defined(_WIN32)
uint64_t current_pid() { return static_cast<uint64_t>(::GetCurrentProcessId()); }

bool process_alive(uint64_t pid)
{
    if (pid == 0)
        return false;
    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                             static_cast<DWORD>(pid));
    if (!h)
        return false;
    DWORD code = STILL_ACTIVE;
    const BOOL ok = ::GetExitCodeProcess(h, &code);
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
}

std::wstring quote_arg(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 2);
    out += L'"';
    for (wchar_t c : s) {
        if (c == L'"')
            out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

struct DefaultMicPaths {
    std::filesystem::path wav;
    std::filesystem::path txt;
};

DefaultMicPaths default_mic_paths()
{
    namespace chr = std::chrono;
    const auto  t  = chr::system_clock::to_time_t(chr::system_clock::now());
    std::tm     tm = {};
    (void)localtime_s(&tm, &t);
    std::ostringstream tag;
    tag << "mic-" << std::put_time(&tm, "%Y%m%d-%H%M%S");
    const auto dir = session_dir();
    return {dir / (tag.str() + ".wav"), dir / (tag.str() + ".txt")};
}

bool spawn_detached_record(const DefaultMicPaths& paths, std::string& err)
{
    RecordCliExeFn exe_fn = cli_slot();
    const std::filesystem::path exe = exe_fn ? exe_fn() : std::filesystem::path{};
    if (exe.empty()) {
        err = "record cli exe is not set";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(paths.wav.parent_path(), ec);

    std::wstring cmd = quote_arg(exe.wstring()) + L" audio record --stt --dst "
                       + quote_arg(paths.wav.wstring()) + L" --text-out "
                       + quote_arg(paths.txt.wstring());
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS;
    if (!::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                          nullptr, &si, &pi)) {
        err = "CreateProcessW failed: " + std::to_string(::GetLastError());
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}
#else
uint64_t current_pid() { return static_cast<uint64_t>(::getpid()); }

bool process_alive(uint64_t pid)
{
    if (pid == 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}
#endif

void init_shared(Shared& s, const StartInfo& info)
{
    std::memset(&s, 0, sizeof(s));
    s.magic            = k_magic;
    s.version          = k_version;
    s.stop             = 0;
    s.state            = static_cast<uint32_t>(State::Recording);
    s.use_stt          = info.use_stt ? 1u : 0u;
    s.samples          = 0;
    s.pid              = current_pid();
    s.started_unix_ms  = now_unix_ms();
    copy_into(s.dst, k_path_cap, info.dst);
    copy_into(s.text_out, k_path_cap, info.text_out);
    copy_into(s.format, k_format_cap, info.format);
    copy_into(s.stt_provider, k_provider_cap, info.stt_provider);
    copy_into(s.stt_model, k_model_cap, info.stt_model);
    s.device[0] = '\0';
    s.stt_buffer[0] = '\0';
}

bool map_existing(const std::filesystem::path& path, void*& view, std::size_t& view_size,
#if defined(_WIN32)
                  void*& file_handle, void*& map_handle,
#else
                  int& fd,
#endif
                  std::string& err)
{
    view      = nullptr;
    view_size = k_file_size;
#if defined(_WIN32)
    file_handle = map_handle = nullptr;
    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        err = "cannot open session file (err=" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    HANDLE hMap = ::CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(k_file_size), nullptr);
    if (!hMap) {
        err = "CreateFileMapping failed (err=" + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(hFile);
        return false;
    }
    void* p = ::MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, k_file_size);
    if (!p) {
        err = "MapViewOfFile failed (err=" + std::to_string(::GetLastError()) + ")";
        ::CloseHandle(hMap);
        ::CloseHandle(hFile);
        return false;
    }
    view        = p;
    file_handle = hFile;
    map_handle  = hMap;
    return true;
#else
    fd = -1;
    const int f = ::open(path.c_str(), O_RDWR);
    if (f < 0) {
        err = std::string("cannot open session file: ") + std::strerror(errno);
        return false;
    }
    void* p = ::mmap(nullptr, k_file_size, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    if (p == MAP_FAILED) {
        err = std::string("mmap failed: ") + std::strerror(errno);
        ::close(f);
        return false;
    }
    view = p;
    fd   = f;
    return true;
#endif
}

void unmap(void* view, std::size_t view_size,
#if defined(_WIN32)
           void* file_handle, void* map_handle
#else
           int fd
#endif
          )
{
    if (view) {
#if defined(_WIN32)
        ::UnmapViewOfFile(view);
#else
        ::munmap(view, view_size);
#endif
    }
#if defined(_WIN32)
    (void)view_size;
    if (map_handle)
        ::CloseHandle(static_cast<HANDLE>(map_handle));
    if (file_handle)
        ::CloseHandle(static_cast<HANDLE>(file_handle));
#else
    if (fd >= 0)
        ::close(fd);
    (void)view_size;
#endif
}

bool create_session_file(const std::filesystem::path& path, std::string& err)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

#if defined(_WIN32)
    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        if (e == ERROR_FILE_EXISTS)
            err = "session file already exists";
        else
            err = "CreateFileW failed (err=" + std::to_string(e) + ")";
        return false;
    }
    LARGE_INTEGER sz{};
    sz.QuadPart = static_cast<LONGLONG>(k_file_size);
    if (!::SetFilePointerEx(hFile, sz, nullptr, FILE_BEGIN) || !::SetEndOfFile(hFile)) {
        err = "failed to size session file";
        ::CloseHandle(hFile);
        std::filesystem::remove(path, ec);
        return false;
    }
    ::CloseHandle(hFile);
    return true;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            err = "session file already exists";
        else
            err = std::string("open(O_EXCL) failed: ") + std::strerror(errno);
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(k_file_size)) != 0) {
        err = std::string("ftruncate failed: ") + std::strerror(errno);
        ::close(fd);
        std::filesystem::remove(path, ec);
        return false;
    }
    ::close(fd);
    return true;
#endif
}

Shared* validated(Shared* s, std::string& err)
{
    if (!s) {
        err = "session not mapped";
        return nullptr;
    }
    if (s->magic != k_magic || s->version != k_version) {
        err = "session file corrupt or wrong version";
        return nullptr;
    }
    return s;
}

} // namespace

void set_record_session_dir(RecordSessionDirFn fn)
{
    dir_slot() = fn;
}

void set_record_cli_exe(RecordCliExeFn fn)
{
    cli_slot() = fn;
}

std::filesystem::path session_file_path()
{
    return session_dir() / "audio-record.session";
}

Owner::~Owner() { release(); }

void Owner::release() noexcept
{
    if (shared_ && owned_) {
        shared_->state = static_cast<uint32_t>(State::Done);
        std::atomic_thread_fence(std::memory_order_release);
    }
    if (view_) {
        unmap(view_, view_size_,
#if defined(_WIN32)
              file_handle_, map_handle_
#else
              fd_
#endif
        );
    }
    view_ = nullptr;
    shared_ = nullptr;
    if (owned_) {
        std::error_code ec;
        std::filesystem::remove(session_file_path(), ec);
    }
    owned_ = false;
#if defined(_WIN32)
    file_handle_ = map_handle_ = nullptr;
#else
    fd_ = -1;
#endif
}

bool Owner::acquire(const StartInfo& info, std::string& err)
{
    release();
    const auto path = session_file_path();

    if (std::filesystem::exists(path)) {
        Status existing{};
        std::string read_err;
        if (read_status(existing, read_err) && existing.active) {
            err = "already recording (pid " + std::to_string(existing.pid) + ")";
            return false;
        }
        std::string clean_err;
        (void)cleanup_stale(clean_err);
    }

    if (!create_session_file(path, err)) {
        if (err == "session file already exists") {
            Status existing{};
            std::string read_err;
            if (read_status(existing, read_err) && existing.active) {
                err = "already recording (pid " + std::to_string(existing.pid) + ")";
                return false;
            }
            std::string clean_err;
            (void)cleanup_stale(clean_err);
            if (!create_session_file(path, err))
                return false;
        } else {
            return false;
        }
    }

    if (!map_existing(path, view_, view_size_,
#if defined(_WIN32)
                      file_handle_, map_handle_,
#else
                      fd_,
#endif
                      err)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }

    shared_ = static_cast<Shared*>(view_);
    init_shared(*shared_, info);
    std::atomic_thread_fence(std::memory_order_release);
    owned_ = true;
    return true;
}

void Owner::set_device(std::string_view device)
{
    if (!shared_) return;
    copy_into(shared_->device, k_name_cap, device);
    std::atomic_thread_fence(std::memory_order_release);
}

void Owner::set_samples(uint32_t samples)
{
    if (!shared_) return;
    shared_->samples = samples;
    std::atomic_thread_fence(std::memory_order_release);
}

void Owner::set_stt_buffer(std::string_view text)
{
    if (!shared_) return;
    copy_into(shared_->stt_buffer, k_stt_cap, text);
    std::atomic_thread_fence(std::memory_order_release);
}

void Owner::set_state(State state)
{
    if (!shared_) return;
    shared_->state = static_cast<uint32_t>(state);
    std::atomic_thread_fence(std::memory_order_release);
}

bool Owner::stop_requested() const
{
    if (!shared_) return false;
    std::atomic_thread_fence(std::memory_order_acquire);
    return shared_->stop != 0;
}

bool cleanup_stale(std::string& err)
{
    err.clear();
    const auto path = session_file_path();
    if (!std::filesystem::exists(path))
        return true;

    void* view = nullptr;
    std::size_t view_size = 0;
#if defined(_WIN32)
    void* fh = nullptr;
    void* mh = nullptr;
    if (!map_existing(path, view, view_size, fh, mh, err))
        return false;
    auto* s = static_cast<Shared*>(view);
#else
    int fd = -1;
    if (!map_existing(path, view, view_size, fd, err))
        return false;
    auto* s = static_cast<Shared*>(view);
#endif

    bool remove = true;
    if (s->magic == k_magic && s->version == k_version && process_alive(s->pid))
        remove = false;

    unmap(view, view_size,
#if defined(_WIN32)
          fh, mh
#else
          fd
#endif
    );

    if (remove) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (ec) {
            err = "failed to remove stale session file";
            return false;
        }
    }
    return true;
}

bool read_status(Status& out, std::string& err)
{
    out = {};
    err.clear();
    const auto path = session_file_path();
    if (!std::filesystem::exists(path)) {
        return true;
    }

    void* view = nullptr;
    std::size_t view_size = 0;
#if defined(_WIN32)
    void* fh = nullptr;
    void* mh = nullptr;
    if (!map_existing(path, view, view_size, fh, mh, err))
        return false;
#else
    int fd = -1;
    if (!map_existing(path, view, view_size, fd, err))
        return false;
#endif

    auto* s = static_cast<Shared*>(view);
    if (!validated(s, err)) {
        unmap(view, view_size,
#if defined(_WIN32)
              fh, mh
#else
              fd
#endif
        );
        return false;
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    out.pid              = s->pid;
    out.state            = static_cast<State>(s->state);
    out.use_stt          = s->use_stt != 0;
    out.samples          = s->samples;
    out.started_unix_ms  = s->started_unix_ms;
    out.device           = s->device;
    out.format           = s->format;
    out.stt_provider     = s->stt_provider;
    out.stt_model        = s->stt_model;
    out.dst              = s->dst;
    out.text_out         = s->text_out;
    out.stt_buffer       = s->stt_buffer;
    out.active = process_alive(s->pid) && out.state != State::Done;
    out.stale  = !out.active;

    unmap(view, view_size,
#if defined(_WIN32)
          fh, mh
#else
          fd
#endif
    );
    return true;
}

bool request_stop(std::string& err)
{
    err.clear();
    const auto path = session_file_path();
    if (!std::filesystem::exists(path)) {
        err = "no active recording session";
        return false;
    }

    void* view = nullptr;
    std::size_t view_size = 0;
#if defined(_WIN32)
    void* fh = nullptr;
    void* mh = nullptr;
    if (!map_existing(path, view, view_size, fh, mh, err))
        return false;
#else
    int fd = -1;
    if (!map_existing(path, view, view_size, fd, err))
        return false;
#endif

    auto* s = static_cast<Shared*>(view);
    if (!validated(s, err)) {
        unmap(view, view_size,
#if defined(_WIN32)
              fh, mh
#else
              fd
#endif
        );
        return false;
    }

    if (!process_alive(s->pid)) {
        unmap(view, view_size,
#if defined(_WIN32)
              fh, mh
#else
              fd
#endif
        );
        std::string clean_err;
        (void)cleanup_stale(clean_err);
        err = "recording process is not running";
        return false;
    }

    s->stop  = 1;
    s->state = static_cast<uint32_t>(State::Stopping);
    std::atomic_thread_fence(std::memory_order_release);

    unmap(view, view_size,
#if defined(_WIN32)
          fh, mh
#else
          fd
#endif
    );
    return true;
}

bool toggle_detached(std::string& err)
{
    err.clear();
    Status info{};
    std::string read_err;
    const bool read_ok = read_status(info, read_err);
    if (!read_ok) {
        std::string clean_err;
        (void)cleanup_stale(clean_err);
        info = {};
    }

    if (info.active) {
        if (request_stop(err))
            return true;
        std::string clean_err;
        (void)cleanup_stale(clean_err);
        err = "recording ended before stop could be sent; session cleared";
        return true;
    }

    if (info.stale || std::filesystem::exists(session_file_path())) {
        std::string clean_err;
        (void)cleanup_stale(clean_err);
        err.clear();
    }

#if defined(_WIN32)
    return spawn_detached_record(default_mic_paths(), err);
#else
    err = "detached mic record requires Win32";
    return false;
#endif
}

} // namespace pm::audio::record_session
