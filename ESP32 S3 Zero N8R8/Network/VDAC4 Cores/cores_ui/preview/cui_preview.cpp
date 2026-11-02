// Стенд ПК: вызывает ту же отрисовку и сохраняет RGB565 без дорисовки.
#include "cores_ui.h"
#include "tests/card_fixtures.h"
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
uint16_t canvas_data[CUI_WIDTH * CUI_HEIGHT];
uint16_t cache_data[CUI_WIDTH * CUI_HEIGHT];
cui_canvas_t canvas{canvas_data, CUI_WIDTH, CUI_HEIGHT, CUI_WIDTH};
FILE *metrics = nullptr;
void *allocate(size_t bytes, int psram, void *) {
    return bytes == sizeof(cache_data) && psram ? cache_data : nullptr;
}
cui_model_t standard() {
    cui_model_t m{};
    m.count = 3;
    m.state = CUI_MENU;
    m.progress_percent = -1;
    m.revision = 1;
    const char *names[] = {"NES", "NEXT", "AMIGA"};
    for (int i = 0; i < 3; ++i) {
        snprintf(m.cores[i].name, sizeof(m.cores[i].name), "%s", names[i]);
        m.cores[i].size_bytes = 886 * 1024;
        m.cores[i].own_esp_layer = 1;
        m.cores[i].card = fixtures::cards[i];
    }
    return m;
}
void render_save(const char *name, const cui_model_t &m, uint32_t now, int full = 0) {
    cui_rect_t dirty[8];
    auto before = std::chrono::steady_clock::now();
    int count = cui_render(&m, now, full, &canvas, dirty, 8);
    auto after = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
    unsigned area = 0;
    bool is_full = false;
    for (int i = 0; i < count; ++i) {
        area += (dirty[i].x1 - dirty[i].x0) * (dirty[i].y1 - dirty[i].y0);
        if (dirty[i].x0 == 0 && dirty[i].y0 == 0 && dirty[i].x1 == 640 && dirty[i].y1 == 360)
            is_full = true;
    }
    fprintf(metrics, "%s,%lu,%d,%d,%u,%lld\n", name, static_cast<unsigned long>(now), count,
            is_full ? 1 : 0, area, static_cast<long long>(ns));
    char path[160];
    snprintf(path, sizeof(path), "raw/%s.rgb565", name);
    FILE *out = fopen(path, "wb");
    if (!out || fwrite(canvas_data, sizeof(uint16_t), CUI_WIDTH * CUI_HEIGHT, out) !=
                    CUI_WIDTH * CUI_HEIGHT) {
        fprintf(stderr, "Cannot write %s\n", path);
        exit(2);
    }
    fclose(out);
}
void still(const char *name, cui_model_t m) {
    cui_init(&canvas, allocate, nullptr);
    cui_rect_t dirty[8];
    cui_render(&m, 0, 1, &canvas, dirty, 8);
    render_save(name, m, 760, 1);
}
void benchmark() {
    cui_model_t m = standard();
    cui_rect_t dirty[8];
    cui_init(&canvas, allocate, nullptr);
    cui_render(&m, 0, 1, &canvas, dirty, 8);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; ++i)
        cui_render(&m, 1000 + uint32_t(i) * 40, 0, &canvas, dirty, 8);
    auto ordinary = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        ++m.revision;
        cui_render(&m, 90000 + uint32_t(i) * 40, 1, &canvas, dirty, 8);
    }
    auto finish = std::chrono::steady_clock::now();
    auto ns1 =
        std::chrono::duration_cast<std::chrono::nanoseconds>(ordinary - start).count() / 2000;
    auto ns2 =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - ordinary).count() / 100;
    FILE *f = fopen("benchmark.json", "wb");
    if (!f)
        exit(2);
    fprintf(f,
            "{\"ordinary_average_ns\":%lld,\"full_rebuild_average_ns\":%lld,\"ordinary_frames\":"
            "2000,\"full_frames\":100}\n",
            static_cast<long long>(ns1), static_cast<long long>(ns2));
    fclose(f);
}
}

