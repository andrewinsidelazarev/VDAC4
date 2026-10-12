"""Packaging of the ESP-IDF build for the VDAC4 A3.1 board.

Takes the outputs of `idf.py build` and produces in firmware/vdac4 the same
set of files PlatformIO packages for the ZiFi adapter: an application image
for the network update, a merged factory image to be written at address 0x0
through USB, and the manifest with the version and SHA-256 of both.

Run with the Python of the ESP-IDF environment (it has esptool):

    python tools/package_vdac4.py [--build build] [--output firmware/vdac4]

The script only writes files; it never talks to the board.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
SAFE_VERSION = re.compile(r"[A-Za-z0-9._+-]+")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def flash_size_bytes(text: str) -> int:
    match = re.fullmatch(r"(\d+)MB", text)
    if not match:
        raise RuntimeError(f"Unexpected flash size in flasher_args.json: {text}")
    return int(match.group(1)) * 1024 * 1024


def build_version(build_dir: Path) -> str:
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^ZIFI_VDAC4_VERSION:[A-Z]+=(.+)$", cache, re.MULTILINE)
    if not match or not SAFE_VERSION.fullmatch(match.group(1).strip()):
        raise RuntimeError("ZIFI_VDAC4_VERSION is missing or unsafe in CMakeCache.txt")
    return match.group(1).strip()


def package(build_dir: Path, output: Path) -> None:
    arguments = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    if arguments["extra_esptool_args"]["chip"] != "esp32s3":
        raise RuntimeError("The build directory is not an ESP32-S3 build")
    app_source = build_dir / arguments["app"]["file"]
    files = [
        (int(offset, 16), build_dir / name)
        for offset, name in arguments["flash_files"].items()
    ]
    files.sort()
    missing = [str(path) for _, path in files if not path.is_file()]
    if missing:
        raise RuntimeError("Firmware packaging inputs are missing: " + ", ".join(missing))

    version = build_version(build_dir)
    # Версия зашита в образ строкой: манифест не должен расходиться с тем, что
    # прошивка сообщит о себе в SYS_INFO.
    if version.encode("ascii") not in app_source.read_bytes():
        raise RuntimeError(
            f"{app_source.name} does not contain version {version}: rebuild the firmware"
        )

    output.mkdir(parents=True, exist_ok=True)
    app_output = output / "firmware.bin"
    factory_output = output / "firmware.factory.bin"
    shutil.copy2(app_source, app_output)
    if factory_output.exists():
        factory_output.unlink()

    command = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "merge-bin",
        "-o", str(factory_output),
        *arguments["write_flash_args"],
    ]
    for offset, path in files:
        command += [hex(offset), str(path)]
    subprocess.run(command, check=True)
    limit = flash_size_bytes(arguments["flash_settings"]["flash_size"])
    if factory_output.stat().st_size > limit:
        factory_output.unlink()
        raise RuntimeError("Merged factory image exceeds the flash size")

    (output / "firmware.sha256").write_text(
        f"VERSION {version}\n"
        f"{sha256(app_output)}  firmware.bin\n"
        f"{sha256(factory_output)}  firmware.factory.bin\n",
        encoding="ascii",
        newline="\n",
    )
    (output / "flash_args.txt").write_text(
        "0x0 firmware.factory.bin\n",
        encoding="ascii",
        newline="\n",
    )
    print(f"Version:  {version}")
    print(f"Packaged: {app_output}")
    print(f"Packaged: {factory_output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, default=PROJECT / "build",
                        help="ESP-IDF build directory (default: build)")
    parser.add_argument("--output", type=Path, default=PROJECT / "firmware" / "vdac4",
                        help="where to put the packaged files (default: firmware/vdac4)")
    options = parser.parse_args()
    try:
        package(options.build.resolve(), options.output.resolve())
    except (OSError, RuntimeError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
