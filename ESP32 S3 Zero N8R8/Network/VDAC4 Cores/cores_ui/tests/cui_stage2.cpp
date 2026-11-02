// Три независимых экземпляра: частичный, полный и повтор последовательности.
// Проверяются пиксели, объединение dirty, модель памяти Tang и защитные поля.
#include "cores_ui.h"
#include "tests/card_fixtures.h"
#include "fonts/pui_font.h"
#include <stdint.h>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
int cui_ref_init(cui_canvas_t *, void *(*)(size_t, int, void *), void *);
int cui_ref_render(const cui_model_t *, uint32_t, int, cui_canvas_t *, cui_rect_t *, int);
void cui_ref_reset(void);
int cui_repeat_init(cui_canvas_t *, void *(*)(size_t, int, void *), void *);
int cui_repeat_render(const cui_model_t *, uint32_t, int, cui_canvas_t *, cui_rect_t *, int);
void cui_repeat_reset(void);
}

namespace {
constexpr int W = 640, H = 360, STRIDE = 648, N = W * H, SLOTS = 8;
constexpr uint16_t GUARD = 0xa73c;
const char *scenario = "init";
uint32_t now = 0;
uint64_t frames = 0, changed_pixels = 0, full_frames = 0, empty_frames = 0;
unsigned partial_max = 0, merged_max = 0;
unsigned allocations = 0, manage_cases = 0;

[[noreturn]] void fail(const char *what, int x = -1, int y = -1) {
    fprintf(stderr, "FAIL: %s; scenario=%s frame=%llu now=%lu x=%d y=%d\n", what, scenario,
            static_cast<unsigned long long>(frames), static_cast<unsigned long>(now), x, y);
    exit(1);
}
void require(bool condition, const char *what) {
    if (!condition)
        fail(what);
}

struct Surface {
    uint16_t data[(H + 2) * STRIDE];
    cui_canvas_t canvas{data + STRIDE, W, H, STRIDE};
    void clear() {
        for (auto &p : data)
            p = GUARD;
        canvas = {data + STRIDE, W, H, STRIDE};
    }
    void guards() const {
        for (int x = 0; x < STRIDE; ++x) {
            require(data[x] == GUARD, "canvas prefix");
            require(data[(H + 1) * STRIDE + x] == GUARD, "canvas suffix");
        }
        for (int y = 0; y < H; ++y)
            for (int x = W; x < STRIDE; ++x)
                require(data[(y + 1) * STRIDE + x] == GUARD, "canvas row padding");
    }
};
struct Cache {
    uint16_t data[N + 128];
    unsigned calls = 0;
    void clear() {
        for (auto &p : data)
            p = GUARD;
        calls = 0;
    }
    void guards() const {
        for (int i = 0; i < 64; ++i) {
            require(data[i] == GUARD, "cache prefix");
            require(data[N + 64 + i] == GUARD, "cache suffix");
        }
    }
};
void *allocate(size_t bytes, int psram, void *context) {
    require(bytes == N * sizeof(uint16_t) && psram == 1, "allocation request");
    auto &cache = *static_cast<Cache *>(context);
    ++cache.calls;
    ++allocations;
    return cache.data + 64;
}
void *refuse(size_t, int, void *) {
    return nullptr;
}
using Init = int (*)(cui_canvas_t *, void *(*)(size_t, int, void *), void *);
using Render = int (*)(const cui_model_t *, uint32_t, int, cui_canvas_t *, cui_rect_t *, int);
struct Engine {
    Surface surface;
    Cache cache;
    Init init;
    Render render;
    void (*reset)();
};
Engine engines[3] = {{{}, {}, cui_init, cui_render, cui_reset},
                     {{}, {}, cui_ref_init, cui_ref_render, cui_ref_reset},
                     {{}, {}, cui_repeat_init, cui_repeat_render, cui_repeat_reset}};
Surface swapped[3];
uint16_t old[N], tang[N], expected[208 * 15];
uint8_t covered[N];
struct Dirty {
    cui_rect_t pre, rect[SLOTS], post;
    Dirty() {
        memset(this, 0xcd, sizeof(*this));
    }
    void guards(int count) const {
        const cui_rect_t sentinel{0xcdcd, 0xcdcd, 0xcdcd, 0xcdcd};
        require(memcmp(&pre, &sentinel, sizeof(pre)) == 0, "dirty prefix");
        require(memcmp(&post, &sentinel, sizeof(post)) == 0, "dirty suffix");
        for (int i = count; i < SLOTS; ++i)
            require(memcmp(rect + i, &sentinel, sizeof(sentinel)) == 0, "dirty unused slots");
    }
};

void begin(const char *name) {
    scenario = name;
    for (auto &engine : engines) {
        engine.reset();
        engine.surface.clear();
        engine.cache.clear();
        require(engine.init(&engine.surface.canvas, allocate, &engine.cache) == 1, "init");
        require(engine.cache.calls == 1, "one init allocation");
    }
    for (auto &p : tang)
        p = GUARD;
}
cui_model_t model(int count = 3) {
    cui_model_t m{};
    m.count = uint8_t(count);
    m.state = CUI_MENU;
    m.revision = 1;
    m.progress_percent = -1;
    for (int i = 0; i < 32; ++i) {
        if (i < 3)
            snprintf(m.cores[i].name, sizeof(m.cores[i].name), "%s",
                     i == 0   ? "NES"
                     : i == 1 ? "NEXT"
                              : "AMIGA");
        else
            snprintf(m.cores[i].name, sizeof(m.cores[i].name), "MY_CORE_%02d-A.01", i);
        m.cores[i].size_bytes = 886 * 1024;
        m.cores[i].own_esp_layer = uint8_t(i % 4 != 3);
        m.cores[i].card = i < 3 ? fixtures::cards[i] : nullptr;
    }
    return m;
}

void step(cui_model_t &m, uint32_t time, int cap = 8, int force = 0) {
    now = time;
    auto &a = engines[0].surface.canvas;
    for (int y = 0; y < H; ++y)
        memcpy(old + y * W, a.px + y * a.stride, W * sizeof(uint16_t));
    cui_model_t saved;
    memcpy(&saved, &m, sizeof(m));
    Dirty dirty[3];
    int counts[3];
    unsigned alloc_before = allocations;
    for (int i = 0; i < 3; ++i)
        counts[i] = engines[i].render(&m, now, i == 1 ? 1 : force, &engines[i].surface.canvas,
                                      dirty[i].rect, cap);
    require(allocations == alloc_before, "allocation inside render");
    require(memcmp(&m, &saved, sizeof(m)) == 0, "input model mutated");
    require(counts[1] == 1, "full reference did not return full frame");
    require(counts[0] == counts[2], "deterministic dirty count");
    require(counts[0] >= 0 && counts[0] <= cap, "dirty count capacity");
    require(memcmp(dirty[0].rect, dirty[2].rect, size_t(counts[0]) * sizeof(cui_rect_t)) == 0,
            "deterministic dirty rectangles");
    memset(covered, 0, sizeof(covered));
    unsigned area = 0;
    for (int i = 0; i < 3; ++i) {
        require(counts[i] >= 0 && counts[i] <= cap, "rectangle overflow");
        dirty[i].guards(counts[i]);
        for (int r = 0; r < counts[i]; ++r) {
            const auto &d = dirty[i].rect[r];
            require(d.x0 < d.x1 && d.y0 < d.y1 && d.x1 <= W && d.y1 <= H, "rectangle bounds");
            require((d.x0 & 1) == 0 && (d.x1 & 1) == 0, "odd rectangle x");
            if (i == 0) {
                area += (d.x1 - d.x0) * (d.y1 - d.y0);
                for (int y = d.y0; y < d.y1; ++y) {
                    memset(covered + y * W + d.x0, 1, d.x1 - d.x0);
                    memcpy(tang + y * W + d.x0, a.px + y * a.stride + d.x0,
                           (d.x1 - d.x0) * sizeof(uint16_t));
                }
            }
        }
        engines[i].surface.guards();
        engines[i].cache.guards();
    }
    bool full = counts[0] == 1 && area == N;
    if (full)
        ++full_frames;
    else if (counts[0] == 0)
        ++empty_frames;
    else if (cap >= 2) {
        if (area > partial_max)
            partial_max = area;
        require(area <= 12000, "ordinary frame area budget");
    } else if (area > merged_max)
        merged_max = area;
    const auto &b = engines[1].surface.canvas;
    const auto &c = engines[2].surface.canvas;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int at = y * W + x;
            uint16_t p = a.px[y * a.stride + x];
            if (p != b.px[y * b.stride + x])
                fail("incremental vs full pixel", x, y);
            if (p != c.px[y * c.stride + x])
                fail("deterministic pixel", x, y);
            if (p != old[at]) {
                ++changed_pixels;
                if (!covered[at])
                    fail("changed pixel outside dirty union", x, y);
            }
            if (p != tang[at])
                fail("Tang mirror mismatch", x, y);
        }
    ++frames;
}
void advance(cui_model_t &m, int frames_to_draw = 1, int cap = 8) {
    for (int i = 0; i < frames_to_draw; ++i)
        step(m, now + 40, cap);
}

