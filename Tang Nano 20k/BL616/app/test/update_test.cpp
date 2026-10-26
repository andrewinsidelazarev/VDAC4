// SPDX-License-Identifier: Apache-2.0
//
// Обновление прошивки BL616 по сети — обе стороны на ПК: код ESP (VDAC4/.../Firmware/src/
// bl616_push.cpp — как есть) пишет образ в почтовый ящик модели ПЛИС (link_test.c с
// LINK_MODEL_ONLY: cp_link, регистры LINK), код BL616 (fw_update.c, fpga_link.c — как есть)
// читает его через модель SPI. Запись во flash и перезагрузка — заглушка: образ копируется
// в «flash», затем BL616 начинает заново, как после сброса, с другой версией в CP_INFO.
//
//   update_test
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "zifi/bl616_push.hpp"

extern "C" {
#include "fpga_link.h"
#include "fw_update.h"
#include "sha256.h"
uint32_t model_link_read(uint32_t a);
void model_link_write(uint32_t a, uint32_t v);
void model_set_version(uint32_t v);
uint32_t model_mbox(uint32_t i);
}

namespace {

uint32_t t_ms = 0;
std::vector<uint8_t> flash;
int commits = 0;
bool rebooting = false;
int fw_version = 2;
bool updater = true;          // false — прошивка без обновления по сети (версия 1)
bool running = true;          // false — BL616 молчит
uint32_t t_info = 0, beat = 0;
ftc_bus_t bus;

void bl_boot()
{
    fpga_bus(&bus);
    if (updater)
        fw_update_init(&bus);
    t_info = t_ms;
}

// Один оборот главного цикла main.c (без сопроцессора: ему в тесте нечего делать)
void bl_step()
{
    if (!running)
        return;
    if (rebooting) {                 // «перезагрузка» после записи — новая прошивка
        rebooting = false;
        fw_version = 3;
        bl_boot();
    }
    if (updater && fw_update_poll(t_ms))
        return;
    if (t_ms - t_info >= 500 && !(updater && fw_update_owns_info(t_ms))) {
        t_info = t_ms;
        fpga_info(0xC0000000u | (static_cast<uint32_t>(fw_version) << 16) | (beat++ & 0xFF));
    }
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
        return false;                // tang::write — не больше 16 слов
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

uint32_t io_now(void*)
{
    return t_ms;
}

const zifi::bl616::PushIo io = {nullptr, io_read, io_write, io_sleep, io_now, nullptr};

std::vector<uint8_t> make_image(size_t size, uint32_t seed)
{
    std::vector<uint8_t> img(size);
    uint32_t x = seed;
    for (auto& b : img) {
        x = x * 1103515245u + 12345u;
        b = static_cast<uint8_t>(x >> 16);
    }
    memcpy(img.data(), "BFNP", 4);
    return img;
}

void settle(uint32_t ms)
{
    io_sleep(nullptr, ms);
}

int errors = 0;
void expect(bool ok, const char* what)
{
    printf("%s %s\n", ok ? "  да " : "  НЕТ", what);
    errors += !ok;
}

}  // namespace

extern "C" void fw_update_commit(const uint8_t* img, uint32_t size)
{
    flash.assign(img, img + size);
    ++commits;
    rebooting = true;
}

int main()
{
    if (fpga_link_init() != 0) {
        printf("модель ПЛИС не ответила\n");
        return 1;
    }
    bl_boot();
    settle(1200);

    // 1. обновление проходит
    {
        auto img = make_image(70001, 1);
        uint8_t digest[32];
        sha256(img.data(), img.size(), digest);
        uint32_t info = 0;
        const uint32_t t0 = t_ms;
        const char* e = zifi::bl616::push(io, img.data(), img.size(), digest, 0x1234, &info);
        printf("обновление 70001 байт: %s, %u мс модели, CP_INFO %08X\n", e ? e : "OK",
               static_cast<unsigned>(t_ms - t0), static_cast<unsigned>(info));
        expect(e == nullptr, "push без ошибки");
        expect(commits == 1 && flash == img, "во «flash» — ровно образ");
        expect((info >> 24) == 0xC0 && ((info >> 16) & 0xFF) == 3, "отвечает новая прошивка (версия 3)");
        expect(model_mbox(0) == 0, "заголовок ящика стёрт");
    }
    settle(4000);

    // 1б. образ — в готовый буфер (прошивка 11: память 3D, сопроцессор стоит), больше кучи
    //     прошивки 9 (~76 КБ)
    {
        static uint8_t fixed[160 * 1024];
        fw_update_set_buffer(fixed, sizeof fixed);
        auto img = make_image(150001, 8);
        uint8_t digest[32];
        sha256(img.data(), img.size(), digest);
        uint32_t info = 0;
        const char* e = zifi::bl616::push(io, img.data(), img.size(), digest, 0x1500, &info);
        printf("обновление 150001 байт в готовый буфер: %s\n", e ? e : "OK");
        expect(e == nullptr && commits == 2 && flash == img, "образ собран в готовом буфере и записан");
        expect(memcmp(fixed, img.data(), img.size()) == 0, "буфер — тот самый");
        // на ПК запись во «flash» возвращается — буфер отмечен; на BL616 после неё перезагрузка
        fw_update_clobbered();
    }
    settle(4000);

    // 2. SHA-256 не тот: BL616 не пишет flash
    {
        auto img = make_image(9000, 2);
        uint8_t digest[32];
        sha256(img.data(), img.size() - 1, digest);
        uint32_t info = 0;
        const char* e = zifi::bl616::push(io, img.data(), img.size(), digest, 0x2000, &info);
        printf("неверный SHA-256: %s\n", e ? e : "OK");
        expect(e != nullptr && strstr(e, "SHA-256") != nullptr, "отказ BL616 по SHA-256");
        expect(commits == 2, "flash не тронут");
    }
    settle(4000);

    // 3. образ не BL616 / слишком большой — отказ ESP до передачи
    {
        auto img = make_image(9000, 3);
        img[0] = 'X';
        uint8_t digest[32];
        sha256(img.data(), img.size(), digest);
        uint32_t info = 0;
        const char* e = zifi::bl616::push(io, img.data(), img.size(), digest, 1, &info);
        expect(e != nullptr && strstr(e, "BFNP") != nullptr, "без BFNP — отказ ESP");
        auto big = make_image(300 * 1024, 4);
        sha256(big.data(), big.size(), digest);
        e = zifi::bl616::push(io, big.data(), big.size(), digest, 1, &info);
        expect(e != nullptr && strstr(e, "256 KB") != nullptr, "больше 256 КБ — отказ ESP");
    }

    // 4. ядро Tang 1.7 — ящика нет
    {
        model_set_version(0x01010107);
        auto img = make_image(9000, 5);
        uint8_t digest[32];
        sha256(img.data(), img.size(), digest);
        uint32_t info = 0;
        const char* e = zifi::bl616::push(io, img.data(), img.size(), digest, 1, &info);
        expect(e != nullptr && strstr(e, "1.8") != nullptr, "ядро 1.7 — отказ");
        model_set_version(0x01010108);
    }

    // 5. прошивка BL616 без обновления по сети (как версия 1): ответа нет
    {
        updater = false;
        fw_version = 1;
        bl_boot();
        settle(1200);
        auto img = make_image(9000, 6);
        uint8_t digest[32];
        sha256(img.data(), img.size(), digest);
        uint32_t info = 0;
        const char* e = zifi::bl616::push(io, img.data(), img.size(), digest, 0x3000, &info);
        printf("прошивка без обновления: %s\n", e ? e : "OK");
        expect(e != nullptr && strstr(e, "does not answer") != nullptr, "нет ответа — понятная ошибка");
        expect(commits == 2, "flash не тронут");
    }

    // 6. старый заголовок BEGIN в ящике при загрузке BL616 — не срабатывает
    {
        updater = true;
        fw_version = 2;
        model_link_write(0x34, 0);
        model_link_write(0x204, 9000);
        model_link_write(0x200, 0xB6014444u);
        bl_boot();
        settle(1500);
        const uint32_t info = model_link_read(0x30);
        expect((info >> 24) == 0xC0, "после загрузки старый BEGIN не начал обновление");
    }

    // 7. BEGIN без продолжения — через 10 с BL616 бросает обновление и снова пишет C0
    {
        model_link_write(0x34, 0);
        model_link_write(0x204, 9000);
        model_link_write(0x200, 0xB6015555u);
        settle(500);
        expect((model_link_read(0x30) >> 24) == 0xC1, "BEGIN принят");
        settle(15000);
        expect((model_link_read(0x30) >> 24) == 0xC0, "через 10 с без команд — снова обычная работа");
        expect(fw_update_clobbered() && !fw_update_clobbered(), "брошенное обновление испортило готовый буфер — один раз");
    }

    // 8. BL616 молчит
    {
        running = false;
        model_link_write(0x34, 0);
        uint32_t info = 0;
        auto img = make_image(9000, 7);
        uint8_t digest[32];
        sha256(img.data(), img.size(), digest);
        // CP_INFO от прошлой прошивки ещё C0: ESP узнает о беде по тайм-ауту ответа
        const char* e = zifi::bl616::push(io, img.data(), img.size(), digest, 0x6000, &info);
        expect(e != nullptr, "BL616 не работает — ошибка");
    }

    printf(errors ? "ТЕСТ НЕ ПРОЙДЕН\n" : "ТЕСТ ПРОЙДЕН\n");
    return errors != 0;
}
