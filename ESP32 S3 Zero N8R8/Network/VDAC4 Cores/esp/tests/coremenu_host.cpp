// Стенд ПК для COREMENU.ELF (экран плагина VDAC4 Cores): приложение целиком (main/app.c с
// cores_ui и разбором карточек) на модели ядра ESP. LINK пишет в память Tang (регион оверлеев и
// регистры оверлея 1), карта Tang — папка с \CORES\ (ELF ядер с карточками), время — шагами по
// 20 мс, как у задачи экрана. Проверяется: копия холста в Tang после каждого шага совпадает с
// холстом точка в точку; появление по ступени прозрачности; карточки из ELF; «Loading» остаётся
// после выгрузки (LEAVE 1); ошибка после этого — сразу непрозрачная; уход плавно (LEAVE 0),
// выгрузка (LEAVE 2); молчание плагина; перезагрузка ПЛИС. Каждый ELF проходит проверку прошивки
// elfImageProblem (как CORE_APP). Запуск — tests/run_host.ps1; кадры — PPM в папку вывода.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "zifi/elf_image.hpp"
#include "zm_app.h"

extern "C" int coremenu_main(int argc, char **argv);
extern "C" void coremenu_host_step(uint32_t now);
extern "C" const uint16_t *coremenu_host_canvas(void);

namespace {

constexpr uint32_t kRegion = 131072, kStride = 320, kBase = kRegion - kStride * 360;
constexpr uint32_t kCtrl = 0xD0;

std::string sdDir, outDir;
unsigned listCount = 4;               // ядер в списке: третий параметр (1…4)
uint32_t clockMs = 1000;
std::vector<uint32_t> tang(kRegion);
uint32_t regs[4];                        // CTRL, POS, SIZE, BASE оверлея 1
std::vector<uint32_t> ctrlLog;           // каждая запись CTRL
bool uiOk = true, appDone = false;
size_t ctrlAtStart = 0;                  // записей CTRL до запуска этого экземпляра
uint8_t replyCode;
std::vector<uint8_t> replyData;
std::vector<std::string> logs;
int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++failures;
        printf("FAIL: %s\n", what);
    }
}

void *hAlloc(size_t size, uint32_t) { return calloc(1, size); }
void *hRealloc(void *p, size_t size) { return realloc(p, size); }
void hFree(void *p) { free(p); }
void hLog(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    logs.push_back(line);
    printf("  log: %s\n", line);
}
uint32_t hMillis() { return clockMs; }
void hDelay(uint32_t ms) { clockMs += ms; }
int hTaskStart(zm_task_fn, void *, uint32_t, uint8_t, int8_t) { return ZM_OK; }
int dummy;
void *hMutexCreate() { return &dummy; }
void hMutex(void *) {}
int hReply(uint8_t response, const void *data, uint16_t length)
{
    replyCode = response;
    replyData.assign((const uint8_t *)data, (const uint8_t *)data + length);
    return ZM_OK;
}
int hUiAvailable() { return uiOk ? 1 : 0; }
void hAppDone() { appDone = true; }

// ABI 1.14: что экран оставил слою ядра
std::string handoffTo;
void *handoffBlock = nullptr;
uint32_t handoffSize = 0;
int hHandoffPut(const char *to, void *block, uint32_t size)
{
    handoffTo = to;
    handoffBlock = block;
    handoffSize = size;
    return ZM_OK;
}
void *hHandoffTake(uint32_t *) { return nullptr; }
int mediaFirst = 1, mediaLast = 0;      // link_media (ABI 1.13): порции DMA каким командам
int hLinkMedia(uint8_t first, uint8_t last)
{
    mediaFirst = first;
    mediaLast = last;
    return ZM_OK;
}

