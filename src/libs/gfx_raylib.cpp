#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_video.h>
#include <bit>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <format>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

#include "gfx.h"
#include "gfx/mesh_bounds.h"
#include "gfx/sdl_compat.h" // before raylib.h: raylib.h must not be the first to pull in <stdarg.h>

namespace ray {
    extern "C" {
#include <raylib.h>
// raylib color macros clash with Color::WHITE etc.
#undef WHITE
#undef BLACK
#undef BLANK
#undef BLUE
#undef GRAY
#undef GREEN
#undef LIGHTGRAY
#undef LIME
#undef ORANGE
#undef RED
#undef SKYBLUE
#undef YELLOW
#include <rlgl.h>
    }
} // namespace ray

// Project data types share raylib's layout; convert by bit copy / pointer cast.
template<class To, class From>
static To to(const From& v) {
    static_assert(sizeof(To) == sizeof(From));
    return std::bit_cast<To>(v);
}
template<class To, class From>
static To* to_ptr(From* p) {
    static_assert(sizeof(To) == sizeof(From));
    return reinterpret_cast<To*>(p);
}
template<class To, class From>
static const To* to_ptr(const From* p) {
    static_assert(sizeof(To) == sizeof(From));
    return reinterpret_cast<const To*>(p);
}

#define SAME_OFFSET(T, f) static_assert(offsetof(T, f) == offsetof(ray::T, f))
SAME_OFFSET(Image, format);
static_assert(pixel_format::COMPRESSED_BPTC_RGBA == ray::PIXELFORMAT_COMPRESSED_BPTC_RGBA); // Z-DON
SAME_OFFSET(GlyphInfo, image);
SAME_OFFSET(Font, glyphs);
SAME_OFFSET(Transform, scale);
SAME_OFFSET(BoneInfo, parent);
SAME_OFFSET(Mesh, boneCount);
SAME_OFFSET(Mesh, vboId);
SAME_OFFSET(MaterialMap, value);
SAME_OFFSET(Material, params);
SAME_OFFSET(ModelSkeleton, bindPose);
SAME_OFFSET(Model, skeleton);
SAME_OFFSET(Model, boneMatrices);
SAME_OFFSET(ModelAnimation, keyframePoses);
static_assert(sizeof(Mesh) == sizeof(ray::Mesh) && sizeof(Model) == sizeof(ray::Model) &&
              sizeof(Material) == sizeof(ray::Material) && sizeof(ModelAnimation) == sizeof(ray::ModelAnimation));
#undef SAME_OFFSET

