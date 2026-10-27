/* Bounded, sequenced cartridge upload. CRC checks bytes accepted by the FPGA;
   it does not claim to verify SDRAM. One committed packet is never replayed. */
static uint32_t rom_crc(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (c & 1 ? 0xedb88320u : 0);
    }
    return ~c;
}

static int rom_status(uint32_t *s)
{
    uint32_t a, b;
    for (int i = 0; i < 4; i++)
        if (link_rd(0x44, &a) == ZM_OK && link_rd(0x44, &b) == ZM_OK && a == b) {
            *s = a;
            return 1;
        }
    return 0;
}

/* меню F1 (0.9): ход передачи — каждые 16 пакетов (4 КБ) и в конце; NULL — не нужен */
static void (*load_progress)(size_t done, size_t total);

static int load_cartridge(const uint8_t *p, size_t size)
{
    if (rom_check(p, size)) return 0;
    uint32_t s = 0;
    const uint32_t start_ms = core->millis();
    do {
        if (rom_status(&s) && !(s & 16)) break;
        core->delay(1);
    } while (running && core->millis() - start_ms < 2000);
    if ((s & 16) || !running || link_wr(0x44, 1) != ZM_OK) return 0;
    core->delay(1);
    if (!rom_status(&s) || !(s & 1)) return 0;
    uint32_t seq = 0;
    for (size_t offset = 0; offset < size && running && !exit_req;) {
        const uint32_t n = size - offset > 256 ? 256 : (uint32_t)(size - offset);
        uint32_t words[64] = {0};
        for (uint32_t i = 0; i < n; i++) words[i / 4] |= (uint32_t)p[offset + i] << (8 * (i & 3));
        for (uint32_t i = 0; i < (n + 3) / 4; i += 16) {
            uint32_t count = (n + 3) / 4 - i;
            if (count > 16) count = 16;
            if (core->link_write(0x4000 + i * 4, words + i, count) != ZM_OK) return 0;
        }
        ++seq;
        if (link_wr(0x48, (seq << 16) | n) != ZM_OK) return 0;
        uint32_t accepted = 0, received = 0;
        const uint32_t t = core->millis();
        do {
            core->delay(1);
            if (!rom_status(&s)) return 0;
            if (!(s & 2) && link_rd(0x48, &accepted) == ZM_OK && accepted == seq) break;
        } while (running && core->millis() - t < 1000);
        if (accepted != seq || (s & 2) || (s & 8) ||
            link_rd(0x4c, &received) != ZM_OK || received != offset + n) return 0;
        offset += n;
        if (load_progress && ((seq & 15) == 0 || offset == size)) load_progress(offset, size);
    }
    uint32_t crc = 0, count = 0, err = 1;
    if (!running || exit_req || !rom_status(&s) || (s & 14) != 4 ||
        link_rd(0x4c, &count) != ZM_OK || count != size || link_rd(0x50, &crc) != ZM_OK ||
        crc != rom_crc(p, size) || link_rd(0x54, &err) != ZM_OK || err) return 0;
#ifdef NES_BATTERY_SAVE
    if (!save_restore()) return 0;
#endif
    if (link_wr(0x44, 2) != ZM_OK) return 0;
    core->delay(1);
    if (!rom_status(&s) || (s & 1)) return 0;
    core->log("nes: cartridge %u bytes, CRC32 %08X verified", (unsigned)size, (unsigned)crc);
    return 1;
}
