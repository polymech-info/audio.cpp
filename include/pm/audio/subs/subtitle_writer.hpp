#pragma once
//
// subtitle_writer.hpp — SRT / WebVTT / SubViewer (YouTube-accepted) sidecars.
// Cue wrap lives here (subtitleMaxChars / subtitleMaxLines), not on video.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pm::subs {

struct Cue {
    std::string text;
    /// Encoded-timeline milliseconds.
    std::int64_t t0_ms = 0;
    std::int64_t t1_ms = 0;
    /// Whisper segment that produced this word, or -1 when the caller
    /// has no segments. Packing uses it as a phrase boundary. Writers ignore it.
    int segment_id = -1;
};

inline constexpr int k_min_cue_chars = 8;
inline constexpr int k_max_cue_chars = 40000;

struct WriterOptions {
    int max_chars = 42;
    int max_lines = 2;
    /// End time for a transcript that has no cue times. Callers that know
    /// the media length set this so the single cue covers the file.
    std::int64_t untimed_end_ms = 1000;
};

/// Grow packing. max_chars / max_lines are the layout bound. max_visible_words
/// is how many words stay on screen. Timing is final here. Callers do not
/// adjust cue ends again.
struct GrowOptions {
    int max_chars = 42;
    int max_lines = 2;
    int max_visible_words = 8;
    int phrase_break_ms = 2500;
    /// Segment change plus a pause at least this long closes the phrase.
    int segment_pause_ms = 400;
    /// Gap between two updates of the same phrase. 1 ms is under one frame.
    int update_gap_ms = 1;
    /// Blank between the last update of a phrase and the next phrase.
    int phrase_gap_ms = 40;
    /// Hold after the last word of a phrase, then capped at the next phrase.
    int final_hold_ms = 200;
};

enum class Format {
    srt,
    vtt,
    sbv,
};

/// Whisper centiseconds (t0/t1 from whisper_full_get_segment_t*) → ms.
inline std::int64_t whisper_cs_to_ms(std::int64_t cs) noexcept
{
    return cs * 10;
}

/// Word-wrap to @p opt.max_chars. A cue keeps its original times. Overflow
/// lines stay on that cue; wrapping does not invent a new timeline.
std::vector<std::string> wrap_cue_text(
    const std::string& text, const WriterOptions& opt);

/// Build pop-on cues from word times. A word is visible only once its
/// speech has started. Words within one Whisper tick (10 ms) share one update.
/// The visible tail rolls at max_visible_words and at the layout bound.
/// Same-phrase updates meet at update_gap_ms. A sentence end, a segment
/// change with a pause, or dead air over phrase_break_ms starts a new line.
/// The int overload uses GrowOptions defaults and the given line width.
std::vector<Cue> pack_word_cues(const std::vector<Cue>& words,
                                const GrowOptions& opt,
                                const std::string& text_fallback = {});

std::vector<Cue> pack_word_cues(const std::vector<Cue>& words,
                                int max_chars = 42,
                                const std::string& text_fallback = {});

bool write_srt(const std::filesystem::path& path,
               const std::vector<Cue>& cues,
               const WriterOptions& opt,
               std::string& err);

bool write_vtt(const std::filesystem::path& path,
               const std::vector<Cue>& cues,
               const WriterOptions& opt,
               std::string& err);

bool write_sbv(const std::filesystem::path& path,
               const std::vector<Cue>& cues,
               const WriterOptions& opt,
               std::string& err);

bool write_format(Format fmt,
                  const std::filesystem::path& path,
                  const std::vector<Cue>& cues,
                  const WriterOptions& opt,
                  std::string& err);

bool read_srt(const std::filesystem::path& path,
              std::vector<Cue>& cues,
              std::string& err);

bool read_vtt(const std::filesystem::path& path,
              std::vector<Cue>& cues,
              std::string& err);

/// `.srt` / `.vtt` by extension. Other extensions try SRT first, then VTT.
bool read_cues_file(const std::filesystem::path& path,
                    std::vector<Cue>& cues,
                    std::string& err);

/// `srt` | `vtt` | `sbv` | `all`. Empty / none / off → no files.
std::vector<Format> parse_format_list(std::string token);

const char* format_ext(Format fmt) noexcept;

/// Write YouTube-accepted sidecar(s) next to @p stem. Audio/video and
/// subtitle extensions are stripped first; any other extension is kept
/// (`clip.mp3` → `clip.srt`, `name.debug` → `name.debug.srt`).
/// Empty / none / off → no files, success. No cues and empty transcript →
/// no files, success. No cues but non-empty transcript → one cue from 0 to
/// WriterOptions::untimed_end_ms.
bool write_sidecars(std::string format_token,
                    std::filesystem::path stem,
                    const std::string& transcript,
                    std::vector<Cue> cues,
                    WriterOptions opt,
                    std::vector<std::filesystem::path>& written,
                    std::string& err);

} // namespace pm::subs