static_assert(window_flag::RESIZABLE == ray::FLAG_WINDOW_RESIZABLE);
static_assert(window_flag::VSYNC == ray::FLAG_VSYNC_HINT);
static_assert(window_flag::ALWAYS_RUN == ray::FLAG_WINDOW_ALWAYS_RUN);
static_assert(texture_filter::BILINEAR == ray::TEXTURE_FILTER_BILINEAR);
static_assert(texture_filter::TRILINEAR == ray::TEXTURE_FILTER_TRILINEAR);
static_assert(texture_wrap::CLAMP == ray::TEXTURE_WRAP_CLAMP);
static_assert(blend_mode::ADDITIVE == ray::BLEND_ADDITIVE);
static_assert(blend_mode::CUSTOM_SEPARATE == ray::BLEND_CUSTOM_SEPARATE);
static_assert(shader_uniform::FLOAT == ray::SHADER_UNIFORM_FLOAT);
static_assert(shader_uniform::VEC2 == ray::SHADER_UNIFORM_VEC2);
static_assert(shader_uniform::VEC3 == ray::SHADER_UNIFORM_VEC3);
static_assert(pixel_format::GRAYSCALE == ray::PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
static_assert(pixel_format::GRAY_ALPHA == ray::PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA);
static_assert(pixel_format::RGB8 == ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8);
static_assert(pixel_format::RGBA8 == ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
static_assert(material_map::DIFFUSE == ray::MATERIAL_MAP_DIFFUSE);
static_assert(cull_face::FRONT == ray::RL_CULL_FACE_FRONT);
static_assert(cull_face::BACK == ray::RL_CULL_FACE_BACK);

void draw_rectangle(int x, int y, int w, int h, Color color) {
    ray::DrawRectangle(x, y, w, h, {color.r, color.g, color.b, color.a});
}

void draw_rectangle(Rect rec, Color color) {
    ray::DrawRectangleRec(rec, {color.r, color.g, color.b, color.a});
}

void draw_rectangle_rounded(Rect rec, float roundness, int segments, Color color) {
    ray::DrawRectangleRounded(rec, roundness, segments, {color.r, color.g, color.b, color.a});
}

void draw_rectangle_rounded_lines_ex(Rect rec, float roundness, int segments, float thickness, Color color) {
    ray::DrawRectangleRoundedLinesEx(rec, roundness, segments, thickness, {color.r, color.g, color.b, color.a});
}

void draw_rectangle_lines(int x, int y, int w, int h, Color color) {
    ray::DrawRectangleLines(x, y, w, h, {color.r, color.g, color.b, color.a});
}

void draw_rectangle_lines_ex(Rect rec, float thickness, Color color) {
    ray::DrawRectangleLinesEx(rec, thickness, {color.r, color.g, color.b, color.a});
}

void draw_line(int x0, int y0, int x1, int y1, Color color) {
    ray::DrawLine(x0, y0, x1, y1, {color.r, color.g, color.b, color.a});
}

void draw_model(Model model, Vec3 pos, float scale, Color tint) {
    ray::DrawModel(to<ray::Model>(model), pos, scale, {tint.r, tint.g, tint.b, tint.a});
}

void draw_text_ex(Font font, const char* text, Vec2 pos, float font_size, float spacing, Color tint) {
    ray::DrawTextEx(to<ray::Font>(font), text, pos, font_size, spacing, {tint.r, tint.g, tint.b, tint.a});
}

void draw_texture_pro(Texture tex, Rect src, Rect dst, Vec2 origin, float rotation, Color tint) {
    ray::DrawTexturePro(tex, src, dst, origin, rotation, {tint.r, tint.g, tint.b, tint.a});
}

void draw_text(const char* text, int x, int y, int font_size, Color color) {
    ray::DrawText(text, x, y, font_size, {color.r, color.g, color.b, color.a});
}

int measure_text(const char* text, int font_size) {
    return ray::MeasureText(text, font_size);
}

void set_window_flags(unsigned flags) {
    ray::SetConfigFlags(flags);
}

void quiet_raylib_log() {
    ray::SetTraceLogLevel(ray::LOG_ERROR);
}

static SDL_Window* window = nullptr;

SDL_Window* main_window() {
    return window;
}

// Z-DON: the graphics card and its OpenGL in the log; one older than the 3.3 the renderer needs draws nothing (a
// black window), so the player is told why
static void zdon_check_opengl() {
    using GetString = const unsigned char* (*)(unsigned);
    auto get = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"));
    if (!get) {
        spdlog::error("OpenGL: no context");
        return;
    }
    auto str = [get](unsigned name) {
        const unsigned char* s = get(name);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("?");
    };
    const std::string vendor = str(0x1F00), renderer = str(0x1F01), version = str(0x1F02); // GL_VENDOR...
    spdlog::info("OpenGL: {} ({}), version {}", renderer, vendor, version);
    if (version.starts_with("OpenGL ES")) {
        return; // GLES builds (web: WebGL 2) render with ES 3.0, not desktop GL 3.3
    }
    int major = 0, minor = 0;
    if (std::sscanf(version.c_str(), "%d.%d", &major, &minor) == 2 && (major > 3 || (major == 3 && minor >= 3))) {
        return;
    }
    spdlog::error("OpenGL {} is older than the 3.3 this game needs: the screen stays black", version);
    const std::string text = std::format("顯示卡不支援 OpenGL 3.3，遊戲畫面會是黑的。\n"
                                         "請安裝或更新顯示卡驅動程式（目前：{}，OpenGL {}）。\n\n"
                                         "This graphics card's driver has no OpenGL 3.3, so the game draws nothing.\n"
                                         "Install or update the graphics driver (now: {}, OpenGL {}).",
                                         renderer, version, renderer, version);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "YataiDON", text.c_str(), window);
}

void init_window(int width, int height, const char* title) {
    ray::InitWindow(width, height, title);
    window = SDL_GL_GetCurrentWindow(); // raylib's SDL platform keeps its window private
    zdon_check_opengl();
}

void close_window() {
    ray::CloseWindow();
    window = nullptr;
}

void poll_window_events() {
    ray::PollInputEvents();
}

bool window_should_close() {
    return ray::WindowShouldClose();
}

void disable_exit_key() {
    ray::SetExitKey(ray::KEY_NULL);
}

void toggle_fullscreen() {
    ray::ToggleFullscreen();
}

// Z-DON: the window is in the composited borderless mode (gfx.h); the row below the monitor is not part of the screen
static bool composited_borderless = false;
static ray::Vector2 composited_saved_pos{};
static int composited_saved_w = 0;
static int composited_saved_h = 0;

void toggle_borderless_windowed() {
#ifdef _WIN32
    if (composited_borderless) {
        ray::ClearWindowState(ray::FLAG_WINDOW_UNDECORATED);
        ray::SetWindowSize(composited_saved_w, composited_saved_h);
        ray::SetWindowPosition(static_cast<int>(composited_saved_pos.x), static_cast<int>(composited_saved_pos.y));
        composited_borderless = false;
        return;
    }
    if (zdon_composited_fullscreen && !ray::IsWindowFullscreen() &&
        !ray::IsWindowState(ray::FLAG_BORDERLESS_WINDOWED_MODE)) {
        composited_saved_pos = ray::GetWindowPosition();
        composited_saved_w = ray::GetScreenWidth();
        composited_saved_h = ray::GetScreenHeight();
        const int monitor = ray::GetCurrentMonitor();
        const ray::Vector2 origin = ray::GetMonitorPosition(monitor);
        ray::SetWindowState(ray::FLAG_WINDOW_UNDECORATED);
        ray::SetWindowPosition(static_cast<int>(origin.x), static_cast<int>(origin.y));
        ray::SetWindowSize(ray::GetMonitorWidth(monitor), ray::GetMonitorHeight(monitor) + 1);
        composited_borderless = true;
        return;
    }
#endif
    ray::ToggleBorderlessWindowed();
}

bool is_window_focused() {
    return ray::IsWindowFocused();
}

bool is_window_fullscreen() {
    return ray::IsWindowFullscreen();
}

void set_window_size(int width, int height) {
    ray::SetWindowSize(width, height);
}

int get_screen_width() {
    return ray::GetScreenWidth();
}

int get_screen_height() {
    return ray::GetScreenHeight() - (composited_borderless ? 1 : 0);
}

static bool quads_3d_open = false; // between begin_quads_3d and end_quads_3d

void begin_quads_3d() {
    ray::rlDrawRenderBatchActive();
    ray::rlDisableDepthTest();
    ray::rlDisableBackfaceCulling();
    quads_3d_open = true;
}

void end_quads_3d() {
    ray::rlDrawRenderBatchActive();
    ray::rlEnableBackfaceCulling();
    ray::rlEnableDepthTest(); // BeginMode3D's state, which EndMode3D turns off again
    quads_3d_open = false;
}

bool draw_texture_quad_3d(Texture tex, const Vec3 corners[4], const Vec2 uvs[4], Color tint) {
    // alone: drawn now, without depth or face culling (state changes apply when the batch is drawn, not when queued);
    // between begin_quads_3d / end_quads_3d: added to the batch
    const bool alone = !quads_3d_open;
    if (alone) {
        begin_quads_3d();
    }
    ray::rlCheckRenderBatchLimit(4);
    ray::rlSetTexture(tex.id);
    ray::rlBegin(RL_QUADS);
    ray::rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    ray::rlNormal3f(0.0f, 0.0f, 1.0f);
    // raylib quads go counter-clockwise: top-left, bottom-left, bottom-right, top-right
    for (const int i : {0, 3, 2, 1}) {
        ray::rlTexCoord2f(uvs[i].x, uvs[i].y);
        ray::rlVertex3f(corners[i].x, corners[i].y, corners[i].z);
    }
    ray::rlEnd();
    ray::rlSetTexture(0);
    if (alone) {
        end_quads_3d();
    }
    return true;
}

namespace {
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
    constexpr const char* kSilhouetteHeader = "#version 300 es\nprecision highp float;\n";
#else
    constexpr const char* kSilhouetteHeader = "#version 330\n";
#endif
    // the texture's shape in the vertex colour, premultiplied (the 3D view composites premultiplied targets)
    constexpr const char* kSilhouetteFragment = R"(
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
void main() {
    float a = texture(texture0, fragTexCoord).a * fragColor.a;
    finalColor = vec4(fragColor.rgb * a, a);
}
)";
    ray::Shader silhouette_shader{};
    bool silhouette_failed = false;
} // namespace

