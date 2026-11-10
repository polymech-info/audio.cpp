#include "pm_image_cmd_reader.hpp"
#include "reader_optimize_default.hpp"

#if defined(FEATURE_READER) && FEATURE_READER

#include "tts/reader.hpp"

#include "core/settings_store.hpp"
#include "core/settings_runtime.hpp"
#include "fs/path_sanitizer.hpp"
#include "audio/audio_constants.hpp"
#include "audio/speech_factory.hpp"
#include "core/audio_devices.hpp"
#include "core/cli_cancel.hpp"
#include "html/html.h"
#include "net/url_fetch.hpp"
#include "llm/agent.hpp"
#include "llm/agent_launch.hpp"
#if defined(PM_PLUGIN_CHAT) && PM_PLUGIN_CHAT
#include "plugins/chat/chat_wire.hpp"
#include "plugins/chat/host_chat.hpp"
#endif

#if defined(FEATURE_STT) && FEATURE_STT
#include "tts/elevenlabs_tts.hpp"
#include "tts/proxy_tts.hpp"
#include "tts/tts_text_input.hpp"
#include "tts/voiceover.hpp"
#endif

#if defined(FEATURE_COMMAND_MARKDOWN) && FEATURE_COMMAND_MARKDOWN
#include "markdown/markdown.hpp"
#endif

#if defined(FEATURE_COMMAND_PDF) && FEATURE_COMMAND_PDF
#include "pdf/pdf_pages.hpp"
#include "pdf/pdf_rasterizer.hpp"
#include "core/meta.hpp"
#include "pm_image_cli_helpers.hpp"
#endif

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& file, std::string& err) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        err = "cannot read " + media::path::path_to_utf8(file);
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

std::string read_prefix(const fs::path& file, std::size_t n, std::string& err) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        err = "cannot read " + media::path::path_to_utf8(file);
        return {};
    }
    std::string buf(n, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(in.gcount() > 0 ? in.gcount() : 0));
    return buf;
}

#if defined(FEATURE_STT) && FEATURE_STT
void reader_chunk_progress(const pm::tts::SpeechProgress& step) {
    pm::tts::reader::WorkProgress progress;
    progress.phase = "speak";
    progress.index = static_cast<std::int64_t>(step.index);
    progress.total = static_cast<std::int64_t>(step.count);
    progress.done = static_cast<std::int64_t>(step.chars_done);
    progress.whole = static_cast<std::int64_t>(step.chars_total);
    progress.elapsed_ms = step.elapsed_ms;
    std::cout << pm::tts::reader::format_progress(progress) << "\n" << std::flush;
}

void write_plan_for(const fs::path& dir, const std::string& speakable, const PmImageCliState& st) {
    pm::tts::ElevenLabsTTSConfig cfg;
    cfg.chunk_sentences = !st.reader_no_chunk;
    cfg.chunk_target_chars = static_cast<std::size_t>(st.reader_chunk_target);
    cfg.chunk_max_chars = static_cast<std::size_t>(st.reader_chunk_max);
    const auto chunks = pm::tts::tts_text_chunks(speakable, cfg);
    std::vector<std::int64_t> sizes;
    if (chunks.empty())
        sizes.push_back(static_cast<std::int64_t>(speakable.size()));
    else {
        sizes.reserve(chunks.size());
        for (const auto& part : chunks)
            sizes.push_back(static_cast<std::int64_t>(part.size()));
    }
    std::string err;
    if (!pm::tts::reader::write_speak_plan(dir.generic_string(),
                                          static_cast<std::int64_t>(speakable.size()), sizes, err)) {
        std::cerr << "reader: " << err << "\n";
        return;
    }
    pm::tts::reader::WorkProgress progress;
    progress.phase = "plan";
    progress.index = 0;
    progress.total = static_cast<std::int64_t>(sizes.size());
    progress.done = 0;
    progress.whole = static_cast<std::int64_t>(speakable.size());
    std::cout << pm::tts::reader::format_progress(progress) << "\n" << std::flush;
}

