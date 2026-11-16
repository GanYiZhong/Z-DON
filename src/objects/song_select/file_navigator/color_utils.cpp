#include "color_utils.h"

#include <cmath>

Color darken_color(const Color& rgb) {
    constexpr float darkening_factor = 0.63f;
    return Color(static_cast<unsigned char>(std::round(rgb.r * darkening_factor)),
                 static_cast<unsigned char>(std::round(rgb.g * darkening_factor)),
                 static_cast<unsigned char>(std::round(rgb.b * darkening_factor)), rgb.a);
}

BoxColors resolve_box_colors(const std::vector<Color>& box_color, const std::vector<Color>& back_color,
                             const std::optional<Color>& fore_color) {
    BoxColors result;
    result.box = !box_color.empty() ? box_color : back_color;
    Color default_outline = !result.box.empty() ? darken_color(result.box.front()) : Color(101, 0, 82, 255);
    if (!box_color.empty()) {
        result.outline = fore_color.value_or(default_outline);
        result.text = fore_color.value_or(Color::WHITE);
    } else {
        result.outline = fore_color.value_or(default_outline);
        result.text = Color::WHITE;
    }
    return result;
}

// Alpha is spelled out on every colour: Color is a plain aggregate, so a
// three-value initialisation quietly leaves alpha at 0 and anything drawn with
// it - a text outline, say - is painted fully transparent.
const std::map<GenreIndex, std::array<std::optional<Color>, 2>> DEFAULT_COLORS = {
    {GenreIndex::JPOP, {Color(32, 160, 186, 255), Color(0, 77, 104, 255)}},
    {GenreIndex::ANIME, {Color(255, 152, 0, 255), Color(156, 64, 2, 255)}},
    {GenreIndex::VOCALOID, {Color(222, 235, 245, 255), Color(84, 101, 126, 255)}},
    {GenreIndex::CHILDREN, {Color(255, 82, 134, 255), Color(153, 4, 46, 255)}},
    {GenreIndex::VARIETY, {Color(142, 212, 30, 255), Color(60, 104, 0, 255)}},
    {GenreIndex::CLASSICAL, {Color(209, 162, 19, 255), Color(134, 88, 0, 255)}},
    {GenreIndex::GAME, {Color(156, 117, 189, 255), Color(79, 40, 134, 255)}},
    {GenreIndex::NAMCO, {Color(255, 90, 19, 255), Color(148, 24, 0, 255)}},
    {GenreIndex::DEFAULT, {std::nullopt, Color(101, 0, 82, 255)}},
    {GenreIndex::RECOMMENDED, {std::nullopt, Color(140, 39, 92, 255)}},
    {GenreIndex::FAVORITE, {std::nullopt, Color(151, 57, 30, 255)}},
    {GenreIndex::RECENT, {std::nullopt, Color(35, 123, 103, 255)}},
    {GenreIndex::DAN, {Color(35, 102, 170, 255), Color(25, 68, 137, 255)}},
    {GenreIndex::DIFFICULTY, {Color(255, 85, 95, 255), Color(157, 13, 31, 255)}},
};
