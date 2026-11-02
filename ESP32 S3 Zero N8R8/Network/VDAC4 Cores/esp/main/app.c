/* COREMENU.ELF — экран плагина VDAC4 Cores на HDMI Tang.
 *
 * Плагин — диспетчер, как и прежде: список ядер (CORE_LIST), выбор, запуск слоя ядра и самого ядра.
 * Это приложение только рисует то, что прислал плагин, а карточку каждого ядра (значок, картинку,
 * тексты, клавишу Manage) берёт из ELF самого ядра на microSD Tang (раздел .vdac4.card, формат —
 * cores_ui/CARD.md). Отрисовка — cores_ui (дизайн Codex, утверждён пользователем 2026-11-02).
 *
 * Вывод — оверлей 1 ядра TS-Config (fpga/vdac2/rtl/fb_ovl.sv): холст 640×360 RGB565 точка в точку, по
 * центру кадра 720p, как экран плеера (на весь экран ×2 пользователь отверг ещё у плеера), непрозрачный,
 * в регионе оверлеев с того же слова, что у экрана плеера (до конца региона).
 * Появление и уход — общей прозрачностью, ступень за кадр вывода (0,3 с). Передаются только
 * изменившиеся прямоугольники, которые вернула отрисовка.
 *
 * Перед запуском ядра плагин просит оставить картинку (LEAVE 1) и шлёт CORE_APP ядра: ядро ESP
 * выгружает это приложение, а последний кадр («Loading…») остаётся на HDMI, пока Tang не сменит
 * ядро. В остальных случаях выгрузка гасит экран сразу, чтобы над WC не осталось чужой картинки.
 *
 * Команды Z80 (ответ — код | 0x80). Коды — свободные у прошивки (за VFS #40…#46): у слоёв ядер
 * свои #6E…#7E, и с ними не путаются ни в плагине, ни в журнале.
 *   47 LIST  [число][записи: имя 16, длина LE32, флаги: 1 — есть ИМЯ.ELF] — тело ответа CORE_LIST
 *            → C7 [1 — принято / 0]
 *   48 STATE [состояние cui_state_t | 0x80 — картинка уже на экране][выбранный][проценты int8,
 *            −1 — неизвестно][ошибка cui_error_t][текст ошибки ESP, до 60 знаков, 0] → C8 [флаги]
 *   49 LEAVE [0 — уйти плавно / 1 — оставить картинку: дальше ядро / 2 — работа кончена] → C9 [флаги];
 *            после 2 ядро ESP выгружает приложение (app_done)
 * Флаги: 1 — Tang с оверлеями есть и холст выделен; 2 — экран виден целиком; 4 — последнее STATE
 *        на экране; 8 — экрана нет (ушёл или не появлялся); 16 — карточки ядер прочитаны.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zm_app.h"
#include "cores_ui.h"
#include "cui_card.h"

#define APP_VERSION "0.1"

enum {
    CMD_LIST = 0x47,
    CMD_STATE = 0x48,
    CMD_LEAVE = 0x49,
};

enum {
    F_TANG = 1,
    F_SHOWN = 2,
    F_DRAWN = 4,
    F_GONE = 8,
    F_CARDS = 16,
};

/* Оверлей 1 (ts_fb_core.sv): CTRL, POS, SIZE, BASE с 0xC0 + 16·k; регион оверлеев — окно LINK
   0x80000…0xFFFFF, слово региона n — адрес 0x80000 + 4·n. Строка холста — ⌈w/16⌉·8 слов. */
#define OVL_CTRL (0xC0u + 16u * 1u)
#define OVL_MEM 0x80000u
#define STRIDE ((((uint32_t)CUI_WIDTH + 15u) / 16u) * 8u)
#define REGION_WORDS 131072u
#define BASE (REGION_WORDS - STRIDE * (uint32_t)CUI_HEIGHT)   /* 15 872, как у экрана плеера */
#define POS_X ((1280u - CUI_WIDTH) / 2)                       /* по центру 720p, ×1 */
#define POS_Y ((720u - CUI_HEIGHT) / 2)

#define TICK_MS 20u          /* кадр вывода 720p50: ступень прозрачности */
#define FRAME_TICKS 2u       /* кадр экрана — 25 раз в секунду */
#define SILENCE_MS 3000u     /* плагин молчит (сброс Evo) — экран уходит */
#define MAX_DIRTY 32
#define CARD_SLOTS 16        /* карточек — столько, сколько ядер в списке плагина (MAX_CORES) */
#define CARD_ARENA ((size_t)CARD_SLOTS * CUI_CARD_ARENA_BYTES)

