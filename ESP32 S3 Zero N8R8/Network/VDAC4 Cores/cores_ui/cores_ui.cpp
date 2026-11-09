#include "cores_ui.h"
#include "fonts/pui_font.h"
#include "assets/cui_art.h"
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint16_t rgb(int r, int g, int b) {
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
constexpr uint16_t ink = rgb(238, 239, 228), muted = rgb(155, 176, 190);
constexpr uint16_t faint = rgb(48, 65, 82), panel = rgb(18, 30, 44);
constexpr uint16_t amber = rgb(255, 184, 112), red = rgb(255, 131, 122);
constexpr uint16_t mint = rgb(104, 219, 197);
struct CardView {
    const char *title, *full_name, *line1, *line2, *category, *manage_key;
    char origin[64];
    uint16_t accent;
    const cui_card_t *card;
};
const CardView unknown = {"FPGA core",
                          "Custom hardware, ready to explore",
                          "Your next",
                          "discovery",
                          "FPGA CORE",
                          "",
                          "Unlisted system",
                          rgb(133, 186, 214),
                          nullptr};
CardView view(const cui_card_t *card) {
    if (!card)
        return unknown;
    CardView result{card->title, card->full_name, card->line1,
                    card->line2, card->category,  card->manage_key,
                    {},          card->accent,    card};
    snprintf(result.origin, sizeof(result.origin), "%s%s%s", card->year,
             card->year[0] && card->manufacturer[0] ? " / " : "", card->manufacturer);
    return result;
}

struct Runtime {
    uint16_t *base;
    bool ready, drawn;
    uint32_t revision, start_ms, last_ms;
    int selection, count, state, error, progress;
    int marker_from, marker_y, marker_to;
    uint16_t *output;
    uint16_t stride;
} rt = {};
cui_canvas_t target = {};

int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
bool valid(const cui_canvas_t *c) {
    return c && c->px && c->w == CUI_WIDTH && c->h == CUI_HEIGHT && c->stride >= CUI_WIDTH;
}
void put(int x, int y, uint16_t c) {
    if (x >= 0 && x < CUI_WIDTH && y >= 0 && y < CUI_HEIGHT)
        target.px[y * target.stride + x] = c;
}
void box(int x, int y, int w, int h, uint16_t color) {
    int x0 = clamp(x, 0, CUI_WIDTH), x1 = clamp(x + w, 0, CUI_WIDTH);
    int y0 = clamp(y, 0, CUI_HEIGHT), y1 = clamp(y + h, 0, CUI_HEIGHT);
    for (int yy = y0; yy < y1; ++yy) {
        uint16_t *p = target.px + yy * target.stride;
        for (int xx = x0; xx < x1; ++xx)
            p[xx] = color;
    }
}
uint16_t mix(uint16_t a, uint16_t b, int weight) {
    int r = ((a >> 11) * (15 - weight) + (b >> 11) * weight + 7) / 15;
    int g = (((a >> 5) & 63) * (15 - weight) + ((b >> 5) & 63) * weight + 7) / 15;
    int bl = ((a & 31) * (15 - weight) + (b & 31) * weight + 7) / 15;
    return uint16_t((r << 11) | (g << 5) | bl);
}
int text_width(const char *s, int font, int limit = 128) {
    int width = 0;
    for (int i = 0; i < limit && s[i]; ++i) {
        uint8_t c = uint8_t(s[i]);
        if (c < 32 || c > 126)
            c = '?';
        width += pui_font::fonts[font].glyphs[c - 32].width;
    }
    return width;
}
int text(int x, int y, const char *s, int font, uint16_t color, int max_width = 640,
         int limit = 128) {
    const auto &f = pui_font::fonts[font];
    int start = x, dots = text_width("...", font);
    bool ellipsis = text_width(s, font, limit) > max_width;
    for (int i = 0; i < limit && s[i]; ++i) {
        uint8_t c = uint8_t(s[i]);
        if (c < 32 || c > 126)
            c = '?';
        const auto &g = f.glyphs[c - 32];
        if (x - start + g.width > (ellipsis ? max_width - dots : max_width)) {
            if (ellipsis)
                text(x, y, "...", font, color, dots, 3);
            break;
        }
        for (int yy = 0; yy < g.height; ++yy)
            for (int xx = 0; xx < g.width; ++xx) {
                int n = yy * g.width + xx;
                uint8_t packed = f.pixels[g.offset + n / 2];
                int a = (n & 1) ? (packed & 15) : (packed >> 4);
                int px = x + xx, py = y + yy;
                if (a && px >= 0 && px < CUI_WIDTH && py >= 0 && py < CUI_HEIGHT)
                    put(px, py, mix(target.px[py * target.stride + px], color, a));
            }
        x += g.width;
    }
    return x;
}
void wrapped(int x, int y, const char *s, int font, uint16_t color, int width, int max_lines,
             int limit = 128) {
    int pos = 0;
    for (int row = 0; row < max_lines && pos < limit && s[pos]; ++row) {
        char line[128] = {};
        int n = 0, last_space = -1;
        while (pos + n < limit && s[pos + n] && n < 126) {
            line[n] = s[pos + n];
            line[n + 1] = 0;
            if (text_width(line, font) > width) {
                line[n] = 0;
                break;
            }
            if (line[n] == ' ')
                last_space = n;
            ++n;
        }
        if (s[pos + n] && pos + n < limit && last_space > 0 && row + 1 < max_lines)
            n = last_space;
        if (n == 0)
            n = 1;
        line[n] = 0;
        if (row + 1 == max_lines && pos + n < limit && s[pos + n]) {
            while (n > 0 && text_width(line, font) + text_width("...", font) > width)
                line[--n] = 0;
            memcpy(line + n, "...", 4);
        }
        text(x, y + row * (pui_font::fonts[font].height + 4), line, font, color, width);
        pos += n;
        while (pos < limit && s[pos] == ' ')
            ++pos;
    }
}
void sprite(const cui_card_t *card, bool small, int x, int y) {
    const auto &s = card ? (small ? card->icon : card->image) : cui_art::sprites[small ? 1 : 0];
    const uint16_t *palette = card ? card->palette : cui_art::palette;
    int px = 0, py = 0;
    for (uint32_t i = 0; i < s.bytes; i += 2) {
        int len = s.rle[i];
        uint8_t ci = s.rle[i + 1];
        for (int k = 0; k < len; ++k) {
            if (ci)
                put(x + px, y + py, palette[ci]);
            if (++px == s.w) {
                px = 0;
                ++py;
            }
        }
    }
}
void border(int x, int y, int w, int h, uint16_t c) {
    box(x, y, w, 1, c);
    box(x, y + h - 1, w, 1, c);
    box(x, y, 1, h, c);
    box(x + w - 1, y, 1, h, c);
}
void background() {
    for (int y = 0; y < CUI_HEIGHT; ++y)
        for (int x = 0; x < CUI_WIDTH; ++x) {
            int glow = clamp(90 - cui_art::bg_x[x] - cui_art::bg_y[y], 0, 90);
            put(x, y, cui_art::bg_color[glow * 16 + ((y & 3) << 2) + (x & 3)]);
        }
    // Неподвижные дорожки напоминают монтажную плату, не конкурируя с текстом.
    for (int x = 416; x < 640; x += 32) {
        box(x, 0, 1, 13, faint);
        box(x, 12, 17, 1, faint);
        box(x + 16, 12, 1, 8, faint);
    }
    text(24, 17, "VDAC4 Cores", 2, ink);
    text(451, 26, "TANG SD / CORES", 0, muted, 165);
    box(24, 55, 592, 1, faint);
    box(24, 320, 592, 1, faint);
}
// Список — всегда крупные карточки, видно три; больше трёх — прокрутка с полосой (пользователь,
// 2026-11-02: «Не нужно компактный», «Лучше скролл-бар»).
int visible_count(int) {
    return 3;
}
int list_top(int count, int selected) {
    int visible = visible_count(count);
    return clamp(selected - visible / 2, 0, count > visible ? count - visible : 0);
}
int row_y(int count, int selected) {
    return 91 + (selected - list_top(count, selected)) * 68;
}
void menu_base(const cui_model_t &m, int count, int selected) {
    const auto &entry = m.cores[selected];
    const auto &sys = view(entry.card);
    text(24, 65, "Choose a core", 0, muted);
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d / %02d", selected + 1, count);
    text(542, 65, buf, 0, muted, 74);
    int first = list_top(count, selected), visible = visible_count(count), step = 68;
    for (int i = first; i < count && i < first + visible; ++i) {
        int y = 91 + (i - first) * step;
        const auto &item = m.cores[i];
        const auto &s = view(item.card);
        box(30, y, 166, step - 6, i == selected ? mix(panel, s.accent, 2) : panel);
        if (i == selected)
            border(30, y, 166, step - 6, mix(panel, s.accent, 6));
        sprite(s.card, true, 34, y + 6);
        text(96, y + 8, item.name, 1, i == selected ? ink : muted, 94, 15);
        text(42, y + 42, s.category, 0, i == selected ? s.accent : muted, 145);
        if (!item.own_esp_layer)
            box(184, y + step - 14, 4, 4, amber);
    }
    // Больше трёх — полоса прокрутки между списком и карточкой: дорожка на высоту трёх карточек,
    // ползунок — доля видимого и место в списке
    if (count > visible) {
        const int track = 3 * step - 6;
        box(201, 91, 4, track, faint);
        int h = track * visible / count;
        if (h < 12)
            h = 12;
        box(201, 91 + (track - h) * first / (count - visible), 4, h, sys.accent);
    }
    box(212, 88, 404, 210, panel);
    border(212, 88, 404, 210, faint);
    box(212, 88, 404, 2, sys.accent);
    text(232, 100, sys.title, 2, ink, 364);
    if (!entry.card) {
        char name[16];
        memcpy(name, entry.name, 15);
        name[15] = 0;
        snprintf(buf, sizeof(buf), "%s.BIN", name);
        text(232, 132, buf, 0, ink, 364);
    } else
        text(232, 132, sys.full_name, 0, muted, 364);
    text(232, 153, sys.origin, 0, sys.accent, 230);
    text(232, 188, sys.line1, 0, muted, 108);
    text(232, 206, sys.line2, 0, muted, 108);
    sprite(sys.card, false, 338, 145);
    box(232, 268, 364, 1, faint);
    snprintf(buf, sizeof(buf), "%lu KB", static_cast<unsigned long>(entry.size_bytes / 1024));
    text(232, 277, buf, 0, ink, 112);
    if (!entry.own_esp_layer) {
        text(388, 277, "Shared ESP layer", 0, amber, 208);
    } else {
        const char *key = sys.manage_key;
        if (key[0]) {
            snprintf(buf, sizeof(buf), "Manage - %.8s", key);
            text(388, 277, buf, 0, mint, 208);
        }
    }
    if (entry.own_esp_layer) {
        char name[16];
        memcpy(name, entry.name, 15);
        name[15] = 0;
        snprintf(buf, sizeof(buf), "%s.BIN", name);
        text(24, 301, buf, 0, muted, 270);
    } else {
        text(24, 301, "Shared ESP layer: keyboard input is unavailable", 0, amber, 592);
    }
    text(24, 330, "Up/Down, Enter - run, ESC - back", 0, ink, 592);
}
const char *error_title(cui_error_t error) {
    switch (error) {
    case CUI_ERR_NO_CORES:
        return "No cores found";
    case CUI_ERR_NO_SD:
        return "Tang SD card not found";
    case CUI_ERR_ESP_LAYER:
        return "ESP layer did not start";
    case CUI_ERR_TIMEOUT:
        return "Core start timed out";
    case CUI_ERR_CORE:
        return "Core did not start";
    case CUI_ERR_ESP_LOST:
        return "ESP connection lost";
    case CUI_ERR_UPDATE_NES:
        return "NES layer update required";
    case CUI_ERR_NO_ESP:
        return "ESP not found";
    case CUI_ERR_NO_ANSWER:
        return "ESP is not responding";
    default:
        return "ESP reported an error";
    }
}
const char *error_detail(cui_error_t error) {
    switch (error) {
    case CUI_ERR_NO_CORES:
        return "ERROR: no cores in \\CORES on the Tang SD";
    case CUI_ERR_NO_SD:
        return "ERROR: no Tang SD card";
    case CUI_ERR_ESP_LAYER:
        return "ERROR: the ESP layer did not start";
    case CUI_ERR_TIMEOUT:
        return "ERROR: the core did not start in 180 s";
    case CUI_ERR_CORE:
        return "ERROR: the core did not start";
    case CUI_ERR_ESP_LOST:
        return "ERROR: the ESP stopped answering";
    case CUI_ERR_UPDATE_NES:
        return "update CORES/NES.ELF to 0.7+";
    case CUI_ERR_NO_ESP:
        return "ERROR: no ESP on the SPI";
    case CUI_ERR_NO_ANSWER:
        return "ERROR: the ESP does not answer (0.1.84+)";
    default:
        return "ERR: Unknown ESP error";
    }
}
void state_base(const cui_model_t &m, int count, int selected) {
    bool error = m.state == CUI_ERROR || (m.state == CUI_MENU && count == 0);
    cui_error_t e = m.state == CUI_MENU ? CUI_ERR_NO_CORES : m.error;
    bool reading = m.state == CUI_CONNECTING || m.state == CUI_READING;
    uint16_t accent = error ? red : mint;
    const char *label = error ? "ATTENTION" : reading ? "DISCOVERING SYSTEMS" : "LAUNCHING CORE";
    text(24, 65, label, 0, error ? red : muted);
    box(24, 92, 592, 209, panel);
    border(24, 92, 592, 209, faint);
    box(24, 92, 4, 209, accent);
    // Слева — рисунок выбранной машины, при отсутствии списка — общая плата.
    const auto sys = count ? view(m.cores[selected].card) : unknown;
    sprite(reading ? nullptr : sys.card, false, 26, 124);
    if (count && !reading)
        text(58, 266, m.cores[selected].name, 1, sys.accent, 210, 15);
    else
        text(58, 266, "Tang microSD", 1, muted, 210);
    const char *title = "Loading core";
    const char *detail = "Loading into the Tang...";
    switch (m.state) {
    case CUI_CONNECTING:
        title = "Connecting to ESP";
        detail = "Connecting to the ESP...";
        break;
    case CUI_READING:
        title = "Reading Tang SD";
        detail = "Reading the Tang SD card...";
        break;
    case CUI_STARTING_ESP:
        title = "Starting ESP layer";
        detail = "Starting the ESP layer of the core...";
        break;
    case CUI_COPYING_CACHE:
        title = "Preparing the core";
        detail = "Copying from SD to the BL616 cache";
        break;
    case CUI_PROGRAMMING:
        title = "Starting the FPGA";
        detail = "Loading the core into the Tang...";
        break;
    case CUI_RECEIVING:
        title = "Loading the game";
        detail = m.error_text[0] ? m.error_text : "Reading the file from the Evo SD...";
        break;
    default:
        break;
    }
    if (error) {
        title = error_title(e);
        detail = (e == CUI_ERR_ESP_TEXT && m.error_text[0]) ? m.error_text : error_detail(e);
    }
    wrapped(294, 109, title, 1, ink, 298, 2);
    wrapped(294, 163, detail, 0, error ? red : muted, 298, 3,
            (e == CUI_ERR_ESP_TEXT && error) || m.state == CUI_RECEIVING ? 60 : 128);
    if (!error) {
        if (m.state == CUI_COPYING_CACHE)
            wrapped(294, 238, "First load or updated core. This can take about a minute.", 0, muted,
                    298, 2);
        else if (m.state == CUI_RECEIVING)
            wrapped(294, 238, "Reading from the Evo SD card. The core starts when it is loaded.", 0,
                    muted, 298, 2);
        else if (!reading)
            wrapped(294, 238, "The core will take over this screen when it starts.", 0, muted, 298,
                    2);
        else
            text(294, 247, "Looking for /CORES/*.BIN", 0, muted, 298);
    } else {
        const char *note = e == CUI_ERR_NO_CORES     ? "Add .BIN files to the /CORES folder."
                           : e == CUI_ERR_UPDATE_NES ? "The NES core needs a newer ESP layer."
                           : e == CUI_ERR_NO_SD      ? "Check the microSD card in the Tang."
                                                     : "Return to Wild Commander to continue.";
        wrapped(294, 242, note, 0, muted, 298, 2);
    }
    text(24, 330,
         error     ? "ESC - return to Wild Commander"
         : reading ? "ESC - back to Wild Commander"
                   : "F12 - back to Wild Commander",
         0, ink, 592);
}
void copy_base(cui_rect_t r) {
    for (int y = r.y0; y < r.y1; ++y)
        memcpy(target.px + y * target.stride + r.x0, rt.base + y * CUI_WIDTH + r.x0,
               (r.x1 - r.x0) * sizeof(uint16_t));
}
int marker_at(uint32_t now) {
    uint32_t elapsed = now - rt.start_ms;
    if (elapsed >= 240)
        return rt.marker_to;
    int t = int(elapsed) * 256 / 240;
    int ease = t * t * (768 - 2 * t) / 65536;
    return rt.marker_from + (rt.marker_to - rt.marker_from) * ease / 256;
}
cui_rect_t indicator_rect(const cui_model_t &m, int count) {
    return m.state == CUI_MENU && count ? cui_rect_t{388, 306, 600, 313}
                                        : cui_rect_t{294, 216, 594, 234};
}
void dynamics(const cui_model_t &m, int count, int selected, uint32_t now) {
    if (m.state == CUI_MENU && count) {
        const auto &sys = view(m.cores[selected].card);
        int h = 60;
        rt.marker_y = marker_at(now);
        box(24, rt.marker_y + 1, 4, h, sys.accent);
        // Световая дорожка отдельна от текста. Она не перемещает большие области.
        uint32_t elapsed = now - rt.start_ms;
        int width = elapsed < 240 ? int(elapsed) * 196 / 240 : 196;
        box(400, 309, 196, 2, faint);
        box(400, 309, width, 2, mix(panel, sys.accent, 9));
        int phase = int((now / 40) % 50);
        int brightness = phase < 25 ? phase : 50 - phase;
        box(590, 307, 6, 6, mix(panel, sys.accent, 7 + brightness * 8 / 25));
    } else if (m.state != CUI_ERROR && !(m.state == CUI_MENU && count == 0)) {
        if ((m.state == CUI_COPYING_CACHE || m.state == CUI_RECEIVING) && m.progress_percent >= 0) {
            int p = clamp(m.progress_percent, 0, 100);
            box(294, 225, 242, 4, faint);
            box(294, 225, 242 * p / 100, 4, mint);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", p);
            text(546, 216, buf, 0, ink, 48);
        } else {
            // Три точки, не шкала: процент и время завершения неизвестны.
            int phase = int((now / 160) % 6);
            if (phase > 2)
                phase = 5 - phase;
            for (int i = 0; i < 3; ++i)
                box(294 + i * 16, 222, 7, 7, i == phase ? mint : faint);
            text(356, 216, "Please wait", 0, muted, 230);
        }
    }
}
void add_dirty(cui_rect_t r, cui_rect_t *list, int max, int &n) {
    if (n < max)
        list[n++] = r;
    else {
        auto &a = list[max - 1];
        if (r.x0 < a.x0)
            a.x0 = r.x0;
        if (r.y0 < a.y0)
            a.y0 = r.y0;
        if (r.x1 > a.x1)
            a.x1 = r.x1;
        if (r.y1 > a.y1)
            a.y1 = r.y1;
    }
}
}

