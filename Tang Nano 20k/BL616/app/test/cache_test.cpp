// SPDX-License-Identifier: Apache-2.0
//
// Кэш ядер ПЛИС — обе стороны на ПК: код ESP (VDAC4/.../Firmware/src/core_cache_client.cpp — как
// есть) пишет команды 0xB7 в почтовый ящик модели ПЛИС (link_test.c с LINK_MODEL_ONLY: cp_link,
// регистры LINK), код BL616 (core_cache.c, fpga_link.c — как есть) читает их через модель SPI.
// Flash BL616 — массив 4 МБ, загрузка по JTAG и JTAG Reconfig — заглушки (образ сверяется).
// Ожидается: INFO, запись ядра (NEXT.BIN, если передан), FIND, LOAD — в «JTAG» ровно образ,
// BL616 ждёт просьбу ядра о перезагрузке и после неё — снова ядро VDAC4; отказ ESP до передачи
// для чужих образов; ошибка загрузки — ответ LOAD_FAIL из слота 0.
//
//   cache_test [NEXT.BIN]
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "zifi/core_cache_client.hpp"

extern "C" {
#include "core_cache.h"
#include "core_jtag.h"
#include "fpga_link.h"
#include "sha256.h"
uint32_t model_link_read(uint32_t a);
void model_link_write(uint32_t a, uint32_t v);
void model_set_version(uint32_t v);
uint32_t model_mbox(uint32_t i);
void model_set_boot(int on);
}

namespace {

uint32_t t_ms = 0;
std::vector<uint8_t> flash(4u << 20, 0xFF);
unsigned overwrite = 0;
int jtag_result = CORE_JTAG_OK;
std::vector<uint8_t> jtag_img;
unsigned reconfigs = 0;
bool cache_busy = false;

void bl_step()
{
    cache_busy = core_cache_poll(t_ms) != 0;
}

bool io_read(void*, uint32_t a, uint32_t* w, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        w[i] = model_link_read(a + 4 * static_cast<uint32_t>(i));
    return true;
}

bool io_write(void*, uint32_t a, const uint32_t* w, size_t n)
{
    if (n > 16)
        return false;
    for (size_t i = 0; i < n; ++i)
        model_link_write(a + 4 * static_cast<uint32_t>(i), w[i]);
    return true;
}

void io_sleep(void*, uint32_t ms)
{
    for (uint32_t i = 0; i < (ms ? ms : 1); ++i) {
        ++t_ms;
        bl_step();
    }
}

uint32_t io_now(void*) { return t_ms; }

const zifi::bl616::PushIo io = {nullptr, io_read, io_write, io_sleep, io_now, nullptr};

struct Mem {
    const std::vector<uint8_t>* img;
};
bool mem_read(void* ctx, uint32_t off, uint8_t* data, uint32_t n)
{
    const auto& v = *static_cast<Mem*>(ctx)->img;
    if (off + n > v.size())
        return false;
    memcpy(data, v.data() + off, n);
    return true;
}

std::vector<uint8_t> make_core(size_t n, uint32_t seed, int kind)
{
    static const uint8_t head[68] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xA5, 0xC3, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x1B,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x0B, 0x00, 0x00, 0x00, 0xD2, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
        0x3B, 0x80, 0x08, 0x3E};
    static const uint8_t tail[30] = {
        0x34, 0x73, 0x0A, 0x00, 0x00, 0x00, 0x56, 0x44, 0x34, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x08, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : v) {
        x = x * 1103515245u + 12345u;
        b = static_cast<uint8_t>(x >> 16);
    }
    memcpy(v.data(), head, sizeof head);
    memcpy(v.data() + n - sizeof tail, tail, sizeof tail);
    if (kind == 1) v[31] = 0x1C;                // IDCODE не GW2AR-18
    if (kind == 2) v[n - 30 + 8] = 0x00;         // нет метки VDAC4
    if (kind == 3) v[59] = 0x10;                // адрес перехода не 0
    return v;
}

int errors = 0;
void expect(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "  да " : "  НЕТ", what);
    errors += !ok;
}

}  // namespace

extern "C" {
uint32_t bflb_flash_get_size(void) { return static_cast<uint32_t>(flash.size()); }
int bflb_flash_erase(uint32_t a, uint32_t n)
{
    if (a % 4096 || n % 4096 || a + n > flash.size()) return -1;
    memset(flash.data() + a, 0xFF, n);
    return 0;
}
int bflb_flash_write(uint32_t a, uint8_t* d, uint32_t n)
{
    if (a + n > flash.size()) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        if (d[i] & ~flash[a + i]) ++overwrite;
        flash[a + i] &= d[i];
    }
    return 0;
}
int bflb_flash_read(uint32_t a, uint8_t* d, uint32_t n)
{
    if (a + n > flash.size()) return -1;
    memcpy(d, flash.data() + a, n);
    return 0;
}
int core_jtag_load(core_jtag_read_fn read, void* ctx, uint32_t size, uint32_t* usercode)
{
    jtag_img.assign(size, 0);
    for (uint32_t at = 0; at < size; at += 512) {
        const uint32_t n = size - at < 512 ? size - at : 512;
        if (read(ctx, at, jtag_img.data() + at, n) != 0) return CORE_JTAG_READ;
    }
    if (usercode) *usercode = 0x56443403u;
    return jtag_result;
}
void fpga_jtag_reconfig(void) { ++reconfigs; }
}