bool begin_silhouette_mode() {
    if (silhouette_failed) {
        return false;
    }
    if (silhouette_shader.id == 0) {
        const std::string fs = std::string(kSilhouetteHeader) + kSilhouetteFragment;
        silhouette_shader = ray::LoadShaderFromMemory(nullptr, fs.c_str());
        if (silhouette_shader.id == 0 || silhouette_shader.id == ray::rlGetShaderIdDefault()) {
            spdlog::warn("3D view: the silhouette shader did not compile; note sides are drawn darkened");
            silhouette_shader = ray::Shader{};
            silhouette_failed = true;
            return false;
        }
    }
    ray::BeginShaderMode(silhouette_shader);
    return true;
}

void end_silhouette_mode() {
    ray::EndShaderMode();
}

void gen_render_texture_mipmaps(RenderTarget& target) {
    ray::rlDrawRenderBatchActive();
    ray::GenTextureMipmaps(reinterpret_cast<ray::Texture2D*>(&target.texture));
    ray::SetTextureFilter(reinterpret_cast<ray::Texture2D&>(target.texture), ray::TEXTURE_FILTER_TRILINEAR);
}

int get_render_width() {
    return zdon_density::on ? zdon_density::target.texture.width : ray::GetRenderWidth();
}

int get_render_height() {
    return zdon_density::on ? zdon_density::target.texture.height
                            : ray::GetRenderHeight() - (composited_borderless ? 1 : 0);
}

