#include "zdon_online.h"

#include <rapidjson/document.h>

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
