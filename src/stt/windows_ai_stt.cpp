#include "pm/audio/stt/windows_ai_stt.hpp"

#include <stdexcept>

namespace pm::stt {
namespace {

WindowsAiPackagedFn& packaged_slot()
{
    static WindowsAiPackagedFn fn = nullptr;
    return fn;
}

} // namespace

void set_windows_ai_packaged(WindowsAiPackagedFn fn)
{
    packaged_slot() = fn;
}

} // namespace pm::stt

#if defined(PM_AUDIO_HAS_WINRT) && PM_AUDIO_HAS_WINRT

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>

namespace pm::stt {
namespace {

bool os_supports_windows_ai()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;
    const auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtl)
        return false;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl(&info) != 0)
        return false;
    return info.dwMajorVersion > 10
        || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 26100);
}

bool package_full_name_is_runtime(const wchar_t* full_name)
{
    return wcsncmp(full_name, L"Microsoft.WindowsAppRuntime.1.7_", 32) == 0
        || wcsncmp(full_name, L"Microsoft.WindowsAppRuntime.1.8_", 32) == 0
        || wcsncmp(full_name, L"Microsoft.WindowsAppRuntime.2_", 30) == 0;
}

bool windows_app_runtime_1_7_or_later()
{
    // FindPackagesByPackageFamily and directory listing of WindowsApps both
    // fail from an unpackaged process (empty set, or access denied). The
    // state repository still records the installed runtime packages.
    HKEY root = nullptr;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModel\\StateRepository\\Cache\\Package\\Data",
            0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t subkey[256];
    for (DWORD index = 0; !found; ++index) {
        DWORD subkey_len = sizeof(subkey) / sizeof(subkey[0]);
        if (RegEnumKeyExW(root, index, subkey, &subkey_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY child = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &child) != ERROR_SUCCESS)
            continue;
        wchar_t full_name[512];
        DWORD bytes = sizeof(full_name);
        DWORD type = 0;
        if (RegQueryValueExW(child, L"PackageFullName", nullptr, &type,
                             reinterpret_cast<BYTE*>(full_name), &bytes) == ERROR_SUCCESS
            && type == REG_SZ)
            found = package_full_name_is_runtime(full_name);
        RegCloseKey(child);
    }
    RegCloseKey(root);
    return found;
}

bool speech_type_present()
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error&) {
    }
    try {
        return winrt::Windows::Foundation::Metadata::ApiInformation::IsTypePresent(
            L"Microsoft.Windows.AI.Speech.SpeechRecognitionModel");
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

} // namespace

bool has_package_identity()
{
    if (WindowsAiPackagedFn fn = packaged_slot())
        return fn();
    return false;
}

WindowsAiSttAvailability windows_ai_stt_availability()
{
    if (!has_package_identity())
        return WindowsAiSttAvailability::Unpackaged;
    if (!os_supports_windows_ai())
        return WindowsAiSttAvailability::UnsupportedOs;
    if (!windows_app_runtime_1_7_or_later())
        return WindowsAiSttAvailability::RuntimeMissing;
    if (!speech_type_present())
        return WindowsAiSttAvailability::ApiMissing;
    return WindowsAiSttAvailability::Ready;
}

const char* windows_ai_stt_availability_message(WindowsAiSttAvailability availability)
{
    switch (availability) {
    case WindowsAiSttAvailability::Ready:
        return "Windows AI speech is registered";
    case WindowsAiSttAvailability::UnsupportedOs:
        return "Windows AI speech requires Windows 11 24H2 (build 26100) or later";
    case WindowsAiSttAvailability::Unpackaged:
        return "Windows AI speech requires the packaged app (systemAIModels)";
    case WindowsAiSttAvailability::RuntimeMissing:
        return "Windows AI speech requires Windows App SDK runtime 1.7.1 or later";
    case WindowsAiSttAvailability::ApiMissing:
        return "Windows AI speech is not in the installed Windows App Runtime "
               "(Microsoft.Windows.AI.Speech.SpeechRecognitionModel is not registered)";
    }
    return "Windows AI speech is not available";
}

std::string windows_ai_stt_transcribe_file(const std::string& wav_path)
{
    if (wav_path.empty())
        throw std::runtime_error("windows ai stt: audio path is empty");
    const auto availability = windows_ai_stt_availability();
    if (availability != WindowsAiSttAvailability::Ready)
        throw std::runtime_error(windows_ai_stt_availability_message(availability));
    // GetReadyState / RecognizeFromFile live on SpeechRecognitionModel. The
    // stable 1.7, 1.8, and 2.x runtimes do not ship that contract, so this
    // build does not link it. EnsureReadyAsync is intentionally not called.
    throw std::runtime_error(
        "Windows AI speech type is registered, but this build has no stable "
        "SpeechRecognitionModel projection to call RecognizeFromFile");
}

} // namespace pm::stt

#else

namespace pm::stt {

bool has_package_identity()
{
    return false;
}

WindowsAiSttAvailability windows_ai_stt_availability()
{
    return WindowsAiSttAvailability::ApiMissing;
}

const char* windows_ai_stt_availability_message(WindowsAiSttAvailability)
{
    return "Windows AI speech is only available on Windows";
}

std::string windows_ai_stt_transcribe_file(const std::string&)
{
    throw std::runtime_error(windows_ai_stt_availability_message(WindowsAiSttAvailability::ApiMissing));
}

} // namespace pm::stt

#endif
