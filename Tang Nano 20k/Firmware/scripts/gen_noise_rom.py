"""Таблица порогов дизеринга записи кадра (vp_fbwrite): синий шум 64×64, пороги 0…15.

Синий шум — метод «пустот и скоплений» (void-and-cluster, Ulichney 1993) на торе 64×64 с гауссовым
ядром σ = 1,5: ранги точек равномерны, рядом стоящие пороги разнятся — зерно ровное, без комков и
без узора. Ранг → порог: ранг·16/4096.

Пишет rtl/vp_noise_rom.sv (ПЗУ 2048 × 8: адрес {y[5:0], x[5:1]}, байт — {порог нечётной x,
порог чётной x}) и tb/jpeg/vp_noise64.bin (4096 байт, порог строка за строкой — эталон стенда).
Запуск из fpga/vdac2:  python scripts/gen_noise_rom.py
"""
from pathlib import Path

import numpy as np

N = 64


def kernel(sig=1.5):
    d = np.minimum(np.arange(N), N - np.arange(N))
    return np.exp(-(d[:, None] ** 2 + d[None] ** 2) / (2 * sig * sig))


def blue_noise(seed=1):
    rng = np.random.default_rng(seed)
    kf = np.fft.fft2(kernel())

    def energy(m):
        return np.real(np.fft.ifft2(np.fft.fft2(m.astype(float)) * kf))

    ones = rng.random((N, N)) < 0.1
    while True:                                    # исходный узор: из скопления — в пустоту
        c = np.argmax(np.where(ones, energy(ones), -1e9))
        ones.flat[c] = False
        v = np.argmin(np.where(ones, 1e9, energy(ones)))
        ones.flat[v] = True
        if v == c:
            break
    rank = np.zeros(N * N, int)
    n1 = int(ones.sum())
    m = ones.copy()
    for r in range(n1 - 1, -1, -1):                # ранги единиц: убираем самое плотное
        c = np.argmax(np.where(m, energy(m), -1e9))
        m.flat[c] = False
        rank[c] = r
    m = ones.copy()
    e = energy(m)
    g = kernel()
    for r in range(n1, N * N):                     # остальные — в самую пустую точку
        v = np.argmin(np.where(m, 1e18, e))
        m.flat[v] = True
        rank[v] = r
        y, x = divmod(v, N)
        e += np.roll(np.roll(g, y, 0), x, 1)
    return (rank.reshape(N, N) * 16 // (N * N)).astype(np.uint8)


def main():
    root = Path(__file__).resolve().parents[1]
    t = blue_noise()
    assert (np.bincount(t.ravel(), minlength=16) == N * N // 16).all()
    (root / 'tb' / 'jpeg' / 'vp_noise64.bin').write_bytes(t.tobytes())
    lines = [
        '// SPDX-License-Identifier: Apache-2.0',
        '//',
        '// ПЗУ порогов дизеринга записи кадра (vp_fbwrite): синий шум 64×64, пороги 0…15.',
        '// СГЕНЕРИРОВАНО scripts/gen_noise_rom.py — не править руками. Адрес {y[5:0], x[5:1]},',
        '// байт — {порог нечётной x, порог чётной x}; чтение — регистром (pROM).',
        '`default_nettype none',
        'module vp_noise_rom (',
        '    input  wire        clk,',
        '    input  wire [10:0] addr,',
        '    output reg  [7:0]  q',
        ');',
        '    always_ff @(posedge clk) begin',
        '        case (addr)',
    ]
    for y in range(N):
        for xw in range(N // 2):
            lines.append("            11'd%d: q <= 8'h%02X;" % (y * 32 + xw, t[y, 2 * xw + 1] << 4 | t[y, 2 * xw]))
    lines += ["            default: q <= 8'h00;", '        endcase', '    end', 'endmodule', '`default_nettype wire', '']
    (root / 'rtl' / 'vp_noise_rom.sv').write_bytes('\n'.join(lines).encode('utf-8'))
    print('rtl/vp_noise_rom.sv, tb/jpeg/vp_noise64.bin')


if __name__ == '__main__':
    main()