int speak_page(const std::string& page, const PmImageCliState& st, std::string& err,
               const std::string& audio_path = {}) {
    pm::tts::TtsTextInputResult text_in;
    if (!pm::tts::resolve_tts_text(page, text_in, err))
        return 1;
    if (text_in.text.empty()) {
        err = "page has no speakable text";
        return 1;
    }

    media::runtime_settings::ChatProviderSettings chat;
    media::runtime_settings::ProviderMap providers;
    std::string load_err;
    media::runtime_settings::load_chat_provider(chat, load_err);
    media::runtime_settings::load_providers(providers, load_err);
    const std::string settings_provider = chat.tts_provider.empty() ? "tanit" : chat.tts_provider;
    const pm::audio::SpeechProvider provider = pm::audio::resolve_speech_provider(
        st.reader_provider, {}, settings_provider, providers);
    if (provider.api_key.empty() || provider.base_url.empty()) {
        err = "no voice provider key. Set App Settings, or pass --provider";
        return 1;
    }
    if (!pm::audio::constants::is_speech_proxy_provider(provider.provider)) {
        err = "reader speak uses the voice proxy (" + provider.provider + " is not tanit, openai, or openrouter)";
        return 1;
    }

    std::string model = st.reader_model.empty() ? chat.tts_model : st.reader_model;
    if (model.empty() || model == "eleven_v3" || model == "tanit-speech")
        model = "tanit-speech-v4";
    std::string voice = st.reader_voice.empty() ? chat.tts_voice_id : st.reader_voice;
    if (voice.empty() || voice == pm::audio::constants::k_elevenlabs_voice_id)
        voice = pm::audio::constants::k_proxy_default_voice;
    std::cout << "voice " << voice << "\n" << std::flush;

    const pm::tts::DocumentSpeech speech = pm::tts::proxy_tts_document(
        text_in.text, provider.base_url, provider.api_key, model, voice,
        !st.reader_no_chunk,
        static_cast<std::size_t>(st.reader_chunk_target),
        static_cast<std::size_t>(st.reader_chunk_max),
        &reader_chunk_progress);
    if (speech.cancelled) {
        std::cout << "reader: cancelled\n";
        return 0;
    }
    if (!speech.error.empty() || speech.audio.empty()) {
        err = speech.error.empty() ? "server returned empty audio" : speech.error;
        return 1;
    }
    const bool join = speech.format == "wav";
    const std::vector<std::uint8_t>& audio = speech.audio;
    if (join)
        std::cout << "reader: joined pcm as wav\n" << std::flush;

    const std::string& chosen = audio_path.empty() ? st.reader_audio : audio_path;
    fs::path dst = chosen.empty()
        ? fs::path(page).replace_extension(join ? ".wav" : ".mp3")
        : fs::path(chosen);
    if (join && dst.extension() == ".mp3")
        dst.replace_extension(".wav");
    {
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "cannot write " + dst.generic_string();
            return 1;
        }
        out.write(reinterpret_cast<const char*>(audio.data()),
                  static_cast<std::streamsize>(audio.size()));
        if (!out) {
            err = "cannot write " + dst.generic_string();
            return 1;
        }
    }

    if (!st.reader_no_play && !media::cli::cancel_requested()) {
        try {
            pm::audio::AudioOutput player;
            std::atomic<bool> play_done{false};
            std::exception_ptr play_error;
            std::thread playback([&] {
                try {
                    player.play_sync(audio.data(), audio.size());
                } catch (...) {
                    play_error = std::current_exception();
                }
                play_done.store(true);
            });
            while (!play_done.load() && !media::cli::cancel_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (media::cli::cancel_requested())
                player.stop();
            playback.join();
            if (play_error)
                std::rethrow_exception(play_error);
        } catch (const std::exception& ex) {
            err = std::string("saved ") + dst.generic_string() + " playback: " + ex.what();
            std::cout << dst.generic_string() << "\n";
            return 1;
        }
        if (media::cli::cancel_requested()) {
            std::cout << dst.generic_string() << "\nreader: cancelled\n";
            return 0;
        }
    }
    std::cout << dst.generic_string() << "\n";
    return 0;
}
#endif