typedef enum { SCR_OFF, SCR_FADE_IN, SCR_SHOWN, SCR_FADE_OUT, SCR_KEPT } screen_t;

static const zm_core_t *core;
static void *lock;
static volatile int running, task_done;

/* Общее с командами (ядро обмена) — под lock */
static cui_model_t shared;
static uint32_t state_seq;           /* растёт с каждой STATE */
static int have_list, have_state, instant;
static int leave_mode = -1;          /* −1 — нет, 0 — уйти плавно, 1 — оставить картинку */
static uint32_t last_cmd_ms;
static int cards_pending;
static volatile uint8_t flags;

/* Только задача экрана */
static cui_model_t view;
static uint16_t *canvas_px;
static cui_canvas_t canvas;
static uint8_t *card_arena;
static screen_t screen = SCR_OFF;
static uint8_t alpha;
static uint32_t tick_no;
static uint32_t drawn_seq;
static int tang_seen;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int tang_ready(void)
{
    return core->ui_available != NULL && core->ui_available() != 0;
}

static int configure(int on, uint8_t a)
{
    if (!on) {
        const uint32_t off = 0;
        return core->link_write(OVL_CTRL, &off, 1) == ZM_OK;
    }
    /* Сначала место, размер и начало, затем включение: Tang берёт всё в начале кадра */
    const uint32_t regs[3] = {POS_X | POS_Y << 16, (uint32_t)CUI_WIDTH | (uint32_t)CUI_HEIGHT << 16, BASE & 0x1FFF8u};
    const uint32_t ctrl = 1u | (uint32_t)(a & 15) << 4;   /* бит 1 (×2) — нет */
    return core->link_write(OVL_CTRL + 4, regs, 3) == ZM_OK && core->link_write(OVL_CTRL, &ctrl, 1) == ZM_OK;
}

/* Прямоугольники холста — в регион оверлеев: по две точки в слове ([15:0] — левая), до 16 слов за
   запись LINK. */
static int upload(const cui_rect_t *rects, int count)
{
    uint32_t words[16];
    for (int i = 0; i < count; i++) {
        const cui_rect_t *r = &rects[i];
        for (uint32_t y = r->y0; y < r->y1; y++) {
            const uint16_t *row = canvas_px + y * CUI_WIDTH;
            for (uint32_t x = r->x0 & ~1u; x < r->x1;) {
                uint32_t n = (r->x1 - x + 1) / 2;
                if (n > 16)
                    n = 16;
                for (uint32_t k = 0; k < n; k++)
                    words[k] = row[x + 2 * k] | (uint32_t)row[x + 2 * k + 1] << 16;
                const uint32_t word = BASE + y * STRIDE + x / 2;
                if (core->link_write(OVL_MEM + 4u * word, words, n) != ZM_OK)
                    return 0;
                x += 2 * n;
            }
        }
    }
    return 1;
}

static void *ui_alloc(size_t size, int psram, void *ctx)
{
    (void)ctx;
    return core->alloc(size, psram ? ZM_MEM_PSRAM : ZM_MEM_INTERNAL);
}

/* ELF ядра в памяти: разбор карточки читает его кусками. Файл берётся целиком (sd_read): sd_open
   открывает только файлы-диски с длиной, кратной 512 (tang_sd::diskOpen), а ELF не такие. */
typedef struct {
    const uint8_t *data;
    uint32_t size;
} mem_reader_t;

static int mem_read_at(void *ctx, uint32_t offset, void *dst, uint32_t count)
{
    const mem_reader_t *r = (const mem_reader_t *)ctx;
    if (offset > r->size || count > r->size - offset)
        return -1;
    memcpy(dst, r->data + offset, count);
    return 0;
}

#define CORE_ELF_MAX (2u * 1024u * 1024u)   /* больше — не слой ядра: карточку не искать */

