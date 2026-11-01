#include "pm/audio/stt/whisper.hpp"

#include "pm/audio/log.hpp"

namespace pm::stt {

std::string whisper_normalize_language(std::string_view language)
{
    std::string raw;
    raw.reserve(language.size());
    for (unsigned char ch : language) {
        if (ch == '_' || ch == ' ')
            raw.push_back('-');
        else
            raw.push_back(static_cast<char>(std::tolower(ch)));
    }
    while (!raw.empty() && (raw.back() == '-' || raw.back() == '.'))
        raw.pop_back();
    if (raw.empty() || raw == "auto" || raw == "detect" || raw == "none")
        return "auto";

    static const char* const k_code_name[][2] = {
        {"en", "english"}, {"zh", "chinese"}, {"de", "german"}, {"es", "spanish"},
        {"ru", "russian"}, {"ko", "korean"}, {"fr", "french"}, {"ja", "japanese"},
        {"pt", "portuguese"}, {"tr", "turkish"}, {"pl", "polish"}, {"ca", "catalan"},
        {"nl", "dutch"}, {"ar", "arabic"}, {"sv", "swedish"}, {"it", "italian"},
        {"id", "indonesian"}, {"hi", "hindi"}, {"fi", "finnish"}, {"vi", "vietnamese"},
        {"he", "hebrew"}, {"uk", "ukrainian"}, {"el", "greek"}, {"ms", "malay"},
        {"cs", "czech"}, {"ro", "romanian"}, {"da", "danish"}, {"hu", "hungarian"},
        {"ta", "tamil"}, {"no", "norwegian"}, {"th", "thai"}, {"ur", "urdu"},
        {"hr", "croatian"}, {"bg", "bulgarian"}, {"lt", "lithuanian"}, {"la", "latin"},
        {"mi", "maori"}, {"ml", "malayalam"}, {"cy", "welsh"}, {"sk", "slovak"},
        {"te", "telugu"}, {"fa", "persian"}, {"lv", "latvian"}, {"bn", "bengali"},
        {"sr", "serbian"}, {"az", "azerbaijani"}, {"sl", "slovenian"}, {"kn", "kannada"},
        {"et", "estonian"}, {"mk", "macedonian"}, {"br", "breton"}, {"eu", "basque"},
        {"is", "icelandic"}, {"hy", "armenian"}, {"ne", "nepali"}, {"mn", "mongolian"},
        {"bs", "bosnian"}, {"kk", "kazakh"}, {"sq", "albanian"}, {"sw", "swahili"},
        {"gl", "galician"}, {"mr", "marathi"}, {"pa", "punjabi"}, {"si", "sinhala"},
        {"km", "khmer"}, {"sn", "shona"}, {"yo", "yoruba"}, {"so", "somali"},
        {"af", "afrikaans"}, {"oc", "occitan"}, {"ka", "georgian"}, {"be", "belarusian"},
        {"tg", "tajik"}, {"sd", "sindhi"}, {"gu", "gujarati"}, {"am", "amharic"},
        {"yi", "yiddish"}, {"lo", "lao"}, {"uz", "uzbek"}, {"fo", "faroese"},
        {"ht", "haitian creole"}, {"ps", "pashto"}, {"tk", "turkmen"}, {"nn", "nynorsk"},
        {"mt", "maltese"}, {"sa", "sanskrit"}, {"lb", "luxembourgish"}, {"my", "myanmar"},
        {"bo", "tibetan"}, {"tl", "tagalog"}, {"mg", "malagasy"}, {"as", "assamese"},
        {"tt", "tatar"}, {"haw", "hawaiian"}, {"ln", "lingala"}, {"ha", "hausa"},
        {"ba", "bashkir"}, {"jw", "javanese"}, {"su", "sundanese"}, {"yue", "cantonese"},
    };
    static const char* const k_alias[][2] = {
        {"deutsch", "de"}, {"ger", "de"}, {"deu", "de"},
        {"francais", "fr"}, {"français", "fr"}, {"fra", "fr"},
        {"espanol", "es"}, {"español", "es"}, {"spa", "es"},
        {"italiano", "it"}, {"ita", "it"},
        {"portugues", "pt"}, {"português", "pt"}, {"por", "pt"},
        {"nederlands", "nl"}, {"nld", "nl"},
        {"polski", "pl"}, {"pol", "pl"},
        {"russki", "ru"}, {"rus", "ru"},
        {"zhongwen", "zh"}, {"cmn", "zh"}, {"chi", "zh"},
        {"nihongo", "ja"}, {"jpn", "ja"},
        {"hangul", "ko"}, {"kor", "ko"},
        {"eng", "en"}, {"us", "en"}, {"gb", "en"},
        {"hebrew", "he"}, {"iw", "he"},
        {"farsi", "fa"}, {"filipino", "tl"},
        {"cantonese", "yue"}, {"haitian", "ht"}, {"creole", "ht"},
    };

    for (const auto& row : k_code_name) {
        if (raw == row[0] || raw == row[1])
            return row[0];
    }
    for (const auto& row : k_alias) {
        if (raw == row[0])
            return row[1];
    }

    const auto dash = raw.find('-');
    if (dash != std::string::npos && dash > 0) {
        const std::string head = raw.substr(0, dash);
        for (const auto& row : k_code_name) {
            if (head == row[0])
                return row[0];
        }
    }

    pm::audio::log::warn("[whisper] unknown language \"" + raw + "\"; using auto");
    return "auto";
}

} // namespace pm::stt