// Независимый эталон надписи: заданная строка и покрытие глифов, без функций UI.
// Сравнивается вся область подписи, включая пустой фон после удаления текста.
void label(const char *value, bool shared = false) {
    constexpr uint16_t bg = uint16_t((2 << 11) | (7 << 5) | 5);
    uint16_t fg =
        shared ? uint16_t((31 << 11) | (46 << 5) | 14) : uint16_t((13 << 11) | (54 << 5) | 24);
    for (auto &p : expected)
        p = bg;
    int pen = 0;
    for (const unsigned char *s = reinterpret_cast<const unsigned char *>(value); *s; ++s) {
        const auto &font = pui_font::fonts[0];
        unsigned ch = *s >= 32 && *s <= 126 ? *s : '?';
        const auto &glyph = font.glyphs[ch - 32];
        for (int y = 0; y < glyph.height; ++y)
            for (int x = 0; x < glyph.width; ++x) {
                int i = y * glyph.width + x;
                int v = font.pixels[glyph.offset + i / 2];
                int alpha = (i % 2) ? (v & 15) : (v >> 4);
                if (pen + x >= 208 || y >= 15)
                    fail("expected label bounds");
                if (alpha) {
                    unsigned out = 0;
                    const int bits[] = {0, 5, 11};
                    const unsigned masks[] = {31, 63, 31};
                    for (int k = 0; k < 3; ++k) {
                        unsigned back = (bg >> bits[k]) & masks[k],
                                 front = (fg >> bits[k]) & masks[k];
                        out |= ((back * unsigned(15 - alpha) + front * unsigned(alpha) + 7) / 15)
                               << bits[k];
                    }
                    expected[y * 208 + pen + x] = uint16_t(out);
                }
            }
        pen += glyph.width;
    }
    const auto &c = engines[0].surface.canvas;
    for (int y = 0; y < 15; ++y)
        for (int x = 0; x < 208; ++x)
            if (c.px[(277 + y) * c.stride + 388 + x] != expected[y * 208 + x])
                fail("Manage label pixels", x, y);
    ++manage_cases;
}