int get_monitor_refresh_rate() {
    return ray::GetMonitorRefreshRate(ray::GetCurrentMonitor());
}

void begin_drawing() {
    ray::BeginDrawing();
}

void end_drawing() {
    ray::EndDrawing();
}

void swap_screen_buffer() {
    ray::SwapScreenBuffer();
}

Texture load_texture_from_image(Image image, bool mipmaps) {
    ray::Texture2D tex = ray::LoadTextureFromImage(to<ray::Image>(image));
    if (mipmaps) {
        ray::GenTextureMipmaps(&tex);
    }
    return tex;
}

void unload_texture(Texture tex) {
    ray::UnloadTexture(tex);
}

void set_texture_filter(Texture tex, int filter) {
    ray::SetTextureFilter(tex, filter);
}

void set_texture_wrap(Texture tex, int wrap) {
    ray::SetTextureWrap(tex, wrap);
}

Font get_font_default() {
    return to<Font>(ray::GetFontDefault());
}

void begin_scissor_mode(int x, int y, int width, int height) {
    ray::BeginScissorMode(x, y, width, height);
}

void end_scissor_mode() {
    ray::EndScissorMode();
}

static bool additive_keeps_alpha = false;
static bool additive_factors_set = false; // the custom-separate factors hold the additive ones