/* Карточки ядер со своим ELF — из их ELF; нет файла, раздела или он битый — общая карточка. */
static void load_cards(void)
{
    cui_model_t list;
    core->mutex_lock(lock);
    list = shared;
    cards_pending = 0;
    core->mutex_unlock(lock);

    const cui_card_t *cards[CUI_MAX_CORES] = {0};
    for (unsigned i = 0; i < list.count && i < CARD_SLOTS && running; i++) {
        if (!list.cores[i].own_esp_layer || card_arena == NULL)
            continue;
        char path[40];
        size_t n = strnlen(list.cores[i].name, sizeof list.cores[i].name);
        memcpy(path, "/CORES/", 7);
        memcpy(path + 7, list.cores[i].name, n);
        memcpy(path + 7 + n, ".ELF", 5);
        uint8_t *data = NULL;
        size_t size = 0;
        int rc = ZM_BUSY;
        for (int tries = 0; tries < 50 && rc == ZM_BUSY && running; tries++) {
            rc = core->sd_read(path, &data, &size, CORE_ELF_MAX, &running);
            if (rc == ZM_BUSY)
                core->delay(20);
        }
        if (rc != ZM_OK || data == NULL) {
            core->log("coremenu: %s: not read (%d)", path, rc);
            continue;
        }
        mem_reader_t reader = {data, (uint32_t)size};
        cards[i] = cui_card_read_elf(mem_read_at, &reader, (uint32_t)size,
                                     card_arena + (size_t)i * CUI_CARD_ARENA_BYTES, CUI_CARD_ARENA_BYTES);
        core->free(data);            /* карточка уже скопирована в arena */
        core->log("coremenu: %s: %u bytes, card %s", path, (unsigned)size, cards[i] != NULL ? "found" : "none");
    }

    core->mutex_lock(lock);
    /* Список за это время сменился — прочитать заново */
    if (!cards_pending && shared.count == list.count) {
        for (unsigned i = 0; i < shared.count; i++)
            shared.cores[i].card = cards[i];
        shared.revision++;
        flags |= F_CARDS;
    }
    core->mutex_unlock(lock);
}

static void set_alpha(uint8_t a)
{
    alpha = a;
    configure(1, a);
}

static void screen_off(void)
{
    configure(0, 0);
    screen = SCR_OFF;
    alpha = 0;
}

/* Кадр: отрисовать изменившееся и передать. full — весь холст. */
static int frame(uint32_t now, int full, uint32_t seq)
{
    cui_rect_t dirty[MAX_DIRTY];
    const int n = cui_render(&view, now, full, &canvas, dirty, MAX_DIRTY);
    if (n > 0 && !upload(dirty, n))
        return 0;
    drawn_seq = seq;
    return 1;
}

static void step(uint32_t now)
{
    tick_no++;
    const int tang = tang_ready() && canvas_px != NULL;
    if (!tang) {
        if (screen != SCR_OFF)
            core->log("coremenu: Tang with overlays gone");
        screen = SCR_OFF;            /* перезагрузка ПЛИС: холсты и оверлеи пропали */
        alpha = 0;
        tang_seen = 0;
        flags = (uint8_t)((flags & F_CARDS) | F_GONE);
        return;
    }
    if (!tang_seen) {
        tang_seen = 1;
        core->log("coremenu: Tang with overlays here");
    }

    core->mutex_lock(lock);
    view = shared;
    const uint32_t seq = state_seq;
    const int ready = have_list && have_state;
    const int fast = instant;
    const int mode = leave_mode;
    const int silent = (uint32_t)(now - last_cmd_ms) > SILENCE_MS;
    const int pending = cards_pending;
    core->mutex_unlock(lock);

    if (pending && screen == SCR_OFF && mode < 0) {
        load_cards();                /* до появления: первый кадр — уже с карточками */
        return;
    }

    switch (screen) {
    case SCR_OFF:
        if (!ready || mode >= 0 || silent)
            break;
        /* Холст целиком — в Tang, затем оверлей: на экране сразу вся картинка, а не её начало */
        if (!frame(now, 1, seq)) {
            core->log("coremenu: canvas upload failed");
            break;
        }
        set_alpha(fast ? 15 : 0);
        screen = fast ? SCR_SHOWN : SCR_FADE_IN;
        core->log("coremenu: screen on");
        break;
    case SCR_FADE_IN:
    case SCR_SHOWN:
        if (mode == 0 || silent) {
            screen = SCR_FADE_OUT;
            break;
        }
        if (screen == SCR_FADE_IN) {
            set_alpha((uint8_t)(alpha + 1));
            if (alpha >= 15)
                screen = SCR_SHOWN;
        }
        if (pending)
            load_cards();
        if (tick_no % FRAME_TICKS == 0 || seq != drawn_seq)
            frame(now, 0, seq);
        if (mode == 1 && screen == SCR_SHOWN && drawn_seq == seq)
            screen = SCR_KEPT;       /* последний кадр на экране: дальше — ядро */
        break;
    case SCR_FADE_OUT:
        if (alpha > 0)
            set_alpha((uint8_t)(alpha - 1));
        if (alpha == 0) {
            screen_off();
            core->log("coremenu: screen off");
        }
        break;
    case SCR_KEPT:
        if (mode < 0)                /* новое STATE после LEAVE 1: запуск не вышел, экран снова живой */
            screen = SCR_SHOWN;
        break;
    }

    uint8_t f = (uint8_t)(F_TANG | (flags & F_CARDS));
    if (screen == SCR_SHOWN || screen == SCR_KEPT)
        f |= F_SHOWN;
    if ((screen == SCR_SHOWN || screen == SCR_KEPT) && drawn_seq == seq)
        f |= F_DRAWN;
    if (screen == SCR_OFF)
        f |= F_GONE;
    flags = f;
}

