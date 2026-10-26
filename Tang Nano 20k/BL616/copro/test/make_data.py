"""Данные проверки сопроцессора из записей bt8xxemu (fpga/vdac2/sim/ft, fpga/vdac2/scripts).

  weather2_ring.bin — поток команд Weather 2: байты, которые Z80 записал в REG_CMDB_WRITE
                      (weather2_spi.txt — запись обмена SPI с bt8xxemu);
  weather2_dl.bin   — RAM_DL bt8xxemu в конце записи (weather2_end.json);
  weather2_regs.txt — REG_CMD_READ, REG_CMD_WRITE, REG_CMD_DL, REG_INT_FLAGS там же;
  gradient.txt      — CMD_GRADIENT: параметры и слова дисплей-листа bt8xxemu
                      (ft_probe_gradient.py и его варианты; JSON — аргументами).

    python make_data.py [замеры_градиента.json ...]
"""
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FT = ROOT / "fpga" / "vdac2" / "sim" / "ft"
OUT = HERE / "data"
REG_CMDB_WRITE = 0x302578


def ring_stream():
    out = bytearray()
    for line in (FT / "weather2_spi.txt").read_text().splitlines():
        parts = line.split()
        mosi = bytes(int(p[:2], 16) for p in parts[2:])
        if len(mosi) < 4 or (mosi[0] & 0xC0) != 0x80:
            continue
        addr = ((mosi[0] & 0x3F) << 16) | (mosi[1] << 8) | mosi[2]
        if addr == REG_CMDB_WRITE:
            data = mosi[3:]
            out += data[:len(data) & ~3]
    return bytes(out)


def main(argv):
    OUT.mkdir(exist_ok=True)
    ring = ring_stream()
    (OUT / "weather2_ring.bin").write_bytes(ring)
    end = json.loads((FT / "weather2_end.json").read_text())
    (OUT / "weather2_dl.bin").write_bytes(bytes.fromhex(end["ram_dl"]))
    regs = end["regs"]
    (OUT / "weather2_regs.txt").write_text(" ".join(str(regs[a]) for a in ("3020f8", "3020fc", "302100", "3020a8")) + "\n")
    lines = []
    for path in argv[1:]:
        for c in json.loads(Path(path).read_text()):
            if isinstance(c, dict):
                p, words = c["p"], c["dl"]
            else:
                p, words = [c[0], c[1], 0, c[2], c[3], 0xFFFFFF], c[4]
            lines.append(" ".join(str(v) for v in p) + " " + str(len(words)) + " " +
                         " ".join(f"{w:08x}" for w in words))
    if lines:
        (OUT / "gradient.txt").write_text("\n".join(lines) + "\n")
    print("ring", len(ring), "bytes; gradient cases", len(lines))


if __name__ == "__main__":
    main(sys.argv)