void set_additive_keeps_alpha(bool on) {
    additive_keeps_alpha = on;
}

void begin_blend_mode(int mode) {
    if (mode == blend_mode::ADDITIVE && additive_keeps_alpha) {
        ray::rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE, RL_ZERO, RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
        additive_factors_set = true;
        ray::BeginBlendMode(blend_mode::CUSTOM_SEPARATE);
        return;
    }
    if (additive_factors_set) { // back to the frame's own custom-separate factors
        set_separate_alpha_blend();
        additive_factors_set = false;
    }
    ray::BeginBlendMode(mode);
}

void set_separate_alpha_blend() {
    ray::rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD,
                                   RL_FUNC_ADD);
}

void set_backface_culling(bool enabled) {
    enabled ? ray::rlEnableBackfaceCulling() : ray::rlDisableBackfaceCulling();
}

void set_cull_face(int face) {
    ray::rlSetCullFace(face);
}

void end_blend_mode() {
    if (additive_factors_set) {
        set_separate_alpha_blend();
        additive_factors_set = false;
    }
    ray::EndBlendMode();
}

void begin_mode_2d(Camera2D camera) {
    ray::BeginMode2D(camera);
}

void end_mode_2d() {
    ray::EndMode2D();
}

void begin_mode_3d(Camera3D camera) {
    ray::BeginMode3D(camera);
}

void end_mode_3d() {
    ray::EndMode3D();
}

Vec2 get_world_to_screen_2d(Vec2 position, Camera2D camera) {
    return ray::GetWorldToScreen2D(position, camera);
}

Vec2 get_screen_to_world_2d(Vec2 position, Camera2D camera) {
    return ray::GetScreenToWorld2D(position, camera);
}

void unload_shader(Shader shader) {
    ray::UnloadShader(shader);
}

bool is_shader_valid(Shader shader) {
    return ray::IsShaderValid(shader);
}

int get_shader_location(Shader shader, const char* name) {
    return ray::GetShaderLocation(shader, name);
}

void set_shader_value(Shader shader, int loc, const void* value, int uniform_type) {
    ray::SetShaderValue(shader, loc, value, uniform_type);
}

void begin_shader_mode(Shader shader) {
    ray::BeginShaderMode(shader);
}

void end_shader_mode() {
    ray::EndShaderMode();
}

Image load_image_from_texture(Texture tex) {
    return to<Image>(ray::LoadImageFromTexture(tex));
}

Image load_image_from_screen() {
    return to<Image>(ray::LoadImageFromScreen());
}

void update_texture(Texture tex, const void* pixels) {
    ray::UpdateTexture(tex, pixels);
}

bool is_texture_valid(Texture tex) {
    return ray::IsTextureValid(tex);
}

RenderTarget load_render_texture(int width, int height) {
    return ray::LoadRenderTexture(width, height);
}

void unload_render_texture(RenderTarget target) {
    ray::UnloadRenderTexture(target);
}

// Z-DON: targets nest (an effect's own target inside the 3D lane's): ending one goes back to the one it was begun in,
// then to the frame's density target, then to the window
static std::vector<RenderTarget> texture_mode_stack;

void begin_texture_mode(RenderTarget target) {
    texture_mode_stack.push_back(target);
    ray::BeginTextureMode(target);
}

void end_texture_mode() {
    ray::EndTextureMode();
    if (!texture_mode_stack.empty()) {
        texture_mode_stack.pop_back();
    }
    if (!texture_mode_stack.empty()) {
        ray::BeginTextureMode(texture_mode_stack.back());
    } else if (zdon_density::bound) { // back to the frame's target, not the window
        ray::BeginTextureMode(zdon_density::target);
    }
}