std::string rewrite_page(const std::string& page_text, const std::string& prompt,
                         const std::string& language, const std::string& chat_preset,
                         std::string& err, const std::string& system = {},
                         bool plan = false) {
    if (media::cli::cancel_requested()) {
        err = "cancelled";
        return {};
    }
    media::llm::agent::ProviderSettings provider;
    if (!chat_preset.empty()) {
        media::llm::agent::launch::AgentLaunchSpec spec;
        spec.preset = chat_preset;
        spec.no_tools = true;
        media::llm::agent::launch::AgentLaunchResolved resolved;
        if (!media::llm::agent::launch::resolve_agent_launch(spec, resolved, err))
            return {};
        provider = resolved.provider;
        provider.max_iterations = plan ? 30 : 0;
        const std::string label = resolved.selected_preset.name.empty()
            ? resolved.selected_preset.id
            : resolved.selected_preset.name;
        std::cout << "chat-preset " << label << "\n";
    } else {
        media::runtime_settings::ChatProviderSettings chat;
        std::string load_err;
        media::runtime_settings::load_chat_provider(chat, load_err);
        provider.router = chat.router.empty() ? "tanit" : chat.router;
        provider.model = chat.model.empty() ? "text-fast" : chat.model;
        provider.api_key = chat.api_key;
        provider.base_url = chat.base_url;
        provider.timeout_ms = chat.timeout_ms > 0 ? chat.timeout_ms : 60'000;
        provider.max_iterations = plan ? 30 : 0;
        if (provider.api_key.empty() || provider.base_url.empty())
            media::runtime_settings::merge_provider_credentials(
                provider.router, false, provider.api_key, provider.base_url);
    }
    if (provider.router.empty() || provider.model.empty()) {
        err = "no chat model configured";
        return {};
    }
    std::cout << "model " << provider.router << "/" << provider.model << "\n";

#if defined(PM_PLUGIN_CHAT) && PM_PLUGIN_CHAT
    nlohmann::json reply;
    try {
        reply = pm::plugins::chat_host::json_call("reader.rewrite", {
            {"page", page_text},
            {"prompt", prompt},
            {"language", language},
            {"system", system},
            {"planner", plan},
            {"provider", pm::plugins::chat_wire::provider_to_json(provider)},
        });
    } catch (const std::exception& ex) {
        err = ex.what();
        return {};
    }
    if (media::cli::cancel_requested() || reply.value("cancelled", false)) {
        err = "cancelled";
        return {};
    }
    const std::string text = reply.value("text", std::string{});
    if (!reply.value("ok", false) || text.empty()) {
        err = reply.value("error", std::string{"prompt returned no text"});
        return {};
    }
    return text;
#else
    media::llm::agent::Result result;
    try {
        result = media::llm::agent::run_spoken_page(
            page_text, prompt, language, provider, system, plan);
    } catch (const std::exception& ex) {
        err = ex.what();
        return {};
    }
    if (media::cli::cancel_requested() || result.cancelled) {
        err = "cancelled";
        return {};
    }
    if (!result.ok || result.final_text.empty()) {
        err = result.error.empty() ? "prompt returned no text" : result.error;
        return {};
    }
    return result.final_text;
#endif
}

#if defined(FEATURE_COMMAND_PDF) && FEATURE_COMMAND_PDF
bool vision_provider(std::string& provider, std::string& model, std::string& err) {
    media::runtime_settings::ChatProviderSettings chat;
    std::string load_err;
    media::runtime_settings::load_chat_provider(chat, load_err);
    provider = chat.ocr_provider.empty() ? chat.image_recognition_provider : chat.ocr_provider;
    model = chat.ocr_model.empty() ? chat.image_recognition_model : chat.ocr_model;
    if (provider.empty()) {
        err = "no vision provider. Set the chat OCR or image recognition provider";
        return false;
    }
    if (provider != "google" && provider != "openai" && provider != "tanit" && provider != "replicate") {
        err = "vision uses image_understand. The configured provider is " + provider;
        return false;
    }
    return true;
}

bool prepare_vision_pdf(const fs::path& pdf, const fs::path& dir, const std::string& pages_spec,
                        const std::string& vision_prompt, bool call_model, std::string& source,
                        std::vector<int>& source_pages, std::string& err) {
    source_pages.clear();
    if (!pm::pdf::Document::ensure_library(&err)) {
        if (err.empty())
            err = "pdf renderer is not available";
        return false;
    }
    pm::pdf::Document doc;
    if (!doc.open_file(pdf, nullptr, err))
        return false;
    const int count = doc.page_count();
    std::vector<int> indexes;
    if (!pm::pdf::parse_page_spec(pages_spec.empty() ? "all" : pages_spec, count, indexes, err))
        return false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    source_pages.reserve(indexes.size());
    std::vector<std::string> bodies;
    bodies.reserve(indexes.size());
    std::string provider;
    std::string model;
    if (call_model && !vision_provider(provider, model, err))
        return false;
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        if (media::cli::cancel_requested()) {
            err = "cancelled";
            return false;
        }
        const int page_one = indexes[i] + 1;
        source_pages.push_back(page_one);
        pm::tts::reader::WorkProgress progress;
        progress.phase = "vision";
        progress.index = static_cast<std::int64_t>(i + 1);
        progress.total = static_cast<std::int64_t>(indexes.size());
        progress.done = static_cast<std::int64_t>(i);
        progress.whole = progress.total;
        std::cout << pm::tts::reader::format_progress(progress) << "\n" << std::flush;
        pm::pdf::RenderOptions opts;
        opts.page_index = indexes[i];
        opts.dpi = 144.0;
        const fs::path png = dir / ("page-" + std::to_string(i + 1) + ".png");
        if (!doc.render_page_to_file(opts, png, "png", 95, err))
            return false;
        if (!call_model) {
            bodies.push_back("source-page: " + std::to_string(page_one) + "\n");
            continue;
        }
        media::MetaOptions options;
        options.provider = provider;
        options.model = model;
        options.prompt = vision_prompt;
        options.resize_first = true;
        options.resize_width = 1280;
        media_cli::apply_image_ai_credentials_from_app(options.provider, false, options.api_key, options.base_url);
        const media::UnderstandResult understood = media::image_understand(
            media::path::path_to_utf8(png), options, nullptr);
        if (media::cli::cancel_requested()) {
            err = "cancelled";
            return false;
        }
        if (!understood.ok || understood.text.empty()) {
            err = understood.error.empty() ? "vision returned no text for page " + std::to_string(page_one)
                                           : understood.error;
            return false;
        }
        bodies.push_back(understood.text);
    }
    std::ostringstream joined;
    for (std::size_t i = 0; i < source_pages.size(); ++i) {
        if (i > 0)
            joined << '\n';
        joined << "# page " << source_pages[i] << "\n\n" << bodies[i];
        if (bodies[i].empty() || bodies[i].back() != '\n')
            joined << '\n';
    }
    source = joined.str();
    return true;
}
#endif