void selection_and_scroll() {
    begin("selection forward/back");
    auto m = model();
    now = 0;
    step(m, now);
    for (int round = 0; round < 3; ++round)
        for (int sel : {1, 2, 1, 0}) {
            m.selected = uint8_t(sel);
            advance(m, 9, round + 1);
        }
    begin("scroll 32 cores");
    m = model(32);
    step(m, now);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < 32; ++i) {
            m.selected = uint8_t(pass ? 31 - i : i);
            advance(m, 7, (i % 3) + 1);
        }
    // Повторный выбор до окончания предыдущего движения.
    for (int i = 0; i < 40; ++i) {
        m.selected = uint8_t((i * 7) % 32);
        advance(m, 1, 2);
    }
}
void counts_and_names() {
    begin("all counts 0..32");
    auto m = model();
    for (int count = 0; count <= 32; ++count) {
        m.count = uint8_t(count);
        ++m.revision;
        for (int sel : {0, count / 2, count ? count - 1 : 0, 255}) {
            m.selected = uint8_t(sel);
            advance(m, 2, 2);
        }
    }
    scenario = "count clamp and bounded strings";
    m.count = 255;
    m.selected = 255;
    ++m.revision;
    for (auto &core : m.cores) {
        memset(core.name, 'W', sizeof(core.name));
        core.own_esp_layer = 1;
        core.size_bytes = UINT32_MAX;
    }
    advance(m, 8);
    scenario = "long and unknown names";
    m = model(32);
    for (int i = 0; i < 32; ++i) {
        m.selected = uint8_t(i);
        advance(m, 2);
    }
    scenario = "ASCII replacement";
    for (int c = 0; c < 256; ++c) {
        m.selected = 0;
        memset(m.cores[0].name, c, 16);
        memset(m.error_text, c, 61);
        m.state = (c & 1) ? CUI_ERROR : CUI_MENU;
        m.error = CUI_ERR_ESP_TEXT;
        ++m.revision;
        advance(m);
    }
}
void transitions() {
    begin("every state pair");
    auto m = model();
    for (int from = 0; from < 8; ++from)
        for (int to = 0; to < 8; ++to) {
            m.state = static_cast<cui_state_t>(from);
            advance(m, 2);
            m.state = static_cast<cui_state_t>(to);
            advance(m, 3);
        }
    scenario = "menu load error and progress";
    for (int count : {0, 1, 32}) {
        m = model(count);
        ++m.revision;
        m.state = CUI_MENU;
        advance(m, 2);
        m.state = CUI_STARTING_ESP;
        advance(m, 7);
        m.state = CUI_COPYING_CACHE;
        m.progress_percent = -1;
        advance(m, 7);
        for (int p = 0; p <= 100; ++p) {
            m.progress_percent = int16_t(p);
            advance(m, 1, p % 2 ? 2 : 1);
        }
        for (int p = 100; p >= 0; --p) {
            m.progress_percent = int16_t(p);
            advance(m);
        }
        for (int p : {-32768, -1, 0, 100, 101, 32767}) {
            m.progress_percent = int16_t(p);
            advance(m, 2);
        }
        m.state = CUI_LOADING;
        advance(m, 8);
        m.state = CUI_PROGRAMMING;
        advance(m, 8);
        m.state = CUI_ERROR;
        for (int e = 0; e < 10; ++e) {
            m.error = static_cast<cui_error_t>(e);
            advance(m, 2);
        }
        m.error = CUI_ERR_ESP_TEXT;
        memset(m.error_text, 'W', sizeof(m.error_text));
        ++m.revision;
        advance(m);
        memset(m.error_text, ' ', sizeof(m.error_text));
        ++m.revision;
        advance(m);
        snprintf(m.error_text, sizeof(m.error_text),
                 "ERR: SD read failed while opening /CORES/NES.BIN (code 04)");
        ++m.revision;
        advance(m);
        m.state = CUI_MENU;
        advance(m, 8);
    }
}
void management() {
    begin("Manage from card only, missing card/key, shared layer");
    auto m = model(3);
    step(m, 0);
    label("Manage - F1");
    m.selected = 1;
    advance(m);
    label("Manage - F1");
    m.selected = 2;
    advance(m);
    label("Manage - F11");
    // Копия проверенной карточки позволяет отдельно испытать смену текста и revision.
    cui_card_t edited = *fixtures::cards[2];
    m.cores[2].card = &edited;
    snprintf(edited.manage_key, sizeof(edited.manage_key), "F2");
    ++m.revision;
    advance(m);
    label("Manage - F2");
    edited.manage_key[0] = 0;
    ++m.revision;
    advance(m);
    label("");
    snprintf(m.cores[2].name, 16, "UNLISTED");
    ++m.revision;
    advance(m);
    label("");
    snprintf(edited.manage_key, sizeof(edited.manage_key), "Ctrl-F11");
    ++m.revision;
    advance(m);
    label("Manage - Ctrl-F11");
    snprintf(edited.manage_key, sizeof(edited.manage_key), "MMMMMMMM");
    ++m.revision;
    advance(m);
    label("Manage - MMMMMMMM");
    m.cores[2].own_esp_layer = 0;
    ++m.revision;
    advance(m);
    label("Shared ESP layer", true);
    edited.manage_key[0] = 0;
    ++m.revision;
    advance(m);
    label("Shared ESP layer", true);
    m.cores[2].card = nullptr;
    snprintf(m.cores[2].name, 16, "NES");
    ++m.revision;
    advance(m);
    label("Shared ESP layer", true);
    m.cores[2].own_esp_layer = 1;
    ++m.revision;
    advance(m);
    label(""); // Даже имя NES не даёт ни карточки, ни клавиши.
    m.cores[2].card = fixtures::cards[0];
    snprintf(m.cores[2].name, 16, "RENAMED");
    ++m.revision;
    advance(m);
    label("Manage - F1"); // Карточка не зависит от имени BIN.
}
void timing_and_reset() {
    begin("same timestamp and irregular cadence");
    auto m = model();
    step(m, 1000);
    for (uint32_t offset : {0u, 1u, 2u, 19u, 39u, 40u, 41u, 79u, 80u, 120u, 239u, 240u, 241u})
        step(m, 1000 + offset);
    m.selected = 2;
    step(m, 1242);
    for (int i = 0; i < 32; ++i)
        step(m, now + 7);
    for (int i = 0; i < 10; ++i)
        step(m, now);
    begin("uint32 time wrap");
    m = model();
    step(m, UINT32_MAX - 240);
    for (int i = 0; i < 20; ++i) {
        if (i == 3)
            m.selected = 1;
        advance(m);
    }
    step(m, now + 1000000);
    step(m, now, 8, 1);
    scenario = "reinit without allocation";
    for (auto &e : engines) {
        unsigned before = e.cache.calls;
        require(e.init(&e.surface.canvas, nullptr, nullptr) == 1, "repeat init failed");
        require(e.cache.calls == before, "repeat init allocated");
    }
    advance(m, 3);
    scenario = "output pointer and stride";
    for (int i = 0; i < 3; ++i) {
        swapped[i].clear();
        engines[i].surface.canvas = swapped[i].canvas;
    }
    advance(m, 3);
    for (int i = 0; i < 3; ++i) {
        swapped[i].guards();
        swapped[i].clear();
        engines[i].surface.canvas.stride = W;
    }
    advance(m, 3);
    for (int i = 0; i < 3; ++i) {
        for (int p = 0; p < STRIDE; ++p)
            require(swapped[i].data[p] == GUARD, "packed canvas prefix");
        for (int p = STRIDE + N; p < (H + 2) * STRIDE; ++p)
            require(swapped[i].data[p] == GUARD, "packed canvas suffix");
        engines[i].surface.canvas = {engines[i].surface.data + STRIDE, W, H, STRIDE};
    }
    advance(m, 3);
}
void invalid_inputs() {
    scenario = "invalid inputs and allocation failure";
    auto m = model();
    auto &e = engines[0];
    e.reset();
    e.surface.clear();
    Dirty dirty;
    require(e.render(&m, 0, 1, &e.surface.canvas, dirty.rect, 8) == 0, "render before init");
    require(e.init(nullptr, allocate, &e.cache) == 0, "null init canvas");
    require(e.init(&e.surface.canvas, nullptr, nullptr) == 0, "null allocate");
    require(e.init(&e.surface.canvas, refuse, nullptr) == 0, "allocation failure");
    for (int mode = 0; mode < 4; ++mode) {
        auto bad = e.surface.canvas;
        if (mode == 0)
            bad.px = nullptr;
        if (mode == 1)
            bad.w = 639;
        if (mode == 2)
            bad.h = 359;
        if (mode == 3)
            bad.stride = 639;
        require(e.init(&bad, allocate, &e.cache) == 0, "invalid init accepted");
    }
    begin("invalid renders preserve history");
    m = model();
    step(m, 400);
    memcpy(old, e.surface.data, sizeof(old));
    unsigned before = allocations;
    require(e.render(nullptr, 440, 0, &e.surface.canvas, dirty.rect, 8) == 0, "null model");
    require(e.render(&m, 440, 0, nullptr, dirty.rect, 8) == 0, "null render canvas");
    require(e.render(&m, 440, 0, &e.surface.canvas, nullptr, 8) == 0, "null dirty");
    for (int cap : {0, -1})
        require(e.render(&m, 440, 0, &e.surface.canvas, dirty.rect, cap) == 0, "invalid max_dirty");
    for (int mode = 0; mode < 4; ++mode) {
        auto bad = e.surface.canvas;
        if (mode == 0)
            bad.px = nullptr;
        if (mode == 1)
            bad.w = 641;
        if (mode == 2)
            bad.h = 361;
        if (mode == 3)
            bad.stride = 1;
        require(e.render(&m, 440, 0, &bad, dirty.rect, 8) == 0, "invalid canvas accepted");
    }
    require(memcmp(old, e.surface.data, sizeof(old)) == 0, "invalid render mutated canvas");
    require(allocations == before, "invalid render allocated");
    dirty.guards(0);
    step(m, 440);
}
void mixed() {
    begin("seeded mixed transitions");
    auto m = model(32);
    uint32_t seed = 0x684C020u;
    for (int i = 0; i < 700; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        switch (seed % 7) {
        case 0:
            m.selected = uint8_t(seed >> 16);
            break;
        case 1:
            m.count = uint8_t((seed >> 16) % 33);
            break;
        case 2:
            m.state = static_cast<cui_state_t>((seed >> 16) % 8);
            break;
        case 3:
            m.error = static_cast<cui_error_t>((seed >> 16) % 10);
            break;
        case 4:
            m.progress_percent = int16_t((seed >> 16) % 105) - 2;
            break;
        case 5:
            ++m.revision;
            m.cores[(seed >> 16) % 32].own_esp_layer ^= 1;
            break;
        default:
            break;
        }
        advance(m, 1, int(seed % 3) + 1);
    }
}
}