static void screen_task(void *arg)
{
    (void)arg;
    uint32_t next = core->millis();
    while (running) {
        const uint32_t now = core->millis();
        if ((int32_t)(now - next) >= 0) {
            step(now);
            next += TICK_MS;
            if ((int32_t)(core->millis() - next) > (int32_t)(4 * TICK_MS))
                next = core->millis();   /* отстали (долгое чтение карты) — без догоняющих кадров */
        }
        const uint32_t left = next - core->millis();
        core->delay(left > 0 && left <= TICK_MS ? left : 1);
    }
    task_done = 1;
}

/* --- команды Z80 --- */

static void reply_flags(uint8_t code)
{
    const uint8_t f = flags;
    core->reply(code, &f, 1);
}

static void cmd_list(const uint8_t *data, uint16_t length)
{
    uint8_t ok = 0;
    if (length >= 1 && data[0] <= CUI_MAX_CORES && length >= 1u + 21u * data[0]) {
        core->mutex_lock(lock);
        shared.count = data[0];
        for (unsigned i = 0; i < shared.count; i++) {
            const uint8_t *e = data + 1 + 21 * i;
            cui_core_t *c = &shared.cores[i];
            memset(c, 0, sizeof *c);
            for (unsigned k = 0; k < 15 && e[k] >= 0x20 && e[k] < 0x7F; k++)
                c->name[k] = (char)e[k];
            c->size_bytes = le32(e + 16);
            c->own_esp_layer = (e[20] & 1) != 0;
        }
        if (shared.selected >= shared.count)
            shared.selected = 0;
        shared.revision++;
        have_list = 1;
        cards_pending = 1;
        flags &= (uint8_t)~F_CARDS;
        last_cmd_ms = core->millis();
        core->mutex_unlock(lock);
        ok = 1;
    }
    core->reply(0x80 | CMD_LIST, &ok, 1);
}

static void cmd_state(const uint8_t *data, uint16_t length)
{
    if (length >= 4) {
        core->mutex_lock(lock);
        const uint8_t st = data[0] & 0x7F;
        shared.state = st <= CUI_ERROR ? (cui_state_t)st : CUI_ERROR;
        shared.selected = shared.count != 0 && data[1] < shared.count ? data[1] : 0;
        const int8_t pct = (int8_t)data[2];
        shared.progress_percent = pct >= 0 && pct <= 100 ? pct : -1;
        shared.error = data[3] <= CUI_ERR_NO_ANSWER ? (cui_error_t)data[3] : CUI_ERR_CORE;
        char text[sizeof shared.error_text] = {0};
        unsigned n = 0;
        for (unsigned k = 0; k + 4 < length && k < sizeof text - 1 && data[4 + k] != 0; k++) {
            text[k] = data[4 + k] >= 0x20 && data[4 + k] < 0x7F ? (char)data[4 + k] : '?';
            if (text[k] != ' ')
                n = k + 1;
        }
        text[n] = 0;                 /* поле окна плагина дополнено пробелами — хвост прочь */
        if (memcmp(text, shared.error_text, sizeof text) != 0) {
            memcpy(shared.error_text, text, sizeof text);
            shared.revision++;
        }
        instant = (data[0] & 0x80) != 0;
        have_state = 1;
        leave_mode = -1;
        state_seq++;
        last_cmd_ms = core->millis();
        core->mutex_unlock(lock);
    }
    reply_flags(0x80 | CMD_STATE);
}