fs::path download_url(const std::string& url, std::string& err) {
    std::error_code ec;
    fs::path dest = fs::temp_directory_path(ec);
    if (ec) {
        err = "no temp directory";
        return {};
    }
    dest /= "tanit-reader-" + pm::tts::reader::document_hash(url);
    if (!media::fetch_url_to_file(url, dest, 30, 8, err))
        return {};
    return dest;
}

std::string file_mtime(const fs::path& file) {
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec)
        return {};
    return std::to_string(stamp.time_since_epoch().count());
}

} // namespace

int pm_image_cmd_reader(CLI::App&, PmImageCliState& st) {
    if (st.reader_print_estimate) {
        pm::tts::reader::WorkProgress progress;
        progress.phase = "speak";
        progress.done = st.reader_done;
        progress.whole = st.reader_total;
        progress.elapsed_ms = st.reader_elapsed_ms;
        std::cout << pm::tts::reader::format_progress(progress) << "\n";
        return 0;
    }
    if (st.reader_print_preset) {
        const auto flags = pm::tts::reader::preset_flags(pm::tts::reader::Preset::Fast);
        std::cout << "{\"preset\":\"fast\",\"tools\":" << (flags.tools ? "true" : "false")
                  << ",\"mcp\":" << (flags.mcp ? "true" : "false")
                  << ",\"skills\":" << (flags.skills ? "true" : "false")
                  << ",\"planner\":" << (flags.planner ? "true" : "false")
                  << ",\"single_turn\":" << (flags.single_turn ? "true" : "false")
                  << "}\n";
        return 0;
    }
    if (st.reader_optimize || !st.reader_optimize_preset.empty()) {
        st.reader_optimize = true;
        if (!reader_optimize::find(st.reader_optimize_preset)) {
            std::cerr << "reader: unknown optimize preset " << st.reader_optimize_preset
                      << "\n" << reader_optimize::ids() << "\n";
            return 1;
        }
    }
    if (st.reader_text.empty()) {
        std::cerr << "reader: --text is required\n";
        return 1;
    }

    std::error_code ec;
    const bool is_url = media::is_http_url(st.reader_text);
    const fs::path file(st.reader_text);
    const bool is_file = !is_url && fs::is_regular_file(file, ec) && !ec;
    std::string key = st.reader_text;
    std::string extension;
    std::string mtime;
    if (is_file) {
        const fs::path canon = fs::weakly_canonical(fs::absolute(file, ec), ec);
        key = media::path::path_to_utf8(ec ? fs::absolute(file) : canon);
        extension = media::path::path_to_utf8(file.extension());
        mtime = file_mtime(ec ? file : canon);
    }

    pm::tts::reader::Interpreter interpreter =
        is_url ? pm::tts::reader::interpreter_for_url(key)
        : is_file ? pm::tts::reader::interpreter_for_extension(extension)
                  : pm::tts::reader::Interpreter::Text;
    if (!st.reader_interpreter.empty()
        && !pm::tts::reader::interpreter_from_name(st.reader_interpreter, interpreter)) {
        std::cerr << "reader: unknown interpreter " << st.reader_interpreter << "\n";
        return 1;
    }
    if (st.reader_print_interpreter) {
        std::cout << pm::tts::reader::interpreter_name(interpreter) << "\n";
        return interpreter == pm::tts::reader::Interpreter::None ? 1 : 0;
    }
    if (is_file && interpreter == pm::tts::reader::Interpreter::None) {
        std::cerr << "reader: no interpreter for " << extension << "\n";
        return 1;
    }
    if (!st.reader_pages.empty() && interpreter != pm::tts::reader::Interpreter::Vision) {
        std::cerr << "reader: --pages selects PDF pages for --interpreter vision\n";
        return 1;
    }
    if (interpreter == pm::tts::reader::Interpreter::Vision && !st.reader_chat_preset.empty()
        && !st.reader_optimize) {
        std::cerr << "reader: --chat-preset rewrites a text page. vision reads the page image\n";
        return 1;
    }
    const std::string parent_prompt = st.reader_prompt.empty() ? st.reader_direction : st.reader_prompt;
    bool vision_inferred = false;
    std::string vision_prompt;
    std::string identity = key + "\n" + pm::tts::reader::interpreter_name(interpreter);
    if (interpreter == pm::tts::reader::Interpreter::Vision) {
        if (is_file) {
            std::string ext = extension;
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".pdf") {
                std::cerr << "reader: vision reads PDF pages. This file has no page renderer\n";
                return 1;
            }
        } else if (!is_url) {
            std::cerr << "reader: vision reads a PDF\n";
            return 1;
        }
        vision_prompt = pm::tts::reader::vision_prompt_for(
            parent_prompt, st.reader_vision_prompt, vision_inferred);
        identity += "\n" + st.reader_pages + "\n" + vision_prompt;
    }

    const std::string root = st.reader_dst.empty()
        ? (media::settings::get_config_dir() / "reader").generic_string()
        : st.reader_dst;
    const std::string dir = pm::tts::reader::store_directory(root, identity);
    if (st.reader_clean) {
        std::string clean_err;
        if (!pm::tts::reader::erase_store(dir, clean_err)) {
            std::cerr << "reader: " << clean_err << "\n";
            return 1;
        }
        std::cout << "removed\n" << dir << "\n";
        return 0;
    }

    pm::tts::reader::Store store;
    std::string err;
    bool cached = false;
    if (!st.reader_refresh && pm::tts::reader::load_store(dir, store, err)
        && store.hash == pm::tts::reader::document_hash(identity)
        && store.mtime == mtime) {
        cached = true;
        store.cached = true;
    } else {
        err.clear();
        std::string source;
        fs::path fetched;
        struct FetchedFile {
            fs::path path;
            ~FetchedFile() {
                if (!path.empty())
                    fs::remove(path);
            }
        } fetched_file;
        if (is_url) {
            fetched = download_url(key, err);
            fetched_file.path = fetched;
            if (fetched.empty()) {
                std::cerr << "reader: " << (err.empty() ? "fetch failed" : err) << "\n";
                return 1;
            }
            const std::string prefix = read_prefix(fetched, 4096, err);
            if (!err.empty()) {
                std::cerr << "reader: " << err << "\n";
                return 1;
            }
            if (st.reader_interpreter.empty())
                interpreter = pm::tts::reader::interpreter_for_url(key, prefix);
            if (interpreter == pm::tts::reader::Interpreter::None) {
                std::cerr << "reader: no interpreter for " << key << "\n";
                return 1;
            }
        }
        const fs::path source_file = fetched.empty() ? file : fetched;
        std::vector<int> source_pages;
        if (interpreter == pm::tts::reader::Interpreter::Vision) {
#if defined(FEATURE_COMMAND_PDF) && FEATURE_COMMAND_PDF
            if (!prepare_vision_pdf(source_file, fs::path(dir), st.reader_pages, vision_prompt,
                                    !st.reader_no_speak, source, source_pages, err)) {
                if (err == "cancelled") {
                    std::cout << "reader: cancelled\n";
                    return 0;
                }
                std::cerr << "reader: " << err << "\n";
                return 1;
            }
#else
            std::cerr << "reader: vision needs the pdf renderer (FEATURE_COMMAND_PDF)\n";
            return 1;
#endif
        } else if (interpreter == pm::tts::reader::Interpreter::ImageUnderstand) {
            source.clear();
        } else if (interpreter == pm::tts::reader::Interpreter::Web) {
            source = read_file(source_file, err);
            if (!err.empty()) {
                std::cerr << "reader: " << err << "\n";
                return 1;
            }
            source = html::to_markdown(html::clean_for_markdown(source));
            if (source.empty()) {
                std::cerr << "reader: page has no readable text\n";
                return 1;
            }
        } else if (interpreter == pm::tts::reader::Interpreter::Anydoc) {
#if defined(FEATURE_COMMAND_MARKDOWN) && FEATURE_COMMAND_MARKDOWN
            const media::markdown::Result converted =
                media::markdown::to_markdown(source_file, media::markdown::Format::Detect);
            if (!converted.ok()) {
                std::cerr << "reader: " << (converted.error.empty() ? "conversion failed" : converted.error) << "\n";
                if (converted.status == media::markdown::Status::NeedsOcr)
                    std::cerr << "reader: scanned pages need --interpreter vision\n";
                return 1;
            }
            source = converted.text;
#else
            std::cerr << "reader: anydoc is not built (FEATURE_COMMAND_MARKDOWN)\n";
            return 1;
#endif
        } else if (is_file || !fetched.empty()) {
            source = read_file(source_file, err);
            if (!err.empty()) {
                std::cerr << "reader: " << err << "\n";
                return 1;
            }
        } else {
            source = st.reader_text;
        }

        store.root = root;
        store.path = (is_file || is_url) ? key : std::string();
        store.hash = pm::tts::reader::document_hash(identity);
        store.interpreter = pm::tts::reader::interpreter_name(interpreter);
        store.direction = interpreter == pm::tts::reader::Interpreter::Vision
            ? parent_prompt : st.reader_direction;
        store.vision_prompt = vision_prompt;
        store.vision_inferred = vision_inferred;
        store.source_pages = std::move(source_pages);
        store.language = st.reader_language;
        store.mtime = mtime;
        store.page = 1;
        store.stopped = false;
        if (!pm::tts::reader::write_pages(dir, store, source, err)) {
            std::cerr << "reader: " << err << "\n";
            return 1;
        }
    }

    if (st.reader_page > 0 && !pm::tts::reader::set_page(store, st.reader_page, err)) {
        std::cerr << "reader: " << err << "\n";
        return 1;
    }
    if (st.reader_resume && !pm::tts::reader::resume_pages(store, err)) {
        std::cerr << "reader: " << err << "\n";
        return 1;
    }
    if (st.reader_stop && !pm::tts::reader::stop_pages(store, err)) {
        std::cerr << "reader: " << err << "\n";
        return 1;
    }
    if (st.reader_next && !pm::tts::reader::advance_page(store, err)) {
        std::cerr << "reader: " << err << "\n";
        return 1;
    }

    if (interpreter == pm::tts::reader::Interpreter::ImageUnderstand)
        std::cout << "image_understand\n";
    else
        std::cout << (cached ? "cached" : "converted") << "\n";
    std::cout << store.dir << "\n";
    const std::string page = pm::tts::reader::page_file(store);
    if (!page.empty())
        std::cout << page << "\n";
    if (store.interpreter == "vision" && !store.vision_prompt.empty()) {
        std::cout << (store.vision_inferred ? "vision-prompt inferred\n" : "vision-prompt given\n");
        std::cout << store.vision_prompt << "\n";
    }
    const std::string prompt = parent_prompt;
    if ((st.reader_llm || !st.reader_chat_preset.empty()) && prompt.empty()) {
        std::cerr << "reader: --prompt is required\n";
        return 1;
    }
    std::string spoken = page;
    if (!prompt.empty() && store.interpreter != "vision") {
        if (page.empty()) {
            std::cerr << "reader: no page to rewrite\n";
            return 1;
        }
        const std::string original = read_file(fs::path(page), err);
        if (!err.empty()) {
            std::cerr << "reader: " << err << "\n";
            return 1;
        }
        const std::string text = rewrite_page(original, prompt, st.reader_language,
                                              st.reader_chat_preset, err);
        if (text.empty()) {
            if (err == "cancelled") {
                std::cout << "reader: cancelled\n";
                return 0;
            }
            std::cerr << "reader: " << err << "\n";
            return 1;
        }
        if (!pm::tts::reader::write_page_version(store, prompt, text, err, st.reader_chat_preset)) {
            std::cerr << "reader: " << err << "\n";
            return 1;
        }
        spoken = pm::tts::reader::speak_file(store);
        std::cout << "version " << store.speak_version << "\n" << spoken << "\n";
    } else if (store.speak_version > 0) {
        spoken = pm::tts::reader::speak_file(store);
    }
    if (st.reader_optimize) {
        const reader_optimize::Preset* preset = reader_optimize::find(st.reader_optimize_preset);
        reader_optimize::RunResult reviewed;
        auto rewrite = [&](const std::string& page, const std::string& prompt,
                           const std::string& system, std::string& model_err) {
            return rewrite_page(page, prompt, st.reader_language, st.reader_chat_preset,
                                 model_err, system, true);
        };
        auto see = [](const std::string& image, const std::string& question, std::string& see_err) -> std::string {
#if defined(FEATURE_COMMAND_PDF) && FEATURE_COMMAND_PDF
            std::string provider;
            std::string model;
            if (!vision_provider(provider, model, see_err))
                return {};
            media::MetaOptions options;
            options.provider = provider;
            options.model = model;
            options.prompt = question;
            options.resize_first = true;
            options.resize_width = 1280;
            media_cli::apply_image_ai_credentials_from_app(
                options.provider, false, options.api_key, options.base_url);
            const media::UnderstandResult understood = media::image_understand(image, options, nullptr);
            if (!understood.ok || understood.text.empty()) {
                see_err = understood.error.empty() ? "vision returned no text" : understood.error;
                return {};
            }
            return understood.text;
#else
            (void)image;
            (void)question;
            see_err = "vision needs the pdf renderer";
            return {};
#endif
        };
        if (!reader_optimize::run(store, *preset, rewrite, see, reviewed, err)) {
            if (err == "cancelled") {
                std::cout << "reader: cancelled\n";
                return 0;
            }
            std::cerr << "reader: " << err << "\n";
            return 1;
        }
        spoken = reviewed.spoken_path;
        std::cout << spoken << "\n" << reviewed.diff_path << "\n";
    }
    std::string audio_path;
    if (!st.reader_optimize && (st.reader_all || st.reader_no_tts)) {
        const std::string doc = pm::tts::reader::document_speak_text(store, err);
        if (doc.empty()) {
            std::cerr << "reader: " << (err.empty() ? "no speakable text" : err) << "\n";
            return 1;
        }
        const fs::path script = fs::path(store.dir) / "spoken.md";
        {
            std::ofstream out(script, std::ios::binary | std::ios::trunc);
            if (!out) {
                std::cerr << "reader: cannot write " << script.generic_string() << "\n";
                return 1;
            }
            out << doc;
            if (doc.back() != '\n')
                out << '\n';
        }
        spoken = script.generic_string();
        std::cout << spoken << "\n";
        if (st.reader_all && st.reader_audio.empty())
            audio_path = (fs::path(store.dir) / "document.mp3").generic_string();
#if defined(FEATURE_STT) && FEATURE_STT
        std::string speakable = doc;
        pm::tts::TtsTextInputResult text_in;
        std::string text_err;
        if (pm::tts::resolve_tts_text(spoken, text_in, text_err) && !text_in.text.empty())
            speakable = text_in.text;
        write_plan_for(fs::path(store.dir), speakable, st);
#endif
    }
    if (st.reader_no_speak || st.reader_no_tts || spoken.empty())
        return 0;
