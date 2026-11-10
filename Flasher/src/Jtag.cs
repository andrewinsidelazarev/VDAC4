// SPDX-License-Identifier: Apache-2.0
// JTAG поверх MPSSE (FT2232 USB-JTAG Tang): автомат ТАП и сдвиги IR/DR. Переходы ТАП и разбиение на
// команды MPSSE — как в openFPGALoader (jtag.cpp, ftdiJtagMPSSE.cpp, Apache-2.0): TMS — командой 0x4B
// (до 6 тактов, бит 7 — TDI, бит за последним повторяет TMS), данные — младшим битом вперёд, запись
// по спаду TCK, чтение по фронту; последний бит сдвига — вместе с TMS = 1 (0x6B).
// Два обхода ошибок прошивки FPGA Partner Sipeed (openFPGALoader узнаёт их по серийному номеру):
//  - такты без данных (0x8E/0x8F) в одной версии не работают — такты здесь всегда командой TMS;
//  - запись без чтения (0x19) в другой версии идёт не тем порядком битов — данные здесь всегда
//    записью с чтением (0x39/0x3B), прочитанное, если не нужно, выбрасывается.
using System;
using System.Collections.Generic;

namespace Vdac4Flasher
{
    public enum Tap
    {
        Reset, Idle, SelectDr, CaptureDr, ShiftDr, Exit1Dr, PauseDr, Exit2Dr, UpdateDr,
        SelectIr, CaptureIr, ShiftIr, Exit1Ir, PauseIr, Exit2Ir, UpdateIr
    }

    public class Jtag
    {
        readonly IMpsse dev;
        readonly List<byte> cmd = new List<byte>(1 << 16);
        readonly List<bool> tms = new List<bool>();
        readonly List<Action<byte[], int>> readers = new List<Action<byte[], int>>();
        readonly List<int> readerBytes = new List<int>();
        int pendingRead;
        public Tap State { get; private set; }
        byte curTdi = 1;

        public Jtag(IMpsse dev) { this.dev = dev; State = Tap.Reset; }

        /// <summary>Начальная настройка MPSSE: делитель, выводы TCK/TDI/TMS — выходы, TMS = 1; ТАП — в Reset.</summary>
        public void Init(uint hz)
        {
            cmd.Add(0x85);              // без петли TDI → TDO
            cmd.Add(0x8A);              // без деления на 5 (60 МГц)
            cmd.Add(0x97);              // без адаптивного такта
            cmd.Add(0x8D);              // без трёхфазного такта
            SetClockCmd(hz);
            cmd.Add(0x80); cmd.Add(0x08); cmd.Add(0x0B);   // ADBUS: TCK 0, TDI 0, TMS 1; выходы TCK, TDI, TMS
            cmd.Add(0x82); cmd.Add(0x00); cmd.Add(0x00);   // ACBUS — входы
            Flush();
            GoReset();
        }

        public void SetClock(uint hz) { FlushTms(); SetClockCmd(hz); }

        void SetClockCmd(uint hz)
        {
            const uint baseHz = 60000000;
            uint presc = (baseHz / hz - 1) / 2;
            if (baseHz / ((1 + presc) * 2) > hz) presc++;
            cmd.Add(0x86); cmd.Add((byte)presc); cmd.Add((byte)(presc >> 8));
        }

        public void GoReset()
        {
            for (int i = 0; i < 6; i++) tms.Add(true);
            FlushTms();
            State = Tap.Reset;
        }

        /// <summary>Переход ТАП в newState кратчайшим путём (как Jtag::set_state).</summary>
        public void SetState(Tap newState, byte tdi = 1)
        {
            curTdi = tdi;
            while (newState != State) {
                bool t;
                switch (State) {
                case Tap.Reset: t = newState == Tap.Reset; if (!t) State = Tap.Idle; break;
                case Tap.Idle: t = newState != Tap.Idle; if (t) State = Tap.SelectDr; break;
                case Tap.SelectDr:
                    t = !IsDr(newState);
                    State = t ? Tap.SelectIr : Tap.CaptureDr;
                    break;
                case Tap.SelectIr:
                    t = !IsIr(newState);
                    State = t ? Tap.Reset : Tap.CaptureIr;
                    break;
                case Tap.CaptureDr: t = newState != Tap.ShiftDr; State = t ? Tap.Exit1Dr : Tap.ShiftDr; break;
                case Tap.ShiftDr: t = true; State = Tap.Exit1Dr; break;
                case Tap.Exit1Dr:
                    t = !(newState == Tap.PauseDr || newState == Tap.Exit2Dr || newState == Tap.ShiftDr);
                    State = t ? Tap.UpdateDr : Tap.PauseDr;
                    break;
                case Tap.PauseDr: t = true; State = Tap.Exit2Dr; break;
                case Tap.Exit2Dr:
                    t = !(newState == Tap.ShiftDr || newState == Tap.Exit1Dr || newState == Tap.PauseDr);
                    State = t ? Tap.UpdateDr : Tap.ShiftDr;
                    break;
                case Tap.UpdateDr:
                case Tap.UpdateIr:
                    t = newState != Tap.Idle;
                    State = t ? Tap.SelectDr : Tap.Idle;
                    break;
                case Tap.CaptureIr: t = newState != Tap.ShiftIr; State = t ? Tap.Exit1Ir : Tap.ShiftIr; break;
                case Tap.ShiftIr: t = true; State = Tap.Exit1Ir; break;
                case Tap.Exit1Ir:
                    t = !(newState == Tap.PauseIr || newState == Tap.Exit2Ir || newState == Tap.ShiftIr);
                    State = t ? Tap.UpdateIr : Tap.PauseIr;
                    break;
                case Tap.PauseIr: t = true; State = Tap.Exit2Ir; break;
                case Tap.Exit2Ir:
                    t = !(newState == Tap.ShiftIr || newState == Tap.Exit1Ir || newState == Tap.PauseIr);
                    State = t ? Tap.UpdateIr : Tap.ShiftIr;
                    break;
                default: throw new InvalidOperationException("TAP");
                }
                tms.Add(t);
            }
        }

