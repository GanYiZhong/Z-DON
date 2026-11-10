#include "zdon_online.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "gfx/color_utils.h"

ZdonOnline zdon_online;

void ZdonOnline::read_modifiers(const std::string& json) {
    rapidjson::Document doc;
    if (json.empty() || doc.Parse(json.c_str()).HasParseError() || !doc.IsObject()) {
        return;
    }
    if (doc.HasMember("inverse") && doc["inverse"].IsBool()) {
        inverse = doc["inverse"].GetBool();
    }
    if (doc.HasMember("random") && doc["random"].IsInt()) {
        random = doc["random"].GetInt();
    }
    if (doc.HasMember("seed") && doc["seed"].IsUint()) {
        seed = doc["seed"].GetUint();
    }
    if (doc.HasMember("subdiff") && doc["subdiff"].IsInt()) {
        subdiff = doc["subdiff"].GetInt();
    }
}

void ZdonOnline::start_round(int new_round) {
    mode = Mode::LIVE;
    round = new_round;
    sent = received = 0;
    incoming.clear();
    opp_pos = -1e18;
    opp_pos_at = 0.0;
    opp_t0 = 0.0;
    opp_left = false;
    go = false;
    opp_final.reset();
}

PlayerData zdon_online_profile(const std::string& json_object) {
    PlayerData pd;
    rapidjson::Document doc;
    if (doc.Parse(json_object.c_str()).HasParseError() || !doc.IsObject()) {
        return pd;
    }
    auto str = [&](const char* k) -> std::string {
        return doc.HasMember(k) && doc[k].IsString() ? doc[k].GetString() : "";
    };
    auto num = [&](const char* k, int fallback) {
        return doc.HasMember(k) && doc[k].IsInt() ? doc[k].GetInt() : fallback;
    };
    pd.username = str("username");
    pd.title = str("title");
    pd.title_bg = num("title_bg", 0);
    try {
        if (!str("chara_color_1").empty()) {
            pd.chara_color_1 = parse_hex_color(str("chara_color_1"));
        }
        if (!str("chara_color_2").empty()) {
            pd.chara_color_2 = parse_hex_color(str("chara_color_2"));
        }
        if (!str("chara_color_3").empty()) {
            pd.chara_color_3 = parse_hex_color(str("chara_color_3"));
        }
    } catch (const std::invalid_argument&) {
    }
    pd.chara_head_index = num("chara_head_index", pd.chara_head_index);
    pd.chara_body_index = num("chara_body_index", pd.chara_body_index);
    pd.chara_cos_index = num("chara_cos_index", pd.chara_cos_index);
    if (doc.HasMember("chara_is_costume") && doc["chara_is_costume"].IsBool()) {
        pd.chara_is_costume = doc["chara_is_costume"].GetBool();
    }
    return pd;
}
