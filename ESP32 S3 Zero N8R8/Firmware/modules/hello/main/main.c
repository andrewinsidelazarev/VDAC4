#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

// Пробный модуль VDAC4: прошивка с ZIFI_ELF_PROBE запускает его при старте (src/elf_probe.cpp)
// и пишет итог в журнал наладки: «returned 41» — всё сошлось, 1000 + номер — первая проверка,
// которая не сошлась. argv[1] — буфер прошивки на 96 байтов: сюда модуль пишет подробности.
//
// С 0.1.122 (08.11.2026, задание TSafari) модуль проверяет и то, что прошивка даёт модулям:
//  - функции, добавленные в zm_symbols.cpp (математика double newlib, время, libgcc для
//    double ↔ uint64) — против значений, посчитанных на ПК (libm Windows): совпадение до 4 ulp;
//  - слабые символы: слабая ссылка на имя, которого в прошивке нет, — адрес 0, модуль при этом
//    загружается; слабая ссылка на имя, которое есть (cbrt), — его адрес;
//  - localtime_r — с часовым поясом прошивки (TZ из zifi.ini): модуль пишет свой сдвиг от UTC,
//    прошивка рядом — свою строку TZ.
// Входы — volatile: иначе компилятор посчитает функции сам, и их имена в модуль не попадут.

// Нет в прошивке и не будет: загрузчик обязан дать 0, а не отказать.
extern int vdac4_probe_absent(int) __attribute__((weak));
// Есть в прошивке (с 0.1.122): слабая ссылка обязана получить её адрес.
#pragma weak cbrt

static int failed;     // номер первой несошедшейся проверки (0 — все сошлись)
static int checks;

static void expect(int ok)
{
    ++checks;
    if (!ok && !failed) failed = checks;
}

// a и b равны с точностью до 4 единиц последнего знака b.
static int close_to(double a, double b)
{
    if (a == b) return 1;
    double ulp = ldexp(fabs(b), -52);
    return fabs(a - b) <= 4 * ulp;
}

int main(int argc, char *argv[])
{
    volatile double x03 = 0.3, x25 = 2.5, x07 = 0.7, x04 = 0.4, x13 = 1.3, x06 = 0.6, x2 = 2.0;
    volatile double small = 1e-5, h3 = 3e200, h4 = 4e200, xm37 = -3.7, xm25 = -2.5, xm27 = -2.7;
    volatile double x15 = 1.5, e19 = 1e19;
    volatile uint64_t big = 0xFFFFFFFFFFFFF800ull;

    // математика double (1–16)
    expect(close_to(acos(x03), 0x1.441f5ecbeef59p+0));
    expect(close_to(asin(x03), 0x1.380159e14f6ffp-2));
    expect(close_to(acosh(x25), 0x1.9119c13a31bafp+0));
    expect(close_to(asinh(x07), 0x1.4e2a4fe9085ddp-1));
    expect(close_to(atanh(x04), 0x1.b1d10670aae9ap-2));
    expect(close_to(cosh(x13), 0x1.f88dd5c279504p+0));
    expect(close_to(sinh(x13), 0x1.b2c9310045c64p+0));
    expect(close_to(tanh(x06), 0x1.12f8292d2ccfcp-1));
    expect(cbrt != 0 && close_to(cbrt(x2), 0x1.428a2f98d728ap+0));
    expect(close_to(expm1(small), 0x1.4f8bc681cdfb6p-17));
    expect(close_to(log1p(small), 0x1.4f8aea9ae7316p-17));
    expect(close_to(hypot(h3, h4), 0x1.a20df0dcd3af0p+666));            // без переполнения
    expect(lrint(x25) == 2 && lrint(xm37) == -4);                        // к чётному
    expect(round(x25) == 3.0 && round(xm25) == -3.0);
    expect(trunc(xm27) == -2.0);
    expect(scalbn(x15, 10) == 1536.0);

    // время (17–20)
    struct timeval tv;
    expect(gettimeofday(&tv, NULL) == 0);
    time_t t = 1700000000;                                               // 2023-11-14 22:13:20 UTC
    struct tm g, l;
    expect(gmtime_r(&t, &g) == &g && g.tm_year == 123 && g.tm_mon == 10 && g.tm_mday == 14 &&
           g.tm_hour == 22 && g.tm_min == 13 && g.tm_sec == 20 && g.tm_wday == 2 && g.tm_yday == 317);
    int zone = 0;
    if (localtime_r(&t, &l) == &l) {
        // сдвиг местного времени от UTC в часах (пояс прошивки — целые часы); 14 ноября —
        // середина года, через новый год сдвиг не переходит
        int minutes = (l.tm_yday - g.tm_yday) * 1440 + (l.tm_hour - g.tm_hour) * 60 + (l.tm_min - g.tm_min);
        zone = minutes / 60;
        expect(minutes % 60 == 0 && zone >= -12 && zone <= 14 && mktime(&l) == t);
    } else {
        expect(0);
    }
    expect(difftime(t, t - 6) == 6.0);

    // libgcc: double ↔ uint64 (21–22)
    expect((unsigned long long)e19 == 10000000000000000000ull);          // __fixunsdfdi
    expect((double)big == 0x1.fffffffffffffp+63);                        // __floatundidf

    // слабые символы (23): нет в прошивке — 0
    expect(vdac4_probe_absent == 0);
    if (vdac4_probe_absent) vdac4_probe_absent(1);                       // ссылка и через PLT

    if (argc > 1) {
        if (failed) snprintf(argv[1], 96, "%d checks, check %d failed, localtime UTC%+d", checks, failed, zone);
        else snprintf(argv[1], 96, "%d checks ok, localtime UTC%+d", checks, zone);
    }
    printf("hello from ELF: %d checks, first failed %d\n", checks, failed);
    return failed ? 1000 + failed : 41;
}