// Порция RAW, как её отдаёт прошивка: свои байты запроса [смещение LE32][XOR], за ними данные
std::vector<uint8_t> rawPortion(uint32_t at, const uint8_t *data, size_t n)
{
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++)
        x ^= data[i];
    std::vector<uint8_t> p{(uint8_t)at, (uint8_t)(at >> 8), (uint8_t)(at >> 16), (uint8_t)(at >> 24), x};
    p.insert(p.end(), data, data + n);
    return p;
}

int hLinkWrite(uint32_t address, const uint32_t *words, uint32_t count)
{
    if (!uiOk || count == 0 || count > 16 || address % 4)
        return ZM_ERROR;
    if (address >= 0x80000 && address + 4 * count <= 0x80000 + 4 * kRegion) {
        memcpy(&tang[(address - 0x80000) / 4], words, 4 * count);
        return ZM_OK;
    }
    if (address >= kCtrl && address + 4 * count <= kCtrl + 16) {
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t r = (address - kCtrl) / 4 + i;
            regs[r] = words[i];
            if (r == 0)
                ctrlLog.push_back(words[i]);
        }
        return ZM_OK;
    }
    return ZM_ERROR;
}

struct SdFile {
    FILE *f;
};
int hSdOpen(const char *path, int writable, uint32_t *sectors, void **file)
{
    if (writable)
        return ZM_ERROR;
    FILE *f = fopen((sdDir + path).c_str(), "rb");
    if (!f)
        return ZM_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    if (size == 0 || size % 512 != 0) {  // как tang_sd::diskOpen: только файлы-диски
        fclose(f);
        return ZM_ERROR;
    }
    *sectors = (uint32_t)(size / 512);
    *file = new SdFile{f};
    return ZM_OK;
}

// Как прошивка (ABI 1.7): файл целиком в память, не длиннее max_size; free — через core->free
int hSdRead(const char *path, uint8_t **data, size_t *size, size_t max_size, const volatile int *active)
{
    *data = nullptr;
    *size = 0;
    if (active != nullptr && !*active)
        return ZM_ERROR;
    FILE *f = fopen((sdDir + path).c_str(), "rb");
    if (!f)
        return ZM_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    const long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (length <= 0 || (size_t)length > max_size) {
        fclose(f);
        return ZM_ERROR;
    }
    *data = static_cast<uint8_t *>(malloc((size_t)length));
    *size = fread(*data, 1, (size_t)length, f);
    fclose(f);
    return ZM_OK;
}
int hSdPread(void *file, uint32_t sector, uint8_t *data, uint32_t count)
{
    FILE *f = static_cast<SdFile *>(file)->f;
    memset(data, 0, 512 * count);
    fseek(f, (long)sector * 512, SEEK_SET);
    fread(data, 1, 512 * count, f);
    return ZM_OK;
}
void hSdClose(void *file)
{
    fclose(static_cast<SdFile *>(file)->f);
    delete static_cast<SdFile *>(file);
}

zm_core_t makeCore()
{
    zm_core_t c;
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    c.abi_major = ZM_APP_ABI_MAJOR;
    c.abi_minor = ZM_APP_ABI_MINOR;
    c.firmware = "vdac4-host";
    c.alloc = hAlloc;
    c.realloc = hRealloc;
    c.free = hFree;
    c.log = hLog;
    c.millis = hMillis;
    c.delay = hDelay;
    c.task_start = hTaskStart;
    c.mutex_create = hMutexCreate;
    c.mutex_lock = hMutex;
    c.mutex_unlock = hMutex;
    c.reply = hReply;
    c.link_write = hLinkWrite;
    c.ui_available = hUiAvailable;
    c.app_done = hAppDone;
    c.sd_open = hSdOpen;
    c.sd_read = hSdRead;
    c.sd_pread = hSdPread;
    c.sd_close = hSdClose;
    c.handoff_put = hHandoffPut;
    c.handoff_take = hHandoffTake;
    c.link_media = hLinkMedia;
    return c;
}

uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)*p++ << 8;
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

