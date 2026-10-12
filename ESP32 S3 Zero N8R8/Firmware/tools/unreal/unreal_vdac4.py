"""Управление Unreal с ESP платы VDAC4 (папка E:\\zx\\unreal_x64\\vdac4).

Клавиши и снимки — файлами-запросами, которые опрашивает сам эмулятор
(vdac4_auto.cpp): окну не нужен фокус, клавиатура ПК не трогается. Кадр FT812
(TS_VDAC2=1) Unreal показывает в отдельном окне — его снимает ft812_shot.

    u = Unreal(); u.start(); u.wait(8); u.shot('boot.png'); u.keys('DOWN', 'ENTER'); u.quit()
"""
import ctypes
import struct
import subprocess
import time
from ctypes import wintypes
from pathlib import Path

RUN = Path(r"E:\zx\unreal_x64\vdac4")

# Коды DirectInput (DIK_*).
DIK = {
    "ESC": 0x01, "1": 0x02, "2": 0x03, "3": 0x04, "4": 0x05, "5": 0x06, "6": 0x07, "7": 0x08,
    "8": 0x09, "9": 0x0A, "0": 0x0B, "BACK": 0x0E, "TAB": 0x0F,
    "Q": 0x10, "W": 0x11, "E": 0x12, "R": 0x13, "T": 0x14, "Y": 0x15, "U": 0x16, "I": 0x17,
    "O": 0x18, "P": 0x19, "ENTER": 0x1C, "LCTRL": 0x1D, "A": 0x1E, "S": 0x1F, "D": 0x20,
    "F": 0x21, "G": 0x22, "H": 0x23, "J": 0x24, "K": 0x25, "L": 0x26, "LSHIFT": 0x2A,
    "Z": 0x2C, "X": 0x2D, "C": 0x2E, "V": 0x2F, "B": 0x30, "N": 0x31, "M": 0x32, "LALT": 0x38,
    "SPACE": 0x39, "F1": 0x3B, "F2": 0x3C, "F3": 0x3D, "F4": 0x3E, "F5": 0x3F, "F6": 0x40,
    "F7": 0x41, "F8": 0x42, "F9": 0x43, "F10": 0x44,
    "HOME": 0xC7, "UP": 0xC8, "PGUP": 0xC9, "LEFT": 0xCB, "RIGHT": 0xCD, "END": 0xCF,
    "DOWN": 0xD0, "PGDN": 0xD1,
}


