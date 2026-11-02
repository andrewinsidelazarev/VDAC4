#include "cui_card.h"
#include <new>
#include <string.h>

namespace {
constexpr uint32_t HEADER = 64, SECTOR = 512;
uint16_t le16(const uint8_t *p) {
    return uint16_t(p[0] | (uint16_t(p[1]) << 8));
}
uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool within(uint32_t at, uint32_t n, uint32_t size) {
    return at <= size && n <= size - at;
}
bool overlaps(uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
    return n && m && (a <= b ? b - a < n : a - b < m);
}
uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= (i >= 16 && i < 20) ? 0 : p[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
uint8_t *storage(void *arena, size_t size, uint32_t bytes) {
    if (!arena || !cui_card_arena_required(bytes))
        return nullptr;
    size_t pad = (alignof(cui_card_t) - reinterpret_cast<uintptr_t>(arena) % alignof(cui_card_t)) %
                 alignof(cui_card_t);
    size_t need = sizeof(cui_card_t) + size_t(bytes);
    if (pad > size || need > size - pad)
        return nullptr;
    return static_cast<uint8_t *>(arena) + pad;
}
bool header_ok(const uint8_t *p, uint32_t n) {
    if (n < HEADER || n > CUI_CARD_MAX_BYTES)
        return false;
    return memcmp(p, "VD4CARD\0", 8) == 0 && le16(p + 8) == 1 && le16(p + 10) == HEADER &&
           le32(p + 12) == n && le32(p + 56) == 0 && le32(p + 60) == 0;
}
bool rle_ok(const uint8_t *p, uint32_t n, uint32_t pixels, uint16_t colors) {
    if (n == 0 || (n & 1) || n > pixels * 2)
        return false;
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; i += 2) {
        if (!p[i] || p[i + 1] >= colors || p[i] > pixels - used)
            return false;
        used += p[i];
    }
    return used == pixels;
}
struct Reader {
    cui_read_fn fn;
    void *ctx;
    uint32_t size;
    bool get(uint32_t at, void *dst, uint32_t n) const {
        if (!within(at, n, size))
            return false;
        auto *out = static_cast<uint8_t *>(dst);
        while (n) {
            uint32_t part = SECTOR - at % SECTOR;
            if (part > n)
                part = n;
            if (fn(ctx, at, out, part) != 0)
                return false;
            out += part;
            at += part;
            n -= part;
        }
        return true;
    }
};
struct Section {
    uint32_t name, type, flags, offset, size;
};
bool section(const Reader &r, uint32_t table, uint32_t i, Section &s) {
    uint8_t h[40];
    if (!r.get(table + i * 40, h, 40))
        return false;
    s = {le32(h), le32(h + 4), le32(h + 8), le32(h + 16), le32(h + 20)};
    return true;
}
// Имена ограничены 255 байтами: число чтений даже у повреждённого ELF ограничено.
bool name_match(const Reader &r, const Section &names, uint32_t at, bool &match) {
    if (at >= names.size)
        return false;
    constexpr char wanted[] = ".vdac4.card";
    match = true;
    uint32_t pos = 0;
    while (at < names.size && pos < 256) {
        uint8_t buf[64];
        uint32_t n = names.size - at;
        if (n > sizeof(buf))
            n = sizeof(buf);
        if (n > 256 - pos)
            n = 256 - pos;
        if (!r.get(names.offset + at, buf, n))
            return false;
        for (uint32_t i = 0; i < n; ++i, ++pos) {
            if (buf[i] == 0) {
                match = match && pos == sizeof(wanted) - 1;
                return true;
            }
            if (pos >= sizeof(wanted) - 1 || buf[i] != uint8_t(wanted[pos]))
                match = false;
        }
        at += n;
    }
    return false;
}
}

extern "C" size_t cui_card_arena_required(uint32_t bytes) {
    if (bytes < HEADER || bytes > CUI_CARD_MAX_BYTES)
        return 0;
    return sizeof(cui_card_t) + alignof(cui_card_t) - 1 + size_t(bytes);
}
static_assert(sizeof(cui_card_t) + alignof(cui_card_t) - 1 + CUI_CARD_MAX_BYTES <=
                  CUI_CARD_ARENA_BYTES,
              "arena constant is too small");