#if defined(FEATURE_STT) && FEATURE_STT
    if (speak_page(spoken, st, err, audio_path) != 0) {
        std::cerr << "reader: " << err << "\n";
        return 1;
    }
    return 0;
#else
    std::cerr << "reader: speech is not built\n";
    return 1;
#endif
}

void pm_image_register_reader(CLI::App& app, PmImageCliState& s) {
    s.reader_cmd = app.add_subcommand(
        "reader",
        "Pick a file or a web page and speak it. --all writes every page into one audio file.");
    s.reader_cmd->add_option("--interpreter", s.reader_interpreter,
                             "Override the file interpreter: text, anydoc, vision, image_understand, web. "
                             "vision renders each PDF page and reads it as a picture. "
                             "Word and PowerPoint have no page renderer yet.")
        ->check(CLI::IsMember({"text", "anydoc", "vision", "image_understand", "web"}))
        ->group("Input");
    s.reader_cmd->add_option("--pages", s.reader_pages,
                             "PDF pages for vision, 1-based. Example: 34-38. The default is every page.")
        ->group("Input");
    s.reader_cmd->add_option("--vision-prompt", s.reader_vision_prompt,
                             "Instruction sent with each page image. Empty infers it from --prompt.")
        ->group("Prompt");
    s.reader_cmd->add_option("--text", s.reader_text,
                             "Source file, an http(s) URL, or a sentence when the path is not a file. "
                             "The URL is the cache key. A page is read as DOM markdown.")
        ->group("Input");
    s.reader_cmd->add_option("--dst", s.reader_dst,
                             "Reader root. Default is the config directory / reader. "
                             "The job is <root>/<path hash>/.")
        ->group("Output");
    s.reader_cmd->add_option("--prompt", s.reader_prompt,
                             "Rewrite the current page. The original page stays. "
                             "Example: summarize, in german.")
        ->group("Prompt");
    s.reader_cmd->add_option("--chat-preset", s.reader_chat_preset,
                             "Chat preset name or id for the rewrite. "
                             "Uses that preset's router and model. Requires --prompt.")
        ->group("Prompt");
    s.reader_cmd->add_option("--direction", s.reader_direction,
                             "Same as --prompt.")
        ->group("Prompt");
    s.reader_cmd->add_option("--language", s.reader_language,
                             "Spoken language recorded on the job.")
        ->group("Input");
    s.reader_cmd->add_option("--page", s.reader_page,
                             "1-based page to select. 0 keeps the stored page.")
        ->group("Input");
    s.reader_cmd->add_flag("--next", s.reader_next,
                           "Advance one page. Stops at the last page, and after --stop.")
        ->group("Input");
    s.reader_cmd->add_flag("--stop", s.reader_stop, "Stop advancing.")
        ->group("Input");
    s.reader_cmd->add_flag("--resume", s.reader_resume, "Allow --next again.")
        ->group("Input");
    s.reader_cmd->add_flag("--refresh", s.reader_refresh,
                           "Convert again and replace the page files. "
                           "Rewrites from the previous source stay on disk and are not spoken.")
        ->group("Output");
    s.reader_cmd->add_flag("--clean", s.reader_clean,
                           "Erase this document's job directory and exit.")
        ->group("Output");
    s.reader_cmd->add_option("--provider", s.reader_provider,
                             "Voice proxy. Default is the chat voice provider, then tanit.")
        ->group("Speak");
    s.reader_cmd->add_option("--model-id", s.reader_model,
                             "Speech model. Default is the chat voice model, then tanit-speech-v4.")
        ->group("Speak");
    s.reader_cmd->add_option("--voice-id", s.reader_voice,
                             "ElevenLabs voice id. Empty uses the chat voice, then Sarah "
                             "EXAVITQu4vr4xnSDxMaL.")
        ->group("Speak");
    s.reader_cmd->add_flag("--no-chunk", s.reader_no_chunk,
                           "Send the page as one request. Long pages otherwise split on sentences "
                           "and stitch with previous and next text.")
        ->group("Speak");
    s.reader_cmd->add_option("--chunk-target-chars", s.reader_chunk_target,
                             "Merge sentences up to this many characters (100–800, default 400).")
        ->default_val(400)
        ->check(CLI::Range(100, 800))
        ->group("Speak");
    s.reader_cmd->add_option("--chunk-max-chars", s.reader_chunk_max,
                             "Split one sentence that is longer than this (200–2000, default 800).")
        ->default_val(800)
        ->check(CLI::Range(200, 2000))
        ->group("Speak");
    s.reader_cmd->add_flag("--all", s.reader_all,
                           "Speak every page as one file. Long text is chunked and joined. "
                           "Default path is document.mp3, or document.wav when more than one chunk.")
        ->group("Speak");
    s.reader_cmd->add_option("--audio", s.reader_audio,
                             "Audio file to write. Default is page-N.mp3, or document.mp3 with --all.")
        ->group("Speak");
    s.reader_cmd->add_flag("--no-play", s.reader_no_play, "Write the audio file and do not play it.")
        ->group("Speak");
    s.reader_cmd->add_flag("--optimize", s.reader_optimize,
                           "Optional review of the derived narration. "
                           "Writes revisions/<preset>/ and rebuilds spoken.md. "
                           "Harvested pages stay. Default preset is reader-accessible-child.")
        ->group("Prompt");
    s.reader_cmd->add_option("--optimize-preset", s.reader_optimize_preset,
                             "Optimize preset: reader-review, reader-accessible-child, "
                             "reader-vision-verify, reader-narrative, reader-faithful. "
                             "Empty uses reader-accessible-child. "
                             "reader-vision-verify reads each page image once.")
        ->group("Prompt");
    s.reader_cmd->add_flag("--no-tts", s.reader_no_tts,
                           "Write spoken.md and do not call the voice proxy. "
                           "Vision still reads each page.")
        ->group("Speak");
    s.reader_cmd->add_flag("--no-speak", s.reader_no_speak,
                           "Cache the pages and do not call the voice proxy. "
                           "With vision, render the pages and record the prompts, and do not call the vision model. "
                           "Use --no-tts to read the pages and skip only the voice.")
        ->group("Speak");
    s.reader_cmd->add_flag("--llm", s.reader_llm,
                           "Require --prompt. One text turn, no tools, on the current page.")
        ->group("Prompt");
    s.reader_cmd->add_flag("--print-preset", s.reader_print_preset,
                           "Print the fast preset clamp and exit.")
        ->group("LLM");
    s.reader_cmd->add_flag("--print-interpreter", s.reader_print_interpreter,
                           "Print the interpreter for --text and exit.")
        ->group("LLM");
    s.reader_cmd->add_flag("--print-estimate", s.reader_print_estimate,
                           "Print the remaining-time line for --done, --total, and --elapsed-ms, then exit.")
        ->group("LLM");
    s.reader_cmd->add_option("--done", s.reader_done,
                             "Characters already finished. Used by --print-estimate.")
        ->group("LLM");
    s.reader_cmd->add_option("--total", s.reader_total,
                             "Characters in the whole speak. Used by --print-estimate.")
        ->group("LLM");
    s.reader_cmd->add_option("--elapsed-ms", s.reader_elapsed_ms,
                             "Milliseconds already spent. Used by --print-estimate.")
        ->group("LLM");
}

#else

int pm_image_cmd_reader(CLI::App&, PmImageCliState&) { return 0; }
void pm_image_register_reader(CLI::App&, PmImageCliState&) {}

#endif