int main(int argc, char** argv)
{
    if (fpga_link_init() != 0) {
        printf("модель ПЛИС не ответила\n");
        return 1;
    }
    ftc_bus_t bus;
    fpga_bus(&bus);
    core_cache_init(&bus);
    io_sleep(nullptr, 200);

    std::vector<uint8_t> next;
    if (argc > 1) {
        FILE* f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        int c;
        while ((c = fgetc(f)) != EOF) next.push_back(static_cast<uint8_t>(c));
        fclose(f);
    } else {
        next = make_core(907418, 1, 0);
    }
    uint8_t sha[32], other[32];
    sha256(next.data(), next.size(), sha);
    memset(other, 0x5A, sizeof other);

    zifi::core_cache::Client client(io, 0x4000);

    // 1. INFO, FIND в пустом кэше
    {
        uint32_t mb = 0;
        const char* e = client.info(&mb);
        printf("INFO: %s, flash BL616 %u МБ\n", e ? e : "OK", static_cast<unsigned>(mb));
        expect(e == nullptr && mb == 4, "INFO — 4 МБ");
        zifi::core_cache::Found found = zifi::core_cache::Found::kFound;
        e = client.find("next", sha, &found);
        expect(e == nullptr && found == zifi::core_cache::Found::kNone, "FIND в пустом кэше — нет");
    }

    // 2. Запись ядра, FIND
    {
        Mem m{&next};
        const zifi::core_cache::ImageReader reader = {&m, mem_read};
        const uint32_t t0 = t_ms;
        const char* e = client.put("next", static_cast<uint32_t>(next.size()), sha, reader);
        printf("запись NEXT (%u байт): %s, %u мс модели\n", static_cast<unsigned>(next.size()), e ? e : "OK",
               static_cast<unsigned>(t_ms - t0));
        expect(e == nullptr, "запись без ошибки");
        expect(model_mbox(0) == 0, "заголовок ящика стёрт после записи");
        zifi::core_cache::Found found = zifi::core_cache::Found::kNone;
        e = client.find("NEXT", sha, &found);
        expect(e == nullptr && found == zifi::core_cache::Found::kFound, "FIND — то же ядро");
        e = client.find("Next", other, &found);
        expect(e == nullptr && found == zifi::core_cache::Found::kStale, "FIND с другим SHA-256 — другое");
        expect(overwrite == 0, "flash: запись только по стёртому");
    }

    // 3. LOAD: в «JTAG» — ровно образ; BL616 ждёт просьбы ядра о перезагрузке
    {
        const char* e = client.load("NEXT", sha);
        printf("LOAD NEXT: %s\n", e ? e : "LOADING");
        expect(e == nullptr, "LOAD принят");
        io_sleep(nullptr, 100);
        expect(jtag_img == next, "в ПЛИС по JTAG — ровно ядро из кэша");
        expect(cache_busy, "ядро из кэша работает: главный цикл BL616 — за кэшем");
        model_set_boot(1);
        io_sleep(nullptr, 30);
        expect(reconfigs == 1, "просьба о перезагрузке — JTAG Reconfig");
        model_set_boot(0);
        io_sleep(nullptr, 100);
        expect(!cache_busy, "после перезагрузки — снова ядро VDAC4");
    }

    // 4. Отказы ESP до передачи: чужие образы, имя
    {
        const uint32_t hdr = model_mbox(0);
        const char* names[3] = {"IDCODE", "VDAC4", "multiboot"};
        for (int kind = 1; kind <= 3; ++kind) {
            auto bad = make_core(300000, 10 + kind, kind);
            uint8_t s[32];
            sha256(bad.data(), bad.size(), s);
            Mem m{&bad};
            const zifi::core_cache::ImageReader reader = {&m, mem_read};
            const char* e = client.put("BAD", static_cast<uint32_t>(bad.size()), s, reader);
            printf("чужой образ (%s): %s\n", names[kind - 1], e ? e : "OK");
            expect(e != nullptr && strstr(e, names[kind - 1]) != nullptr, "отказ ESP до передачи");
        }
        expect(model_mbox(0) == hdr, "в ящик ничего не ушло");
        zifi::core_cache::Found found;
        expect(client.find("ДООМ", sha, &found) != nullptr, "имя не латиницей — отказ");
        expect(client.find("ABCDEFGHIJKLMNOP", sha, &found) != nullptr, "имя длиннее 15 — отказ");
        const char* e = client.load("NOPE", nullptr);
        expect(e != nullptr && strstr(e, "no such core") != nullptr, "LOAD несуществующего — нет такого ядра");
    }

    // 5. Загрузка не вышла: LOADING, затем из слота 0 — LOAD_FAIL | код
    {
        jtag_result = CORE_JTAG_NOT_DONE;
        const uint32_t before = reconfigs;
        const char* e = client.load("NEXT", nullptr);
        expect(e == nullptr, "LOAD принят (LOADING)");
        io_sleep(nullptr, 300);
        const uint32_t info = model_link_read(0x30);
        printf("после неудачной загрузки: перезагрузок %u, CP_INFO %08X\n", reconfigs - before,
               static_cast<unsigned>(info));
        expect(reconfigs == before + 1, "назад в слот 0 — JTAG Reconfig");
        expect((info >> 16) == ((0xC2u << 8) | (0x20u | CORE_JTAG_NOT_DONE)), "CP_INFO — LOAD_FAIL и код JTAG");
        expect(!cache_busy, "BL616 снова за ядром VDAC4");
        jtag_result = CORE_JTAG_OK;
    }

    // 6. Ядро Tang без ящика (1.7) — отказ ESP
    {
        model_set_version(0x01010107);
        uint32_t mb = 0;
        const char* e = client.info(&mb);
        expect(e != nullptr && strstr(e, "mailbox") != nullptr, "ядро 1.7 — нет ящика");
        model_set_version(0x0101011E);
    }

    printf("%s\n", errors ? "ТЕСТ НЕ ПРОЙДЕН" : "ТЕСТ ПРОЙДЕН");
    return errors != 0;
}
