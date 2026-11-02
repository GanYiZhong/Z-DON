#pragma once

#include <string>
#include <utility>

#include "../../libs/file/script.h"

// The fail / clear / full-combo / donderful ending animations differ only in the Lua class and
// script they load (and the extra "donderful" argument of the full-combo one).
class EndingAnimation : public LuaScript {
private:
    sol::protected_function fn_update, fn_draw;
    std::string update_context, draw_context;

public:
    template<typename... Args>
    EndingAnimation(const std::string& class_name, const std::string& script_name, Args&&... args)
        : update_context(class_name + ":update"), draw_context(class_name + ":draw") {
        if (!load(class_name, script_name, std::forward<Args>(args)...)) {
            return;
        }
        fn_update = lua_object["update"];
        fn_draw = lua_object["draw"];
    }

    void update(double current_ms) { call(fn_update, update_context, current_ms); }
    void draw() { call(fn_draw, draw_context); }
};