// Пакет DATA, как у плагина: [смещение LE32][данные][CRC16 смещения и данных LE]
std::vector<uint8_t> packet(uint32_t at, const uint8_t *data, size_t n)
{
    std::vector<uint8_t> p{(uint8_t)at, (uint8_t)(at >> 8), (uint8_t)(at >> 16), (uint8_t)(at >> 24)};
    p.insert(p.end(), data, data + n);
    const uint16_t crc = crc16(p.data(), p.size());
    p.push_back((uint8_t)crc);
    p.push_back((uint8_t)(crc >> 8));
    return p;
}

zm_core_t core;
const zm_app_t *app;

uint8_t command(uint8_t cmd, const std::vector<uint8_t> &data)
{
    replyCode = 0;
    replyData.clear();
    check(app->command(cmd, data.data(), (uint16_t)data.size()) == ZM_OK, "команда своя");
    check(replyCode == (cmd | 0x80) && !replyData.empty(), "ответ — код | 0x80");
    return replyData.empty() ? 0 : replyData[0];
}

std::vector<uint8_t> listBody()
{
    struct { const char *name; uint32_t size; uint8_t elf; } cores[] = {
        {"NES", 907418, 1}, {"NEXT", 907418, 1}, {"AMIGA", 907418, 1}, {"DEMO", 300000, 0}};
    std::vector<uint8_t> body{(uint8_t)listCount};
    for (unsigned i = 0; i < listCount; i++) {
        auto &c = cores[i];
        uint8_t e[21] = {};
        memcpy(e, c.name, strlen(c.name));
        for (int i = 0; i < 4; i++)
            e[16 + i] = (uint8_t)(c.size >> 8 * i);
        e[20] = c.elf;
        body.insert(body.end(), e, e + 21);
    }
    return body;
}

std::vector<uint8_t> state(uint8_t st, uint8_t sel, uint8_t err = 0, const char *text = "")
{
    std::vector<uint8_t> s{st, sel, 0xFF, err};
    s.insert(s.end(), text, text + strlen(text));
    s.push_back(0);
    return s;
}

// Копия в Tang совпадает с холстом
bool tangMatches()
{
    const uint16_t *px = coremenu_host_canvas();
    if (px == nullptr)
        return false;
    for (uint32_t y = 0; y < 360; y++)
        for (uint32_t x = 0; x < 640; x += 2) {
            const uint32_t w = tang[kBase + y * kStride + x / 2];
            if ((w & 0xFFFF) != px[y * 640 + x] || (w >> 16) != px[y * 640 + x + 1])
                return false;
        }
    return true;
}

void run(uint32_t ms, bool compare = true)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        clockMs += 20;
        coremenu_host_step(clockMs);
        // Сверять — с первой записи CTRL этого экземпляра: до неё на HDMI может держаться кадр
        // прежнего («Loading» после LEAVE 1), а холст нового ещё пуст
        if (compare && uiOk && (regs[0] & 1) && ctrlLog.size() > ctrlAtStart && !tangMatches()) {
            check(false, "копия в Tang = холст после каждого шага");
            return;
        }
    }
}

uint8_t alpha() { return (uint8_t)(regs[0] >> 4 & 15); }