extern "C" int cui_init(cui_canvas_t *c, void *(*alloc)(size_t, int, void *), void *ctx) {
    if (!valid(c))
        return 0;
    if (!rt.base) {
        if (!alloc)
            return 0;
        rt.base = static_cast<uint16_t *>(alloc(CUI_WIDTH * CUI_HEIGHT * sizeof(uint16_t), 1, ctx));
        if (!rt.base)
            return 0;
    }
    rt.ready = true;
    rt.drawn = false;
    return 1;
}
extern "C" void cui_reset(void) {
    rt = Runtime{};
    target = cui_canvas_t{};
}

extern "C" int cui_render(const cui_model_t *model, uint32_t now, int full, cui_canvas_t *canvas,
                          cui_rect_t *dirty, int max_dirty) {
    if (!rt.ready || !valid(canvas) || !model || !dirty || max_dirty <= 0)
        return 0;
    const auto &m = *model;
    int count = clamp(m.count, 0, CUI_MAX_CORES),
        selected = count ? clamp(m.selected, 0, count - 1) : 0;
    bool change = !rt.drawn || rt.revision != m.revision || rt.selection != selected ||
                  rt.count != count || rt.state != m.state || rt.error != m.error;
    bool output_change = rt.output != canvas->px || rt.stride != canvas->stride;
    int n = 0;
    if (change) {
        bool glide = rt.drawn && rt.state == CUI_MENU && m.state == CUI_MENU && rt.count == count;
        rt.marker_from = glide ? marker_at(now) : row_y(count, selected);
        rt.marker_to = row_y(count, selected);
        rt.marker_y = rt.marker_from;
        rt.start_ms = now;
        target = {rt.base, CUI_WIDTH, CUI_HEIGHT, CUI_WIDTH};
        background();
        if (m.state == CUI_MENU && count)
            menu_base(m, count, selected);
        else
            state_base(m, count, selected);
    }
    target = *canvas;
    if (change || full || output_change) {
        copy_base({0, 0, CUI_WIDTH, CUI_HEIGHT});
        dynamics(m, count, selected, now);
        dirty[n++] = {0, 0, CUI_WIDTH, CUI_HEIGHT};
    } else if (rt.last_ms != now || rt.progress != m.progress_percent) {
        if (m.state == CUI_MENU && count) {
            int new_y = marker_at(now), h = 62;
            cui_rect_t r{24, uint16_t(new_y < rt.marker_y ? new_y : rt.marker_y), 28,
                         uint16_t((new_y > rt.marker_y ? new_y : rt.marker_y) + h)};
            copy_base(r);
            add_dirty(r, dirty, max_dirty, n);
        }
        if (m.state != CUI_ERROR && !(m.state == CUI_MENU && count == 0)) {
            cui_rect_t r = indicator_rect(m, count);
            copy_base(r);
            add_dirty(r, dirty, max_dirty, n);
            dynamics(m, count, selected, now);
        }
    }
    rt.drawn = true;
    rt.revision = m.revision;
    rt.selection = selected;
    rt.count = count;
    rt.state = m.state;
    rt.error = m.error;
    rt.last_ms = now;
    rt.progress = m.progress_percent;
    rt.output = canvas->px;
    rt.stride = canvas->stride;
    return n;
}
