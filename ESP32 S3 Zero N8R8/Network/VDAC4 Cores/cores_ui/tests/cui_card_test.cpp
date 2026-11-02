#include "cores_ui.h"
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
unsigned checks = 0, fallback_frames = 0, sector_calls = 0, max_request = 0;
void check(bool ok, const char *why) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "FAIL card: %s (check %u)\n", why, checks);
        exit(1);
    }
}
Bytes file(const char *path) {
    FILE *f = fopen(path, "rb");
    check(f && fseek(f, 0, SEEK_END) == 0, path);
    long n = ftell(f);
    check(n >= 0 && fseek(f, 0, SEEK_SET) == 0, "file seek");
    Bytes data(static_cast<size_t>(n));
    check(fread(data.data(), 1, data.size(), f) == data.size(), "file read");
    fclose(f);
    return data;
}
uint16_t u16(const Bytes &v, size_t at) {
    return uint16_t(v[at] | unsigned(v[at + 1]) << 8);
}
uint32_t u32(const Bytes &v, size_t at) {
    return uint32_t(v[at]) | uint32_t(v[at + 1]) << 8 | uint32_t(v[at + 2]) << 16 |
           uint32_t(v[at + 3]) << 24;
}
void p16(Bytes &v, size_t at, uint16_t n) {
    v[at] = uint8_t(n);
    v[at + 1] = uint8_t(n >> 8);
}
void p32(Bytes &v, size_t at, uint32_t n) {
    p16(v, at, uint16_t(n));
    p16(v, at + 2, uint16_t(n >> 16));
}
void crc(Bytes &data) {
    p32(data, 16, 0);
    uint32_t c = ~0u;
    for (auto b : data) {
        c ^= b;
        for (int i = 0; i < 8; ++i)
            c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
    }
    p32(data, 16, ~c);
}
uint16_t pixels[640 * 360 + 2], cache[640 * 360];
void *alloc(size_t n, int psram, void *) {
    check(psram && n == sizeof(cache), "cache contract");
    return cache;
}
void fallback(const cui_card_t *card) {
    check(card == nullptr, "bad input did not return NULL");
    pixels[0] = pixels[640 * 360 + 1] = 0xabba;
    cui_canvas_t canvas{pixels + 1, 640, 360, 640};
    cui_model_t model{};
    model.state = CUI_MENU;
    model.count = 1;
    model.cores[0].own_esp_layer = 1;
    model.cores[0].card = card;
    memcpy(model.cores[0].name, "NES", 4);
    cui_rect_t dirty[8];
    cui_reset();
    check(cui_init(&canvas, alloc, nullptr) == 1, "fallback init");
    check(cui_render(&model, 0, 1, &canvas, dirty, 8) == 1, "fallback full render");
    for (int y = 277; y < 292; ++y)
        for (int x = 388; x < 596; ++x)
            check(canvas.px[y * 640 + x] == uint16_t((18 >> 3) << 11 | (30 >> 2) << 5 | 44 >> 3),
                  "fallback must hide Manage even for NES name");
    check(pixels[0] == 0xabba && pixels[640 * 360 + 1] == 0xabba, "fallback bounds");
    ++fallback_frames;
}
struct Arena {
    Bytes memory;
    explicit Arena(size_t n = CUI_CARD_ARENA_BYTES) : memory(n + 33, 0xa6) {}
    void *data() {
        return memory.data() + 17;
    } // deliberately unaligned
    size_t size() const {
        return memory.size() - 33;
    }
    void guards() {
        check(std::all_of(memory.begin(), memory.begin() + 17, [](uint8_t c) { return c == 0xa6; }),
              "arena prefix");
        check(std::all_of(memory.end() - 16, memory.end(), [](uint8_t c) { return c == 0xa6; }),
              "arena suffix");
    }
};
struct Reader {
    const Bytes &bytes;
    unsigned calls = 0, fail_at = ~0u;
    static int read(void *ctx, uint32_t offset, void *dst, uint32_t count) {
        auto &r = *static_cast<Reader *>(ctx);
        check(count && count <= 512 && offset / 512 == (offset + count - 1) / 512,
              "512-byte sector contract");
        check(offset <= r.bytes.size() && count <= r.bytes.size() - offset, "read beyond file");
        ++sector_calls;
        max_request = std::max(max_request, unsigned(count));
        if (r.calls++ == r.fail_at)
            return -1;
        uint8_t sector[512] = {};
        uint32_t base = offset & ~511u;
        size_t n = std::min(size_t(512), r.bytes.size() - base);
        memcpy(sector, r.bytes.data() + base, n);
        memcpy(dst, sector + offset % 512, count);
        return 0;
    }
};
const cui_card_t *parse(const Bytes &data, Arena &a) {
    auto *card = cui_card_parse(data.data(), uint32_t(data.size()), a.data(), a.size());
    a.guards();
    return card;
}
const cui_card_t *elf(const Bytes &data, Arena &a, unsigned fail_at = ~0u) {
    Reader reader{data, 0, fail_at};
    auto *card =
        cui_card_read_elf(Reader::read, &reader, uint32_t(data.size()), a.data(), a.size());
    a.guards();
    return card;
}
void bad_card(Bytes data, bool repair_crc = true) {
    if (repair_crc)
        crc(data);
    Arena arena;
    fallback(parse(data, arena));
}
void valid_and_arena() {
    const char *names[] = {"NES", "NEXT", "AMIGA"};
    const char *keys[] = {"F1", "F1", "F11"};
    for (int i = 0; i < 3; ++i) {
        char path[128];
        snprintf(path, sizeof(path), "../cards/%s/card.bin", names[i]);
        Bytes data = file(path);
        Arena arena(cui_card_arena_required(uint32_t(data.size())));
        const auto *card = parse(data, arena);
        check(card && strcmp(card->manage_key, keys[i]) == 0, "valid card key");
        check(card->icon.w == 56 && card->icon.h == 28 && card->image.w == 256 &&
                  card->image.h == 128,
              "valid dimensions");
        check(card->image.rle >= arena.memory.data() &&
                  card->image.rle + card->image.bytes <= arena.memory.data() + arena.memory.size(),
              "arena ownership");
        snprintf(path, sizeof(path), "elf/%s.ELF", names[i]);
        Bytes image = file(path);
        Arena from_elf;
        auto *streamed = elf(image, from_elf);
        check(streamed && strcmp(streamed->title, card->title) == 0 &&
                  streamed->accent == card->accent,
              "ELF card metadata");
        check(memcmp(streamed->palette, card->palette, sizeof(card->palette)) == 0 &&
                  streamed->image.bytes == card->image.bytes &&
                  memcmp(streamed->image.rle, card->image.rle, card->image.bytes) == 0 &&
                  streamed->icon.bytes == card->icon.bytes &&
                  memcmp(streamed->icon.rle, card->icon.rle, card->icon.bytes) == 0,
              "ELF card pixels");
        snprintf(path, sizeof(path), "elf/%s_replaced.ELF", names[i]);
        auto replacement = file(path);
        auto *replaced = elf(replacement, from_elf);
        check(replaced && replaced->manage_key[strlen(replaced->manage_key) - 1] == '9',
              "replacement section selected");
        snprintf(path, sizeof(path), "elf/%s_original.ELF", names[i]);
        fallback(elf(file(path), from_elf));
        // Parsed strings and streams survive destruction/modification of source bytes.
        std::fill(data.begin(), data.end(), 0);
        check(strcmp(card->manage_key, keys[i]) == 0 && card->image.rle[0] != 0, "input copied");
    }
    Bytes data = file("../cards/NES/card.bin");
    for (size_t size :
         {size_t(0), size_t(1), sizeof(cui_card_t), sizeof(cui_card_t) + data.size() - 1}) {
        Bytes exact(size);
        check(!cui_card_parse(data.data(), uint32_t(data.size()), exact.data(), size),
              "insufficient arena");
    }
    Arena a;
    check(!cui_card_parse(nullptr, 64, a.data(), a.size()), "NULL data");
    check(!cui_card_parse(data.data(), uint32_t(data.size()), nullptr, a.size()), "NULL arena");
    check(!cui_card_read_elf(nullptr, nullptr, 100, a.data(), a.size()), "NULL reader");
    check(cui_card_arena_required(63) == 0 && cui_card_arena_required(24577) == 0,
          "arena size bounds");
    check(cui_card_arena_required(24576) <= CUI_CARD_ARENA_BYTES, "maximum arena");
    printf("sizeof(cui_card_t)=%zu; max arena=%zu; NES arena=%zu\n", sizeof(cui_card_t),
           cui_card_arena_required(24576), cui_card_arena_required(uint32_t(data.size())));
    auto *limits = parse(file("limits.bin"), a);
    check(limits && strlen(limits->title) == 48 && strlen(limits->full_name) == 96 &&
              strlen(limits->year) == 8 && strlen(limits->manufacturer) == 48 &&
              strlen(limits->line1) == 32 && strlen(limits->line2) == 32 &&
              strlen(limits->category) == 16 && strlen(limits->manage_key) == 8,
          "maximal valid strings");
    cui_canvas_t canvas{pixels + 1, 640, 360, 640};
    cui_model_t model{};
    model.state = CUI_MENU;
    model.count = 32;
    for (auto &core : model.cores) {
        memset(core.name, 'W', 15);
        core.card = limits;
        core.own_esp_layer = 1;
    }
    cui_rect_t rects[8];
    check(cui_init(&canvas, alloc, nullptr) == 1, "max strings init");
    for (int i = 0; i < 32; ++i) {
        model.selected = uint8_t(i);
        check(cui_render(&model, uint32_t(i) * 40, 1, &canvas, rects, 8) == 1,
              "max strings render");
    }
    check(pixels[0] == 0xabba && pixels[640 * 360 + 1] == 0xabba, "max strings bounds");
}
void malformed_cards() {
    Bytes good = file("../cards/NES/card.bin");
    Arena arena;
    for (size_t size = 0; size < good.size(); ++size) {
        Bytes truncated(good.begin(), good.begin() + size);
        check(!parse(truncated, arena), "every raw truncation");
    }
    for (size_t field :
         {size_t(0), size_t(8), size_t(10), size_t(12), size_t(16), size_t(56), size_t(60)}) {
        auto bad = good;
        bad[field] ^= 0x55;
        bad_card(bad, field != 16);
    }
    auto bad = good;
    bad.back() ^= 1;
    bad_card(bad, false);
    for (size_t field : {size_t(22), size_t(24), size_t(26), size_t(28), size_t(30)})
        for (uint16_t value : {uint16_t(0), uint16_t(65535)}) {
            bad = good;
            p16(bad, field, value);
            bad_card(bad);
        }
    for (size_t field : {size_t(32), size_t(36)})
        for (uint32_t value : {0u, 1u, 0xffffffffu}) {
            bad = good;
            p32(bad, field, value);
            bad_card(bad);
        }
    for (int i = 0; i < 8; ++i) {
        bad = good;
        p16(bad, 40 + i * 2, 65535);
        bad_card(bad);
    }
    for (uint8_t c : {uint8_t(0), uint8_t(31), uint8_t(127), uint8_t(255)}) {
        bad = good;
        bad[64] = c;
        bad_card(bad);
    }
    size_t string_bytes = 0;
    for (int i = 0; i < 8; ++i)
        string_bytes += u16(good, 40 + i * 2);
    size_t icon = 64 + string_bytes + u16(good, 22) * 2;
    for (size_t start : {icon, icon + u32(good, 32)}) {
        bad = good;
        bad[start] = 0;
        bad_card(bad);
        bad = good;
        bad[start + 1] = uint8_t(u16(good, 22));
        bad_card(bad);
        bad = good;
        bad[start] -= 1; // total run coverage is one pixel short
        bad_card(bad);
        bad = good;
        bad[start] = bad[start] == 255 ? 254 : uint8_t(bad[start] + 1);
        bad_card(bad);
    }
    bad = good;
    bad.push_back(0);
    p32(bad, 12, uint32_t(bad.size()));
    bad_card(bad);
    // Valid empty and full-width keys, no hidden fallback based on name.
    const size_t key = 64 + string_bytes - u16(good, 54);
    for (const char *text : {"", "Ctrl-F11"}) {
        bad = good;
        bad.erase(bad.begin() + key, bad.begin() + key + u16(good, 54));
        bad.insert(bad.begin() + key, text, text + strlen(text));
        p16(bad, 54, uint16_t(strlen(text)));
        p32(bad, 12, uint32_t(bad.size()));
        crc(bad);
        auto *card = parse(bad, arena);
        check(card && strcmp(card->manage_key, text) == 0, "valid empty/eight-character key");
    }
}
void malformed_elf() {
    Bytes good = file("elf/NES.ELF");
    Arena arena;
    size_t table = u32(good, 32), count = u16(good, 48), names = table + u16(good, 50) * 40;
    size_t card = table + (count - 1) * 40;
    check(u32(good, card + 20) == file("../cards/NES/card.bin").size(),
          "fixture card is last section");
    // Table is at EOF: every prefix is incomplete, including cuts inside card/name data.
    for (size_t size = 0; size < good.size(); ++size) {
        Bytes truncated(good.begin(), good.begin() + size);
        check(!elf(truncated, arena), "every ELF truncation");
    }
    for (size_t field : {size_t(0), size_t(4), size_t(5), size_t(6), size_t(16), size_t(18),
                         size_t(20), size_t(40), size_t(46), size_t(48), size_t(50)}) {
        auto bad = good;
        bad[field] = 0;
        fallback(elf(bad, arena));
    }
    for (size_t field : {size_t(32), names + 16, names + 20, card, card + 16, card + 20}) {
        auto bad = good;
        p32(bad, field, 0xffffffffu);
        fallback(elf(bad, arena));
    }
    for (size_t field : {size_t(42), size_t(44), size_t(48), size_t(50)}) {
        auto bad = good;
        p16(bad, field, 65535);
        fallback(elf(bad, arena));
    }
    for (uint32_t offset : {0u, 0xfffffff0u, u32(good, 28)}) {
        auto bad = good;
        p32(bad, 32, offset);
        fallback(elf(bad, arena));
    }
    for (size_t offset : {size_t(0), table, size_t(u32(good, names + 16)), size_t(u32(good, 28))}) {
        auto bad = good;
        p32(bad, card + 16, uint32_t(offset));
        fallback(elf(bad, arena));
    }
    for (uint32_t flags : {1u, 2u, 4u, 0xffffffffu}) {
        auto bad = good;
        p32(bad, card + 8, flags);
        fallback(elf(bad, arena));
    }
    auto bad = good;
    p32(bad, card + 4, 8);
    fallback(elf(bad, arena));
    bad = good;
    p32(bad, card, 0); // no .vdac4.card name
    fallback(elf(bad, arena));
    bad = good;
    p32(bad, table + 40, u32(good, card)); // duplicate card name
    fallback(elf(bad, arena));
    bad = good;
    bad[u32(good, names + 16) + u32(good, names + 20) - 1] = 'X';
    fallback(elf(bad, arena));
    bad = good;
    bad[u32(good, card + 16) + 100] ^= 1; // ELF valid; card CRC invalid
    fallback(elf(bad, arena));
    Reader reader{good};
    check(cui_card_read_elf(Reader::read, &reader, uint32_t(good.size()), arena.data(),
                            arena.size()) != nullptr,
          "reader baseline");
    unsigned calls = reader.calls;
    for (unsigned i = 0; i < calls; ++i)
        check(!elf(good, arena, i), "I/O error on each read");
    printf("ELF callback failure points=%u\n", calls);
}
void seeded_mutations() {
    Bytes card = file("../cards/NES/card.bin"), image = file("elf/NES.ELF");
    Arena arena;
    uint32_t seed = 0x43a212u;
    auto random = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    unsigned accepted = 0;
    for (int i = 0; i < 6000; ++i) {
        auto data = (i & 1) ? card : image;
        for (unsigned j = 0, n = random() % 8 + 1; j < n; ++j)
            data[random() % data.size()] ^= uint8_t(random());
        if ((i % 4) == 1)
            crc(data); // reach semantic card checks with a valid CRC
        auto *result = (i & 1) ? parse(data, arena) : elf(data, arena);
        if (result) {
            ++accepted; // mutation may affect unused ELF bytes or valid text/pixels
            check(result->image.w <= 256 && result->image.h <= 128 && result->palette_count <= 256,
                  "accepted mutation bounds");
        }
    }
    printf("Seeded mutations=6000; accepted structurally valid=%u\n", accepted);
}
}
int main() {
    valid_and_arena();
    malformed_cards();
    malformed_elf();
    seeded_mutations();
    printf("PASS card: %u checks; %u fallback frames; %u sector reads; max request %u bytes\n",
           checks, fallback_frames, sector_calls, max_request);
    return 0;
}