Model load_model(const char* path) {
    return to<Model>(ray::LoadModel(path));
}

void unload_model(Model model) {
    ray::UnloadModel(to<ray::Model>(model));
}

ModelAnimation* load_model_animations(const char* path, int* count) {
    return to_ptr<ModelAnimation>(ray::LoadModelAnimations(path, count));
}

void unload_model_animations(ModelAnimation* anims, int count) {
    ray::UnloadModelAnimations(to_ptr<ray::ModelAnimation>(anims), count);
}

void update_model_animation(Model model, ModelAnimation anim, float frame) {
    ray::UpdateModelAnimation(to<ray::Model>(model), to<ray::ModelAnimation>(anim), frame);
}

bool get_mesh_bounds(const Mesh& mesh, Vec3& lower, Vec3& upper) {
    return cpu_mesh_bounds(mesh, lower, upper);
}

void draw_mesh(Mesh mesh, Material material, Mat4 transform) {
    ray::DrawMesh(to<ray::Mesh>(mesh), to<ray::Material>(material), transform);
}

void update_mesh_buffer(Mesh mesh, int index, const void* data, int data_size, int offset) {
    ray::UpdateMeshBuffer(to<ray::Mesh>(mesh), index, data, data_size, offset);
}

void set_material_texture(Material* material, int map_type, Texture texture) {
    ray::SetMaterialTexture(to_ptr<ray::Material>(material), map_type, texture);
}

void clear_background(Color color) {
    ray::ClearBackground({color.r, color.g, color.b, color.a});
}

Shader load_shader(const char* vs_path, const char* fs_path) {
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
    auto repath = [](const char* p) -> std::string {
        if (!p) {
            return "";
        }
        std::string s(p);
        const std::string prefix = "shader/";
        const std::string es_prefix = "shader/es/";
        if (s.compare(0, es_prefix.size(), es_prefix) == 0) {
            return s;
        }
        if (s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix) {
            s.insert(prefix.size(), "es/");
        }
        return s;
    };

    auto read_asset = [](const std::string& path) -> std::string {
        if (path.empty()) {
            return "";
        }
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "r");
        if (!io) {
            return "";
        }
        Sint64 size = SDL_GetIOSize(io);
        if (size <= 0) {
            SDL_CloseIO(io);
            return "";
        }
        std::string buf(static_cast<size_t>(size), '\0');
        size_t got = SDL_ReadIO(io, buf.data(), static_cast<size_t>(size));
        SDL_CloseIO(io);
        if (got != static_cast<size_t>(size)) {
            return "";
        }
        return buf;
    };

    std::string vs_repath = vs_path ? repath(vs_path) : std::string{};
    std::string fs_repath = fs_path ? repath(fs_path) : std::string{};
    std::string vs_src = vs_path ? read_asset(vs_repath) : std::string{};
    std::string fs_src = fs_path ? read_asset(fs_repath) : std::string{};
    return ray::LoadShaderFromMemory(vs_src.empty() ? nullptr : vs_src.c_str(),
                                     fs_src.empty() ? nullptr : fs_src.c_str());
#endif
#ifdef __EMSCRIPTEN__
    auto repath = [](const char* p) -> std::string {
        if (!p) {
            return "";
        }
        std::string s(p);
        const std::string prefix = "shader/";
        const std::string es_prefix = "shader/es/";
        if (s.compare(0, es_prefix.size(), es_prefix) == 0) {
            return s;
        }
        if (s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix) {
            s.insert(prefix.size(), "es/");
        }
        return s;
    };
    std::string vs_repath = vs_path ? repath(vs_path) : std::string{};
    std::string fs_repath = fs_path ? repath(fs_path) : std::string{};
    return ray::LoadShader(vs_path ? vs_repath.c_str() : nullptr, fs_path ? fs_repath.c_str() : nullptr);
#endif
    return ray::LoadShader(vs_path, fs_path);
}
