#include "pm/audio/tts/windows_tts.hpp"

#include "pm/audio/constants.hpp"

#include <algorithm>
#include <cctype>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(PM_AUDIO_HAS_WINRT) && PM_AUDIO_HAS_WINRT

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>
#include "pm/audio/text.hpp"

namespace pm::tts {
namespace {


bool voice_is_sentinel(const std::string& voice)
{
    return voice.empty()
        || voice == pm::audio::constants::k_elevenlabs_voice_id
        || voice == pm::audio::constants::k_proxy_default_voice;
}

void ensure_apartment()
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error&) {
    }
}

std::vector<uint8_t> read_stream(winrt::Windows::Storage::Streams::IRandomAccessStream const& stream)
{
    const uint64_t size64 = stream.Size();
    if (size64 == 0 || size64 > 128ull * 1024ull * 1024ull)
        throw std::runtime_error("windows tts returned an empty or oversized stream");
    const uint32_t size = static_cast<uint32_t>(size64);
    winrt::Windows::Storage::Streams::DataReader reader{stream.GetInputStreamAt(0)};
    reader.LoadAsync(size).get();
    std::vector<uint8_t> out(size);
    if (!out.empty())
        reader.ReadBytes(winrt::array_view<uint8_t>(out));
    return out;
}

winrt::Windows::Media::SpeechSynthesis::VoiceInformation pick_voice(const std::string& voice)
{
    using winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer;
    using winrt::Windows::Media::SpeechSynthesis::VoiceInformation;
    if (voice_is_sentinel(voice))
        return SpeechSynthesizer::DefaultVoice();

    const std::string want = pm::audio::lower_ascii(voice);
    std::string listed;
    for (VoiceInformation const& candidate : SpeechSynthesizer::AllVoices()) {
        const std::string lang = pm::audio::lower_ascii(winrt::to_string(candidate.Language()));
        const std::string name = pm::audio::lower_ascii(winrt::to_string(candidate.DisplayName()));
        const std::string id = pm::audio::lower_ascii(winrt::to_string(candidate.Id()));
        if (!listed.empty())
            listed += ", ";
        listed += winrt::to_string(candidate.DisplayName());
        listed += " (";
        listed += winrt::to_string(candidate.Language());
        listed += ")";
        if (want == lang || want == name || want == id)
            return candidate;
    }
    throw std::runtime_error("windows tts voice not installed: " + voice + ". Installed: " + listed);
}

bool looks_like_ssml(const std::string& text)
{
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
    const std::string head = pm::audio::lower_ascii(text.substr(i, 6));
    return head == "<speak";
}

std::vector<uint8_t> synthesize_on_mta(const std::string& text, const std::string& voice)
{
    ensure_apartment();
    winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer synth;
    synth.Voice(pick_voice(voice));
    const auto wide = winrt::to_hstring(text);
    auto stream = looks_like_ssml(text)
        ? synth.SynthesizeSsmlToStreamAsync(wide).get()
        : synth.SynthesizeTextToStreamAsync(wide).get();
    auto bytes = read_stream(stream);
    stream.Close();
    if (bytes.size() < 44 || std::string(reinterpret_cast<const char*>(bytes.data()), 4) != "RIFF")
        throw std::runtime_error("windows tts did not return a WAV stream");
    return bytes;
}

} // namespace

std::vector<uint8_t> windows_tts_synthesize(const std::string& text, const std::string& voice)
{
    if (text.empty())
        throw std::runtime_error("windows tts: text is empty");
    // STA callers deadlock on IAsyncOperation::get(). Synthesize on an MTA thread.
    std::packaged_task<std::vector<uint8_t>()> task([&] {
        try {
            return synthesize_on_mta(text, voice);
        } catch (const std::runtime_error&) {
            throw;
        } catch (const winrt::hresult_error& ex) {
            throw std::runtime_error(std::string("windows tts: ") + winrt::to_string(ex.message()));
        }
    });
    auto done = task.get_future();
    std::thread(std::move(task)).join();
    return done.get();
}

} // namespace pm::tts

#else

namespace pm::tts {

std::vector<uint8_t> windows_tts_synthesize(const std::string&, const std::string&)
{
    throw std::runtime_error("windows tts is only available on Windows");
}

} // namespace pm::tts

#endif
