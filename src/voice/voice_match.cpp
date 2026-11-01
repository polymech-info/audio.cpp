#include "pm/audio/voice/voice_match.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

namespace pm::audio::voice {
namespace {

bool utf8_next(std::string_view& in, std::uint32_t& cp)
{
    if (in.empty())
        return false;
    const auto b0 = static_cast<unsigned char>(in[0]);
    auto take = [&](std::size_t n, std::uint32_t value) {
        in.remove_prefix(std::min(n, in.size()));
        cp = value;
        return true;
    };
    if (b0 < 0x80)
        return take(1, b0);
    if ((b0 & 0xE0) == 0xC0 && in.size() >= 2)
        return take(2, (static_cast<std::uint32_t>(b0 & 0x1F) << 6)
            | (static_cast<unsigned char>(in[1]) & 0x3F));
    if ((b0 & 0xF0) == 0xE0 && in.size() >= 3)
        return take(3, (static_cast<std::uint32_t>(b0 & 0x0F) << 12)
            | ((static_cast<unsigned char>(in[1]) & 0x3F) << 6)
            | (static_cast<unsigned char>(in[2]) & 0x3F));
    if ((b0 & 0xF8) == 0xF0 && in.size() >= 4)
        return take(4, (static_cast<std::uint32_t>(b0 & 0x07) << 18)
            | ((static_cast<unsigned char>(in[1]) & 0x3F) << 12)
            | ((static_cast<unsigned char>(in[2]) & 0x3F) << 6)
            | (static_cast<unsigned char>(in[3]) & 0x3F));
    return take(1, b0);
}

bool is_voice_punct(std::uint32_t cp)
{
    if (cp < 0x80)
        return std::isspace(static_cast<int>(cp)) || std::ispunct(static_cast<int>(cp));
    if (cp == 0x00A0 || cp == 0x00AD || cp == 0xFEFF)
        return true;
    if (cp >= 0x00A1 && cp <= 0x00BF)
        return true;
    if (cp >= 0x2000 && cp <= 0x206F)
        return true;
    if (cp >= 0x2E00 && cp <= 0x2E7F)
        return true;
    if (cp >= 0x3000 && cp <= 0x303F)
        return true;
    if ((cp >= 0xFF00 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20))
        return true;
    return false;
}

std::string drop_whisper_artifacts(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    int square = 0;
    int paren = 0;
    for (const unsigned char ch : text) {
        if (ch == '[') {
            ++square;
            continue;
        }
        if (ch == ']' && square > 0) {
            --square;
            continue;
        }
        if (ch == '(') {
            ++paren;
            continue;
        }
        if (ch == ')' && paren > 0) {
            --paren;
            continue;
        }
        if (square == 0 && paren == 0)
            out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool word_in(std::string_view word, std::initializer_list<const char*> words)
{
    return std::any_of(words.begin(), words.end(), [&](const char* item) {
        return word == item;
    });
}

bool is_filler_word(std::string_view word)
{
    return word_in(word, {
        "um", "uh", "ah", "er", "hmm", "mhm", "mm",
        "comma", "period", "ellipsis", "dot",
    });
}

bool is_lead_in_word(std::string_view word)
{
    return word_in(word, {"okay", "ok", "hey", "yes", "yeah", "please", "well", "so"});
}

std::vector<std::string> tokenize_words(std::string_view text)
{
    std::vector<std::string> words;
    std::string current;
    auto flush = [&] {
        if (current.empty())
            return;
        if (!is_filler_word(current))
            words.push_back(std::move(current));
        current.clear();
    };
    for (const char ch : text) {
        if (ch == ' ')
            flush();
        else
            current.push_back(ch);
    }
    flush();
    return words;
}

std::string join_words(const std::vector<std::string>& words)
{
    std::string out;
    for (const auto& word : words) {
        if (!out.empty())
            out.push_back(' ');
        out += word;
    }
    return out;
}

void strip_lead_in_and_tail(std::vector<std::string>& words)
{
    while (!words.empty() && is_lead_in_word(words.front()))
        words.erase(words.begin());
    while (!words.empty() && (words.back() == "please" || words.back() == "thanks"))
        words.pop_back();
    if (words.size() >= 2 && words[words.size() - 2] == "thank" && words.back() == "you") {
        words.pop_back();
        words.pop_back();
    }
}

bool phrase_is(std::string_view value, const std::vector<std::string>& phrases)
{
    return std::any_of(phrases.begin(), phrases.end(), [&](const std::string& phrase) {
        return value == normalize_voice_text(phrase);
    });
}

bool has_prefix_word(std::string_view text, std::string_view prefix)
{
    return text == prefix ||
           (text.size() > prefix.size() &&
            text.compare(0, prefix.size(), prefix) == 0 &&
            text[prefix.size()] == ' ');
}

} // namespace

void apply_wake_phrase(MatchConfig& config, std::string_view wake)
{
    if (normalize_voice_text(wake).empty()) {
        config.wake_prefixes.clear();
        config.wake_required = false;
        return;
    }
    config.wake_prefixes = {std::string(wake)};
    config.wake_required = true;
}

std::string normalize_voice_text(std::string_view text)
{
    const std::string stripped = drop_whisper_artifacts(text);
    std::string folded;
    folded.reserve(stripped.size());
    bool pending_space = false;
    std::string_view cursor = stripped;
    std::uint32_t cp = 0;
    while (utf8_next(cursor, cp)) {
        if (is_voice_punct(cp)) {
            pending_space = !folded.empty();
            continue;
        }
        if (pending_space) {
            folded.push_back(' ');
            pending_space = false;
        }
        if (cp < 0x80)
            folded.push_back(static_cast<char>(std::tolower(static_cast<int>(cp))));
        else if (cp <= 0x7FF) {
            folded.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            folded.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            folded.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            folded.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            folded.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            folded.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            folded.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            folded.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            folded.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    return join_words(tokenize_words(folded));
}

MatchResult match_voice_text(std::string_view text, const MatchConfig& config)
{
    MatchResult result;
    result.normalized_text = normalize_voice_text(text);
    result.command_text = result.normalized_text;
    if (result.normalized_text.empty())
        return result;

    auto words = tokenize_words(result.normalized_text);
    std::size_t lead = 0;
    while (lead < words.size() && is_lead_in_word(words[lead]))
        ++lead;

    std::string best_prefix;
    std::size_t best_start = 0;
    for (std::size_t start = 0; start <= lead && start < words.size(); ++start) {
        const std::string from_here = join_words(
            std::vector<std::string>(words.begin() + static_cast<std::ptrdiff_t>(start), words.end()));
        for (const std::string& candidate : config.wake_prefixes) {
            const std::string normalized = normalize_voice_text(candidate);
            if (normalized.size() > best_prefix.size() &&
                has_prefix_word(from_here, normalized)) {
                best_prefix = normalized;
                best_start = start;
            }
        }
    }
    if (!best_prefix.empty()) {
        result.wake_prefix = best_prefix;
        result.normalized_text = join_words(
            std::vector<std::string>(words.begin() + static_cast<std::ptrdiff_t>(best_start), words.end()));
        result.command_text = result.normalized_text;
        result.command_text.erase(0, best_prefix.size());
        if (!result.command_text.empty() && result.command_text.front() == ' ')
            result.command_text.erase(0, 1);
        auto command_words = tokenize_words(result.command_text);
        // "Sonic's screenshot" folds to "sonic s screenshot" after apostrophe drop.
        if (!command_words.empty() && command_words.front() == "s")
            command_words.erase(command_words.begin());
        strip_lead_in_and_tail(command_words);
        result.command_text = join_words(command_words);
    } else if (!config.wake_required) {
        auto command_words = tokenize_words(result.normalized_text);
        strip_lead_in_and_tail(command_words);
        result.command_text = join_words(command_words);
    }

    const std::string_view intent_text =
        result.command_text.empty() ? std::string_view(result.normalized_text)
                                    : std::string_view(result.command_text);
    if (phrase_is(intent_text, config.cancel_phrases)) {
        result.control = ControlIntent::cancel;
        result.matched = true;
        return result;
    }
    if (phrase_is(intent_text, config.stop_phrases)) {
        result.control = ControlIntent::stop_listening;
        result.matched = true;
        return result;
    }

    result.matched = !result.command_text.empty() &&
                     (!config.wake_required || !result.wake_prefix.empty());
    return result;
}

} // namespace pm::audio::voice