void savePpm(const char *name)
{
    if (coremenu_host_canvas() == nullptr)
        return;
    FILE *f = fopen((outDir + "/" + name).c_str(), "wb");
    if (!f)
        return;
    fprintf(f, "P6\n640 360\n255\n");
    const uint16_t *px = coremenu_host_canvas();
    for (int i = 0; i < 640 * 360; i++) {
        const uint16_t p = px[i];
        const uint8_t rgb[3] = {(uint8_t)((p >> 11) * 255 / 31), (uint8_t)((p >> 5 & 63) * 255 / 63),
                                (uint8_t)((p & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int countLogs(const char *needle)
{
    int n = 0;
    for (auto &l : logs)
        n += l.find(needle) != std::string::npos;
    return n;
}

void startApp()
{
    appDone = false;
    ctrlAtStart = ctrlLog.size();
    check(app->start(&core) == ZM_OK, "start");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: coremenu_host SD_DIR OUT_DIR\n");
        return 2;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);   // падение не съедает вывод
    sdDir = argv[1];
    outDir = argv[2];
    if (argc > 3 && atoi(argv[3]) >= 1 && atoi(argv[3]) <= 4)
        listCount = (unsigned)atoi(argv[3]);
    core = makeCore();

    // ELF ядер с карточками и сам экран — годны для CORE_APP прошивки
    for (const char *name : {"NES", "NEXT", "AMIGA", "COREMENU"}) {
        FILE *f = fopen((sdDir + "/CORES/" + name + ".ELF").c_str(), "rb");
        check(f != nullptr, "ELF на карте");
        if (!f)
            continue;
        std::vector<uint8_t> data;
        uint8_t buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            data.insert(data.end(), buf, buf + n);
        fclose(f);
        const char *problem = zifi::elfImageProblem(data.data(), data.size());
        printf("%s.ELF: %zu bytes, elfImageProblem: %s\n", name, data.size(), problem ? problem : "none");
        check(problem == nullptr, "ELF проходит проверку прошивки");
    }

    char a0[] = "COREMENU", a1[] = "", *args[3] = {a0, a1, nullptr};
    const zm_app_t *table = nullptr;
    args[2] = reinterpret_cast<char *>(&table);
    check(coremenu_main(3, args) == ZM_APP_ABI_MAJOR && table != nullptr, "main отдаёт таблицу");
    app = table;
    check(app->command_first == 0x47 && app->command_last == 0x4D,
          "команды 47…4D (0.2: 4A…4C — приём файла; 0.3: 4D — порции DMA)");

    // 1. Меню: список, STATE, карточки из ELF, появление по ступени
    startApp();
    check(command(0x47, listBody()) == 1, "LIST принят");
    uint8_t f = command(0x48, state(2, 0));
    run(100);
    check(countLogs("card found") == (int)(listCount < 3 ? listCount : 3),
          "карточки из ELF ядер (длина ELF не кратна 512)");
    check(countLogs("DEMO") == 0, "у ядра без ELF карточку не ищут");
    check(ctrlLog.size() >= 1 && (ctrlLog[0] & 3) == 1 && (ctrlLog[0] >> 4 & 15) == 0,
          "оверлей включён ×1 с прозрачностью 0: холст уже весь в Tang");
    check(regs[1] == (320u | 180u << 16) && regs[2] == (640u | 360u << 16) && regs[3] == kBase,
          "по центру 720p, размер, начало");
    run(400);
    check(alpha() == 15, "проявился до непрозрачного");
    bool steps = true;
    for (size_t i = 1; i < ctrlLog.size() && i <= 15; i++)
        steps = steps && (ctrlLog[i] >> 4 & 15) == (ctrlLog[i - 1] >> 4 & 15) + 1;
    check(steps, "по ступени за шаг");
    f = command(0x48, state(2, 0));
    run(40);
    f = command(0x48, state(2, 0));
    check((f & 0x1F) == (1 | 2 | 4 | 16), "флаги: Tang, виден, нарисован, карточки");
    savePpm("01_menu_nes.ppm");
    if (listCount > 1) {
        command(0x48, state(2, 1));
        run(400);
        savePpm("02_menu_next.ppm");
    }
    if (listCount > 2) {
        command(0x48, state(2, 2));
        run(400);
        savePpm("03_menu_amiga.ppm");
    }
    if (listCount > 3) {
        command(0x48, state(2, 3));
        run(400);
        savePpm("04_menu_demo.ppm");
    }

    // 2. Запуск ядра: «Loading», LEAVE 1, выгрузка — картинка остаётся
    command(0x48, state(5, 1));
    f = 0;
    for (int i = 0; i < 50 && (f & 6) != 6; i++) {
        f = command(0x49, {1});
        run(20);
    }
    check((f & 6) == 6, "LEAVE 1: кадр на экране");
    savePpm("05_loading.ppm");
    const size_t ctrlBefore = ctrlLog.size();
    run(200);
    check(ctrlLog.size() == ctrlBefore, "оставленная картинка не меняется");
    app->stop();
    check(ctrlLog.size() == ctrlBefore && (regs[0] & 1) && alpha() == 15, "выгрузка после LEAVE 1 не гасит экран");

    // 3. Ошибка после запуска: новый экран — сразу непрозрачный, с текстом
    startApp();
    command(0x47, listBody());
    command(0x48, state(7 | 0x80, 1, 6, "ERR: core not found in the BL616 cache      "));
    const size_t errFrom = ctrlLog.size();
    run(200);
    check(ctrlLog.size() > errFrom && (ctrlLog[errFrom] >> 4 & 15) == 15, "сразу непрозрачный (#80)");
    savePpm("06_error.ppm");
    // уход плавно и выгрузка
    for (int i = 0; i < 40; i++) {
        f = command(0x49, {0});
        run(20);
        if (f & 8)
            break;
    }
    check((f & 8) && !(regs[0] & 1), "LEAVE 0: ушёл и выключен");
    command(0x49, {2});
    check(appDone, "LEAVE 2: app_done");
    app->stop();

    // 4. Молчание плагина (сброс Evo) — экран уходит сам
    startApp();
    command(0x47, listBody());
    command(0x48, state(2, 0));
    run(600);
    check(alpha() == 15 && (regs[0] & 1), "виден");
    run(3600, false);
    check(!(regs[0] & 1), "молчание 3 с — экран ушёл");
    app->stop();

    // 5. Перезагрузка ПЛИС: Tang пропал и вернулся — холст заново, копия совпадает
    startApp();
    command(0x47, listBody());
    command(0x48, state(2, 0));
    run(600);
    uiOk = false;
    std::fill(tang.begin(), tang.end(), 0);
    regs[0] = 0;
    for (int i = 0; i < 5; i++) {
        command(0x48, state(2, 0));
        run(20, false);
    }
    uiOk = true;
    for (int i = 0; i < 30; i++) {
        command(0x48, state(2, 0));
        run(20);
    }
    check((regs[0] & 1) && alpha() == 15 && tangMatches(), "после перезагрузки — снова весь холст");
    // 6. Выгрузка без LEAVE 1 (чужой APP_STOP) — экран гаснет сразу
    app->stop();
    check(!(regs[0] & 1), "APP_STOP посреди меню — экран погашен");

    // 7. (0.2) Образ с карты Evo: экран принимает его сам, рисует ход и оставляет слою ядра
    startApp();
    command(0x47, listBody());
    std::vector<uint8_t> rom(3000);
    for (size_t i = 0; i < rom.size(); i++)
        rom[i] = (uint8_t)(i * 37 + (i >> 9));
    std::vector<uint8_t> begin{(uint8_t)rom.size(), (uint8_t)(rom.size() >> 8), 0, 0, 'M', 'D', 0};
    check(command(0x4A, begin) == 1, "BEGIN: память есть");
    check(replyData.size() == 2 && replyData[1] == 1 && mediaFirst == 0x4D && mediaLast == 0x4D,
          "0.3: BEGIN — [1][1]: порции DMA разрешены (link_media 4D)");
    command(0x48, state(8, 1, 0, "Aladdin (Europe).gen"));
    run(600);
    check(alpha() == 15, "приём — экраном");
    uint32_t at = 0;
    for (int k = 0; k < 3; k++) {                    // 1536 из 3000 — около половины
        check(command(0x4B, packet(at, rom.data() + at, 512)) == 1, "DATA принят");
        at += 512;
        check(replyData.size() == 5 && replyData[1] == (uint8_t)at && replyData[2] == (uint8_t)(at >> 8),
              "ответ DATA — сколько принято");
    }
    command(0x48, state(8, 1, 0, "Aladdin (Europe).gen"));
    run(200);
    savePpm("07_receiving.ppm");
    check(command(0x4B, packet(0, rom.data(), 512)) == 1, "повтор принятого пакета — снова 1");
    std::vector<uint8_t> bad = packet(at, rom.data() + at, 512);
    bad[10] ^= 1;
    check(command(0x4B, bad) == 0, "испорченный пакет (CRC16) — 0");
    check(command(0x4C, {}) == 0, "END до конца файла — 0");
    while (at < rom.size()) {
        const size_t n = rom.size() - at < 512 ? rom.size() - at : 512;
        check(command(0x4B, packet(at, rom.data() + at, n)) == 1, "DATA принят");
        at += (uint32_t)n;
    }
    check(command(0x4C, {}) == 1, "END: файл оставлен слою");
    check(handoffTo == "MD" && handoffSize == rom.size() && handoffBlock != nullptr &&
              !memcmp(handoffBlock, rom.data(), rom.size()),
          "handoff_put: кому MD, весь образ байт в байт");
    command(0x48, state(8, 1, 0, "Aladdin (Europe).gen"));
    run(200);
    savePpm("08_received.ppm");
    app->stop();
    free(handoffBlock);
    handoffBlock = nullptr;

    // 7a. (0.3) Порции DMA до 8 КБ: смещение и XOR — свои байты запроса, повтор, испорченная
    startApp();
    command(0x47, listBody());
    std::vector<uint8_t> big(20000);
    for (size_t i = 0; i < big.size(); i++)
        big[i] = (uint8_t)(i * 13 + (i >> 8));
    std::vector<uint8_t> begin2{(uint8_t)big.size(), (uint8_t)(big.size() >> 8), 0, 0, 'A', 'M', 'I', 'G', 'A', 0};
    check(command(0x4A, begin2) == 1, "BEGIN 20000 для AMIGA");
    command(0x48, state(8, 2, 0, "Workbench.adf"));
    run(600);
    uint32_t got = 0;
    check(command(0x4D, rawPortion(0, big.data(), 8192)) == 1, "RAW 8 КБ принят");
    got = 8192;
    check(replyData.size() == 5 && replyData[1] == 0x00 && replyData[2] == 0x20, "ответ RAW — сколько принято");
    check(command(0x4D, rawPortion(0, big.data(), 8192)) == 1, "повтор порции — снова 1");
    std::vector<uint8_t> badRaw = rawPortion(got, big.data() + got, 8192);
    badRaw[100] ^= 0x10;
    check(command(0x4D, badRaw) == 0, "испорченная порция (XOR) — 0");
    check(command(0x4D, rawPortion(got + 8192, big.data() + got + 8192, 100)) == 0, "порция не по порядку — 0");
    check(command(0x4D, rawPortion(got, big.data() + got, 8192)) == 1, "та же порция снова — принята");
    got += 8192;
    command(0x48, state(8, 2, 0, "Workbench.adf"));
    run(200);
    savePpm("09_receiving_raw.ppm");
    check(command(0x4D, rawPortion(got, big.data() + got, big.size() - got)) == 1, "хвост порцией");
    check(command(0x4C, {}) == 1, "END после порций");
    check(handoffTo == "AMIGA" && handoffSize == big.size() && !memcmp(handoffBlock, big.data(), big.size()),
          "handoff_put: AMIGA, образ из порций байт в байт");
    app->stop();
    free(handoffBlock);
    handoffBlock = nullptr;

    // 8. Прошивка без ABI 1.14 — BEGIN отказывает: плагин шлёт образ слою, как прежде
    core.handoff_put = nullptr;
    startApp();
    check(command(0x4A, begin) == 0, "без handoff_put — BEGIN 0");
    app->stop();
    core.handoff_put = hHandoffPut;

    printf(failures ? "FAILED: %d\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