extern "C" const cui_card_t *cui_card_parse(const void *data, uint32_t n, void *arena,
                                            size_t arena_size) {
    if (!data || n < HEADER || n > CUI_CARD_MAX_BYTES)
        return nullptr;
    const auto *p = static_cast<const uint8_t *>(data);
    if (!header_ok(p, n) || crc32(p, n) != le32(p + 16))
        return nullptr;
    uint16_t colors = le16(p + 22), iw = le16(p + 24), ih = le16(p + 26), w = le16(p + 28),
             h = le16(p + 30);
    if (!colors || colors > 256 || !iw || iw > 56 || !ih || ih > 28 || !w || w > 256 || !h ||
        h > 128)
        return nullptr;
    constexpr uint16_t limits[] = {48, 96, 8, 48, 32, 32, 16, 8};
    uint32_t cursor = HEADER;
    uint16_t lengths[8];
    for (int i = 0; i < 8; ++i) {
        lengths[i] = le16(p + 40 + i * 2);
        if (lengths[i] > limits[i] || (i == 0 && !lengths[i]) || !within(cursor, lengths[i], n))
            return nullptr;
        for (uint32_t j = 0; j < lengths[i]; ++j)
            if (p[cursor + j] < 32 || p[cursor + j] > 126)
                return nullptr;
        cursor += lengths[i];
    }
    uint32_t pal_at = cursor;
    if (!within(cursor, uint32_t(colors) * 2, n))
        return nullptr;
    cursor += uint32_t(colors) * 2;
    uint32_t icon_at = cursor, icon_bytes = le32(p + 32), image_bytes = le32(p + 36);
    if (!within(cursor, icon_bytes, n))
        return nullptr;
    cursor += icon_bytes;
    uint32_t image_at = cursor;
    if (!within(cursor, image_bytes, n) || image_bytes != n - cursor)
        return nullptr;
    if (!rle_ok(p + icon_at, icon_bytes, uint32_t(iw) * ih, colors) ||
        !rle_ok(p + image_at, image_bytes, uint32_t(w) * h, colors))
        return nullptr;
    auto *base = storage(arena, arena_size, n);
    if (!base)
        return nullptr;
    auto *raw = base + sizeof(cui_card_t);
    // memmove допускает вход уже в этой arena (так работает чтение ELF).
    memmove(raw, p, n);
    auto *card = new (base) cui_card_t{};
    char *strings[] = {card->title, card->full_name, card->year,     card->manufacturer,
                       card->line1, card->line2,     card->category, card->manage_key};
    cursor = HEADER;
    for (int i = 0; i < 8; ++i) {
        memcpy(strings[i], raw + cursor, lengths[i]);
        cursor += lengths[i];
    }
    card->accent = le16(raw + 20);
    card->palette_count = colors;
    for (int i = 0; i < colors; ++i)
        card->palette[i] = le16(raw + pal_at + i * 2);
    card->icon = {raw + icon_at, icon_bytes, iw, ih};
    card->image = {raw + image_at, image_bytes, w, h};
    return card;
}

extern "C" const cui_card_t *cui_card_read_elf(cui_read_fn read, void *ctx, uint32_t file_size,
                                               void *arena, size_t arena_size) {
    if (!read || !arena || file_size < 52)
        return nullptr;
    Reader r{read, ctx, file_size};
    uint8_t h[52];
    if (!r.get(0, h, sizeof(h)) ||
        memcmp(h,
               "\x7f"
               "ELF",
               4) ||
        h[4] != 1 || h[5] != 1 || h[6] != 1 || (le16(h + 16) != 2 && le16(h + 16) != 3) ||
        le16(h + 18) != 94 || le32(h + 20) != 1 || le16(h + 40) != 52 || le16(h + 46) != 40)
        return nullptr;
    uint32_t table = le32(h + 32), count = le16(h + 48), index = le16(h + 50);
    if (!count || count > 1024 || !index || index >= count || table < 52 ||
        !within(table, count * 40, file_size))
        return nullptr;
    uint32_t ph = le32(h + 28), pn = le16(h + 44), ps = le16(h + 42);
    if (pn && (pn > 128 || ps != 32 || ph < 52 || !within(ph, pn * ps, file_size) ||
               overlaps(ph, pn * ps, table, count * 40)))
        return nullptr;
    Section names{};
    if (!section(r, table, index, names) || names.type != 3 || !names.size || names.size > 65536 ||
        !within(names.offset, names.size, file_size) ||
        overlaps(names.offset, names.size, table, count * 40))
        return nullptr;
    Section card{};
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        Section s{};
        bool match = false;
        if (!section(r, table, i, s) || (s.type != 8 && !within(s.offset, s.size, file_size)) ||
            !name_match(r, names, s.name, match))
            return nullptr;
        if (i == 0 && (s.name || s.type || s.flags || s.offset || s.size))
            return nullptr;
        if (s.type != 8 &&
            (overlaps(s.offset, s.size, 0, 52) || overlaps(s.offset, s.size, table, count * 40) ||
             overlaps(s.offset, s.size, ph, pn * ps)))
            return nullptr;
        if (match) {
            if (found || s.type != 1 || s.flags != 0 || s.size < HEADER ||
                s.size > CUI_CARD_MAX_BYTES)
                return nullptr;
            card = s;
            found = true;
        }
    }
    if (!found || overlaps(card.offset, card.size, 0, 52) ||
        overlaps(card.offset, card.size, table, count * 40) ||
        overlaps(card.offset, card.size, ph, pn * ps))
        return nullptr;
    // Карточка не должна ссылаться на байты другой секции, в том числе таблицы имён.
    for (uint32_t i = 0; i < count; ++i) {
        Section s{};
        if (!section(r, table, i, s))
            return nullptr;
        if (s.name != card.name && s.type != 8 &&
            overlaps(card.offset, card.size, s.offset, s.size))
            return nullptr;
    }
    auto *base = storage(arena, arena_size, card.size);
    if (!base)
        return nullptr;
    auto *raw = base + sizeof(cui_card_t);
    if (!r.get(card.offset, raw, card.size))
        return nullptr;
    return cui_card_parse(raw, card.size, arena, arena_size);
}
