"""Offline validation of the separate READ1 snapshot; no serial or flash access.

Run with the project's Python environment containing the z80 emulator:
    python -B tools/test_esp_info_tsvdac_read1.py
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_spi_link_z80 as harness

BASE = ROOT / "tools" / "esp_info_tsvdac.sna"
READ1 = ROOT / "tools" / "esp_info_tsvdac_read1.sna"
BASE_HASH = "79d98e0dee10a2550be34f2727940f5fc1d9768ecb61a7886eece89bf27bb41d"
ENTRY = 0x8663
TITLE = 0x8363
READ1_CODE = bytes.fromhex("3E 02 CD 79 86 ED 78 ED 78 C9")
READ2_CODE = bytes.fromhex("3E 02 CD 79 86 ED 78 ED 78 ED 78 C9")
TEST_TITLE = b"TS-VDAC ESP32-S3 READ1 TEST\r\0"


def offset(address: int) -> int:
    return 27 + address - 0x4000


class Read1Test(unittest.TestCase):
    def test_snapshot_changes_only_reader_and_title(self) -> None:
        base, variant = BASE.read_bytes(), READ1.read_bytes()
        self.assertEqual(hashlib.sha256(base).hexdigest(), BASE_HASH,
                         "The base changed; recheck the experiment's addresses.")
        self.assertEqual(len(base), 49179)
        self.assertEqual(len(variant), len(base))
        self.assertEqual(variant[:27], base[:27], "Original SNA header retained")
        self.assertEqual(variant[offset(ENTRY):offset(ENTRY) + len(READ1_CODE)],
                         READ1_CODE)
        self.assertEqual(variant[offset(TITLE):offset(TITLE) + len(TEST_TITLE)],
                         TEST_TITLE)
        allowed = set(range(offset(ENTRY), offset(ENTRY) + len(READ1_CODE)))
        allowed.update(range(offset(TITLE), offset(TITLE) + len(TEST_TITLE)))
        changed = {i for i, (a, b) in enumerate(zip(base, variant)) if a != b}
        self.assertTrue(changed)
        self.assertTrue(changed <= allowed, "Unexpected changes outside the experiment")

    def run_snapshot(self, current: bool, two_discards: bool = False):
        saved_path = harness.INFO_SNA
        try:
            harness.INFO_SNA = READ1
            machine = harness.InfoMachine()
        finally:
            harness.INFO_SNA = saved_path
        self.assertEqual(machine.symbols["SpiLink_OpenRead"], ENTRY)
        self.assertEqual(machine.symbols["MsgTitle"], TITLE)
        machine.ports.current = current
        if two_discards:
            # A control experiment in emulated RAM only. No files are patched.
            machine.memory[ENTRY:ENTRY + len(READ2_CODE)] = READ2_CODE
        app = harness.EspApp()
        machine.app = app
        lines = machine.run()
        self.assertIn(TEST_TITLE[:-2].decode(), lines)
        self.assertFalse(machine.slave.selected)
        self.assertEqual(machine.slave.overruns, 0)
        return lines, app

    def test_read1_current_result_completes_ping_and_sysinfo(self) -> None:
        lines, app = self.run_snapshot(current=True)
        self.assertIn("ESP link over SPI: READY", lines)
        self.assertEqual(app.commands, [harness.CMD_PING, harness.CMD_SYS_INFO])

    def test_read1_previous_result_does_not_claim_success(self) -> None:
        lines, app = self.run_snapshot(current=False)
        self.assertIn("TS-VDAC firmware not found", lines)
        self.assertNotIn(harness.CMD_SYS_INFO, app.commands)

    def test_read2_previous_result_control_succeeds(self) -> None:
        lines, app = self.run_snapshot(current=False, two_discards=True)
        self.assertIn("ESP link over SPI: READY", lines)
        self.assertEqual(app.commands, [harness.CMD_PING, harness.CMD_SYS_INFO])

    def test_read2_current_result_control_fails(self) -> None:
        lines, app = self.run_snapshot(current=True, two_discards=True)
        self.assertIn("TS-VDAC firmware not found", lines)
        self.assertNotIn(harness.CMD_SYS_INFO, app.commands)


if __name__ == "__main__":
    unittest.main(verbosity=2)