int main() {
    fixtures::load();
    metrics = fopen("preview_metrics.csv", "wb");
    if (!metrics)
        return 2;
    fprintf(metrics, "name,now_ms,rectangles,full,area,render_ns\n");
    auto m = standard();
    still("01_menu_nes", m);
    m.selected = 1;
    still("02_menu_next", m);
    m.selected = 2;
    still("03_menu_amiga", m);
    const char *many[] = {"NES", "NEXT",     "AMIGA",           "C64",     "MSX",     "SMS",
                          "PCE", "ARCADE-X", "MY_CORE_2026.01", "VECTREX", "DEMO_01", "TEST-CORE"};
    m = standard();
    m.count = 12;
    m.selected = 8;
    for (int i = 0; i < 12; ++i) {
        snprintf(m.cores[i].name, sizeof(m.cores[i].name), "%s", many[i]);
        m.cores[i].size_bytes = 886 * 1024;
        m.cores[i].own_esp_layer = i != 8;
    }
    still("04_many_unknown_shared", m);
    m.selected = 3;
    still("05_many_future_c64", m);
    m = standard();
    m.count = 1;
    still("06_single_core", m);
    m = standard();
    m.count = 0;
    m.state = CUI_CONNECTING;
    still("07_connecting", m);
    m.state = CUI_READING;
    still("08_reading_sd", m);
    m = standard();
    m.state = CUI_STARTING_ESP;
    still("09_starting_esp", m);
    m.state = CUI_COPYING_CACHE;
    m.progress_percent = 45;
    still("10_cache_45_percent", m);
    m.state = CUI_LOADING;
    m.progress_percent = -1;
    still("11_loading_unknown", m);
    m.state = CUI_PROGRAMMING;
    still("12_programming_fpga", m);
    m.state = CUI_ERROR;
    m.error = CUI_ERR_NO_CORES;
    m.count = 0;
    still("13_error_no_cores", m);
    m = standard();
    m.state = CUI_ERROR;
    m.error = CUI_ERR_ESP_TEXT;
    snprintf(m.error_text, sizeof(m.error_text),
             "ERR: SD read failed while opening /CORES/NES.BIN (code 04)");
    still("14_error_esp_text", m);
    m.error = CUI_ERR_CORE;
    still("15_error_core", m);
    m.error = CUI_ERR_UPDATE_NES;
    still("16_error_update_nes", m);
    m.error = CUI_ERR_NO_SD;
    m.count = 0;
    still("17_error_no_sd", m);
    m = standard();
    m.state = CUI_ERROR;
    m.error = CUI_ERR_ESP_LAYER;
    still("18_error_esp_layer", m);
    m.error = CUI_ERR_TIMEOUT;
    still("19_error_timeout", m);
    m.error = CUI_ERR_ESP_LOST;
    still("20_error_esp_lost", m);
    m.error = CUI_ERR_NO_ESP;
    m.count = 0;
    still("21_error_no_esp", m);
    m.error = CUI_ERR_NO_ANSWER;
    still("22_error_no_answer", m);

    m = standard();
    m.count = 1;
    snprintf(m.cores[0].name, sizeof(m.cores[0].name), "CUSTOM_CORE");
    m.cores[0].card = nullptr;
    still("27_no_card_no_manage", m);

    cui_init(&canvas, allocate, nullptr);
    m = standard();
    for (int frame = 0; frame < 100; ++frame) {
        uint32_t now = uint32_t(frame) * 40;
        m.selected = frame < 14 ? 0 : frame < 28 ? 1 : frame < 41 ? 2 : frame < 53 ? 1 : 0;
        m.state = frame < 65   ? CUI_MENU
                  : frame < 76 ? CUI_STARTING_ESP
                  : frame < 88 ? CUI_COPYING_CACHE
                               : CUI_PROGRAMMING;
        char name[32];
        snprintf(name, sizeof(name), "motion_%03d", frame);
        render_save(name, m, now);
    }
    fclose(metrics);
    benchmark();
    printf("23 still frames, 100 animation frames; RGB565 and metrics saved.\n");
    return 0;
}
