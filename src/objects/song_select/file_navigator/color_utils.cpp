#include "color_utils.h"

#include <cmath>

ray::Color darken_color(const ray::Color& rgb) {
    constexpr float darkening_factor = 0.63f;
    return ray::Color((unsigned char)std::round(rgb.r * darkening_factor),
                      (unsigned char)std::round(rgb.g * darkening_factor),
                      (unsigned char)std::round(rgb.b * darkening_factor), rgb.a);
}

BoxColors resolve_box_colors(const std::optional<ray::Color>& box_color, const std::optional<ray::Color>& back_color,
                             const std::optional<ray::Color>& fore_color) {
    BoxColors result;
    result.box = box_color.has_value() ? box_color : back_color;
    ray::Color default_outline =
        result.box.has_value() ? darken_color(result.box.value()) : ray::Color(101, 0, 82, 255);
    result.outline = fore_color.value_or(default_outline);
    result.text = box_color.has_value() ? fore_color.value_or(ray::WHITE) : ray::WHITE;
    return result;
}

// Alpha is spelled out on every colour: ray::Color is a plain aggregate, so a
// three-value initialisation quietly leaves alpha at 0 and anything drawn with
// it - a text outline, say - is painted fully transparent.
const std::map<GenreIndex, std::array<std::optional<ray::Color>, 2>> DEFAULT_COLORS = {
    {GenreIndex::JPOP, {ray::Color(32, 160, 186, 255), ray::Color(0, 77, 104, 255)}},
    {GenreIndex::ANIME, {ray::Color(255, 152, 0, 255), ray::Color(156, 64, 2, 255)}},
    {GenreIndex::VOCALOID, {std::nullopt, ray::Color(84, 101, 126, 255)}},
    {GenreIndex::CHILDREN, {ray::Color(255, 82, 134, 255), ray::Color(153, 4, 46, 255)}},
    {GenreIndex::VARIETY, {ray::Color(142, 212, 30, 255), ray::Color(60, 104, 0, 255)}},
    {GenreIndex::CLASSICAL, {ray::Color(209, 162, 19, 255), ray::Color(134, 88, 0, 255)}},
    {GenreIndex::GAME, {ray::Color(156, 117, 189, 255), ray::Color(79, 40, 134, 255)}},
    {GenreIndex::NAMCO, {ray::Color(255, 90, 19, 255), ray::Color(148, 24, 0, 255)}},
    {GenreIndex::DEFAULT, {std::nullopt, ray::Color(101, 0, 82, 255)}},
    {GenreIndex::RECOMMENDED, {std::nullopt, ray::Color(140, 39, 92, 255)}},
    {GenreIndex::FAVORITE, {std::nullopt, ray::Color(151, 57, 30, 255)}},
    {GenreIndex::RECENT, {std::nullopt, ray::Color(35, 123, 103, 255)}},
    {GenreIndex::DAN, {ray::Color(35, 102, 170, 255), ray::Color(25, 68, 137, 255)}},
    {GenreIndex::DIFFICULTY, {ray::Color(255, 85, 95, 255), ray::Color(157, 13, 31, 255)}},
};