static int command(uint8_t cmd, const uint8_t *data, uint16_t length)
{
    switch (cmd) {
    case CMD_LIST:
        cmd_list(data, length);
        return ZM_OK;
    case CMD_STATE:
        cmd_state(data, length);
        return ZM_OK;
    case CMD_LEAVE:
        if (length >= 1 && data[0] == 2) {
            core->app_done();        /* ответ уйдёт, затем ядро ESP выгрузит приложение */
            reply_flags(0x80 | CMD_LEAVE);
            return ZM_OK;
        }
        core->mutex_lock(lock);
        leave_mode = length >= 1 && data[0] == 1 ? 1 : 0;
        last_cmd_ms = core->millis();
        core->mutex_unlock(lock);
        reply_flags(0x80 | CMD_LEAVE);
        return ZM_OK;
    default:
        return ZM_ERROR;
    }
}

static int start(const zm_core_t *c)
{
    core = c;
    /* Нужны оверлей (ABI 1.5), выгрузка по команде (1.6) и чтение файла карты Tang (1.7) */
    if (c->size < offsetof(zm_core_t, sd_read) + sizeof c->sd_read || !c->link_write || !c->ui_available ||
        !c->app_done || !c->sd_read)
        return ZM_ERROR;
    lock = c->mutex_create();
    if (!lock)
        return ZM_NO_MEMORY;
    memset(&shared, 0, sizeof shared);
    shared.progress_percent = -1;
    state_seq = 0;
    have_list = have_state = instant = 0;
    leave_mode = -1;
    cards_pending = 0;
    screen = SCR_OFF;
    alpha = 0;
    tick_no = 0;
    drawn_seq = 0;
    tang_seen = 0;
    flags = F_GONE;
    last_cmd_ms = c->millis();

    canvas_px = (uint16_t *)c->alloc((size_t)CUI_WIDTH * CUI_HEIGHT * 2, ZM_MEM_PSRAM);
    card_arena = (uint8_t *)c->alloc(CARD_ARENA, ZM_MEM_PSRAM);
    canvas.px = canvas_px;
    canvas.w = CUI_WIDTH;
    canvas.h = CUI_HEIGHT;
    canvas.stride = CUI_WIDTH;
    if (canvas_px == NULL || card_arena == NULL || !cui_init(&canvas, ui_alloc, NULL)) {   /* 1 — да */
        /* Без памяти экрана нет: флаг Tang не встанет, плагин покажет своё окно */
        c->log("coremenu: no memory for the screen");
        canvas_px = NULL;
    }

    running = 1;
    task_done = 0;
#ifdef COREMENU_HOST_TEST
    /* Стенд ПК (tests/coremenu_host.c) сам зовёт шаги задачи экрана: coremenu_host_step */
    task_done = 1;
    const int r = ZM_OK;
    (void)screen_task;
#else
    /* Ядро 0, приоритет 1: на ядре 1 ответы Evo, выше них приложениям нельзя */
    const int r = c->task_start(screen_task, NULL, 8192, 1, 0);
#endif
    if (r != ZM_OK) {
        running = 0;
        task_done = 1;
        return r;
    }
    c->log("coremenu: COREMENU.ELF " APP_VERSION " ready");
    return ZM_OK;
}

static void stop(void)
{
    running = 0;
    for (unsigned i = 0; i < 300 && !task_done; i++)
        core->delay(10);
    /* Картинку оставляет только LEAVE 1 (дальше грузится ядро); иначе — погасить сразу */
    if (task_done && screen != SCR_OFF && screen != SCR_KEPT && tang_ready())
        configure(0, 0);
    cui_reset();
}

#ifdef COREMENU_HOST_TEST
void coremenu_host_step(uint32_t now)
{
    step(now);
}

const uint16_t *coremenu_host_canvas(void)
{
    return canvas_px;
}
#endif

int main(int argc, char **argv)
{
    static const zm_app_t app = {sizeof(zm_app_t), ZM_APP_ABI_MAJOR, ZM_APP_ABI_MINOR, "coremenu", APP_VERSION,
                                 CMD_LIST, CMD_LEAVE, start, stop, command, NULL, NULL, NULL};
    if (argc != 3)
        return 0;
    *(const zm_app_t **)argv[2] = &app;
    return ZM_APP_ABI_MAJOR;
}