        static bool IsDr(Tap s) { return s >= Tap.CaptureDr && s <= Tap.UpdateDr; }
        static bool IsIr(Tap s) { return s >= Tap.CaptureIr && s <= Tap.UpdateIr; }

        /// <summary>Такты на месте (TMS не меняется: 1 в Reset, 0 в прочих устойчивых состояниях).</summary>
        public void ToggleClk(int n)
        {
            FlushTms();
            bool t = State == Tap.Reset;
            for (int i = 0; i < n; i++) tms.Add(t);
            FlushTms();
        }

        void FlushTms()
        {
            int i = 0;
            while (i < tms.Count) {
                int n = Math.Min(6, tms.Count - i);
                int v = curTdi << 7;
                bool last = false;
                for (int k = 0; k < n; k++) { last = tms[i + k]; if (last) v |= 1 << k; }
                if (last) v |= 1 << n;
                cmd.Add(0x4B); cmd.Add((byte)(n - 1)); cmd.Add((byte)v);
                i += n;
            }
            tms.Clear();
        }

        public void ShiftIr(byte ir, Tap end = Tap.Idle)
        {
            Shift(false, new[] { ir }, 8, null, end);
        }

        /// <summary>Сдвиг DR: nbits битов из tdi (младшим вперёд; null — нули); прочитанное — в tdo после Flush.</summary>
        public void ShiftDr(byte[] tdi, int nbits, byte[] tdo, Tap end = Tap.Idle)
        {
            Shift(true, tdi, nbits, tdo, end);
        }

        void Shift(bool dr, byte[] tdi, int nbits, byte[] tdo, Tap end)
        {
            Tap shift = dr ? Tap.ShiftDr : Tap.ShiftIr;
            if (State != shift) SetState(shift);
            FlushTms();
            bool last = end != shift;
            int real = last ? nbits - 1 : nbits;
            int nbyte = real / 8, nbit = real % 8;
            int pos = 0;
            while (pos < nbyte) {
                int chunk = Math.Min(4096, nbyte - pos);
                cmd.Add(0x39); cmd.Add((byte)(chunk - 1)); cmd.Add((byte)((chunk - 1) >> 8));
                for (int k = 0; k < chunk; k++) cmd.Add(tdi != null ? tdi[pos + k] : (byte)0);
                int at = pos;
                Expect(chunk, (buf, off) => { if (tdo != null) Array.Copy(buf, off, tdo, at, chunk); });
                pos += chunk;
                if (pendingRead > 32768) Flush();
            }
            if (nbit > 0) {
                cmd.Add(0x3B); cmd.Add((byte)(nbit - 1)); cmd.Add(tdi != null ? tdi[nbyte] : (byte)0);
                int at = nbyte, bits = nbit;
                Expect(1, (buf, off) => {
                    if (tdo != null) tdo[at] = (byte)((tdo[at] & ~((1 << bits) - 1)) | (buf[off] >> (8 - bits)));
                });
            }
            if (last) {
                int lb = nbits - 1;
                bool bit = tdi != null && (tdi[lb / 8] >> (lb % 8) & 1) != 0;
                cmd.Add(0x6B); cmd.Add(0x00); cmd.Add((byte)(bit ? 0x81 : 0x01));
                Expect(1, (buf, off) => {
                    if (tdo == null) return;
                    int m = 1 << (lb % 8);
                    if ((buf[off] & 0x80) != 0) tdo[lb / 8] |= (byte)m; else tdo[lb / 8] &= (byte)~m;
                });
                State = dr ? Tap.Exit1Dr : Tap.Exit1Ir;
                SetState(end);
            }
        }

        void Expect(int n, Action<byte[], int> reader)
        {
            readers.Add(reader);
            readerBytes.Add(n);
            pendingRead += n;
        }

        /// <summary>Отправить накопленное и разобрать ответы.</summary>
        public void Flush()
        {
            FlushTms();
            if (pendingRead > 0) cmd.Add(0x87);          // отправить ответ сразу
            if (cmd.Count > 0) {
                dev.Write(cmd.ToArray(), cmd.Count);
                cmd.Clear();
            }
            if (pendingRead > 0) {
                var buf = new byte[pendingRead];
                dev.Read(buf, pendingRead, 5000);
                int off = 0;
                for (int i = 0; i < readers.Count; i++) {
                    readers[i](buf, off);
                    off += readerBytes[i];
                }
                readers.Clear();
                readerBytes.Clear();
                pendingRead = 0;
            }
        }
    }
}
