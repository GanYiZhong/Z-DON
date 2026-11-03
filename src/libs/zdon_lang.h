#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

// Z-DON: zh is Traditional Chinese (upstream YataiDON's zh is Simplified). Charts and box.def files often carry
// both, e.g. TITLEZH (Simplified) and TITLEZH_TW (Traditional).

// How well a line's language tag (the part between the keyword and the colon: "ZH_TW" of TITLEZH_TW, any case)
// fits the interface language: 0 = another language; for zh, ZH_TW / TW rank above ZH / CN
inline int zdon_lang_tag_rank(std::string_view tag, std::string_view lang) {
    std::string t(tag), l(lang);
    for (char& c : t) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for (char& c : l) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (l == "ZH") {
        if (t == "ZH_TW" || t == "ZH-TW" || t == "TW") {
            return 2;
        }
        return t == "ZH" || t == "CN" || t == "ZH_CN" || t == "ZH-CN" ? 1 : 0;
    }
    return t == l ? 1 : 0;
}

// A chart's TITLE / SUBTITLE tag as the metadata key it is stored under, and its rank there: the Chinese tags
// all go to "zh", Traditional ranking above Simplified; any other tag keeps its first two letters, lower case
inline std::pair<std::string, int> zdon_lang_region(std::string_view tag) {
    const int zh = zdon_lang_tag_rank(tag, "zh");
    if (zh > 0) {
        return {"zh", zh};
    }
    std::string code(tag.substr(0, 2));
    for (char& c : code) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return {code, 1};
}