int main() {
    fixtures::load();
    selection_and_scroll();
    counts_and_names();
    transitions();
    management();
    timing_and_reset();
    invalid_inputs();
    mixed();
    for (auto &e : engines) {
        e.surface.guards();
        e.cache.guards();
        e.reset();
    }
    printf("PASS: %llu frames; %llu changed pixels; %llu full; %llu empty; %u Manage cases\n",
           static_cast<unsigned long long>(frames), static_cast<unsigned long long>(changed_pixels),
           static_cast<unsigned long long>(full_frames),
           static_cast<unsigned long long>(empty_frames), manage_cases);
    printf("Partial area max (2+ slots): %u; merged max (1 slot): %u; init allocations: %u\n",
           partial_max, merged_max, allocations);
    FILE *out = fopen("stage2_metrics.json", "wb");
    require(out != nullptr, "metrics file");
    fprintf(
        out,
        "{\"frames\":%llu,\"changed_pixels\":%llu,\"full_frames\":%llu,\"empty_frames\":%llu,"
        "\"manage_cases\":%u,\"partial_area_max\":%u,\"merged_area_max\":%u,\"init_allocations\":%"
        "u}\n",
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(changed_pixels),
        static_cast<unsigned long long>(full_frames), static_cast<unsigned long long>(empty_frames),
        manage_cases, partial_max, merged_max, allocations);
    fclose(out);
    return 0;
}