class Unreal:
    def __init__(self, run: Path = RUN) -> None:
        self.run = run
        self.process: subprocess.Popen | None = None

    def start(self) -> None:
        for name in ("vdac4_keys.req", "vdac4_shot.req", "vdac4_quit.req"):
            (self.run / name).unlink(missing_ok=True)
        self.console = open(self.run / "unreal_console.log", "wb")
        self.process = subprocess.Popen([str(self.run / "Unreal_vdac4.exe")], cwd=self.run,
                                        stdout=self.console, stderr=subprocess.STDOUT)

    def wait(self, seconds: float) -> None:
        time.sleep(seconds)

    def keys(self, *presses: str, hold: int = 3, gap: int = 4) -> None:
        """Нажатия по очереди: «DOWN», «LSHIFT+A»; «DOWN*5» — пять раз."""
        lines = []
        for press in presses:
            count = 1
            if "*" in press:
                press, times = press.split("*")
                count = int(times)
            codes = "+".join(f"{DIK[name]:02X}" for name in press.split("+"))
            lines += [f"{codes} {hold} {gap}"] * count
        request = self.run / "vdac4_keys.req"
        while request.exists():
            time.sleep(0.05)
        temp = request.with_suffix(".tmp")
        temp.write_text("\n".join(lines) + "\n", encoding="ascii")
        temp.replace(request)
        time.sleep(len(lines) * (hold + gap) / 50 + 0.2)

    def shot(self, path: str | Path, timeout: float = 10) -> Path:
        path = Path(path).resolve()
        path.unlink(missing_ok=True)
        request = self.run / "vdac4_shot.req"
        temp = request.with_suffix(".tmp")
        temp.write_text(str(path), encoding="ascii")
        temp.replace(request)
        end = time.time() + timeout
        while time.time() < end:
            if path.exists():
                time.sleep(0.1)
                return path
            time.sleep(0.05)
        raise TimeoutError("Unreal не сделал снимок")

    def ft812_shot(self, path: str | Path) -> Path:
        """Кадр FT812 — окно «VDAC2 Emul» (класс FT_WND, ft812.cpp) — в BMP.

        Окно стоит под остальными (HWND_BOTTOM), поэтому PrintWindow с
        PW_RENDERFULLCONTENT: содержимое окна берётся у DWM, перекрытие не мешает."""
        user32 = ctypes.WinDLL("user32")
        gdi32 = ctypes.WinDLL("gdi32")
        user32.FindWindowA.restype = wintypes.HWND
        user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        user32.GetDC.restype = wintypes.HDC
        user32.GetDC.argtypes = [wintypes.HWND]
        user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
        user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
        gdi32.CreateCompatibleDC.restype = wintypes.HDC
        gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
        gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
        gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
        gdi32.SelectObject.restype = wintypes.HGDIOBJ
        gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
        gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT,
                                    ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
        gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
        gdi32.DeleteDC.argtypes = [wintypes.HDC]
        user32.SetProcessDPIAware()
        window = user32.FindWindowA(b"FT_WND", None)
        if not window:
            raise RuntimeError("нет окна FT812 (TS_VDAC2=0 или Unreal не запущен)")
        rect = wintypes.RECT()
        user32.GetClientRect(window, ctypes.byref(rect))
        width, height = rect.right, rect.bottom
        screen = user32.GetDC(None)
        dc = gdi32.CreateCompatibleDC(screen)
        bitmap = gdi32.CreateCompatibleBitmap(screen, width, height)
        gdi32.SelectObject(dc, bitmap)
        try:
            if not user32.PrintWindow(window, dc, 1 | 2):  # PW_CLIENTONLY | PW_RENDERFULLCONTENT
                raise RuntimeError("PrintWindow не снял окно FT812")
            # BITMAPINFOHEADER: 32 бита на точку, строки сверху вниз (высота < 0).
            info = ctypes.create_string_buffer(
                struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, 0, 0, 0, 0, 0), 40)
            pixels = ctypes.create_string_buffer(width * height * 4)
            if gdi32.GetDIBits(dc, bitmap, 0, height, pixels, info, 0) != height:
                raise RuntimeError("GetDIBits не отдал кадр FT812")
        finally:
            gdi32.DeleteObject(bitmap)
            gdi32.DeleteDC(dc)
            user32.ReleaseDC(None, screen)
        path = Path(path).resolve()
        header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(pixels.raw), 0, 0, 14 + 40)
        path.write_bytes(header + info.raw + pixels.raw)
        return path

    def state(self, timeout: float = 5) -> str:
        """PC, SP, IFF1, HALT и байты от PC и стека Z80 (vdac4_state.req)."""
        out = self.run / "vdac4_state.txt"
        out.unlink(missing_ok=True)
        request = self.run / "vdac4_state.req"
        temp = request.with_suffix(".tmp")
        temp.write_text(str(out), encoding="ascii")
        temp.replace(request)
        end = time.time() + timeout
        while time.time() < end:
            if out.exists():
                time.sleep(0.1)
                return out.read_text(encoding="ascii")
            time.sleep(0.05)
        raise TimeoutError("Unreal не записал состояние")

    def quit(self) -> None:
        (self.run / "vdac4_quit.req").write_text("", encoding="ascii")
        if self.process is not None:
            try:
                self.process.wait(10)
            except subprocess.TimeoutExpired:
                self.process.kill()

    def esp_log(self) -> str:
        log = self.run / "vdac4_esp.log"
        return log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
