"""Tests for tools/wp4/myems_ac_meter_sim.py (DP-WP4 D4).

SIM-001..SIM-012 of the DP-WP4 Verification & Test Specification v1.1
(Section 18), plus framing and configuration checks. No serial port or
pyserial needed: the serial link is replaced by an in-memory fake with a
simulated clock.

Run from the project root:
    python -m unittest discover -s tools/wp4/tests -v
"""
from __future__ import annotations

import contextlib
import io
import math
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import myems_ac_meter_sim as sim_mod  # noqa: E402
from myems_ac_meter_sim import Simulator, crc16, parse_mode, valid_crc, with_crc  # noqa: E402

ROOT = sim_mod.PROJECT_ROOT
SITE = ROOT / "config" / "site"
LAB = sim_mod.load_layout(SITE / "lab_bench.site.json")
LAB2 = sim_mod.load_layout(SITE / "lab_bench_2dev.site.json")
DECODER = sim_mod.load_layout(SITE / "lab_decoder_test.site.json")

# Test Spec 4.1, register words 0..18 (NORMAL, import case)
SPEC_4_1 = ("08FC 0906 08F7 03E8 03F2 03DE 0000 30D4 0000 0BB8 0000 3237 03CC 1388 "
            "0000 2729 0000 0000 0000").split()
# Test Spec 4.2, registers 1000..1019
SPEC_4_2 = ("4366 8000 8000 4366 6643 0080 0080 6643 FFFF CF2C CF2C FFFF 2729 0000 "
            "7FC0 0000 7F80 0000 8000 FFFF").split()


def read_req(slave: int, start: int, count: int, fc: int = 3) -> bytes:
    return with_crc(bytes((slave, fc, start >> 8, start & 0xFF, count >> 8, count & 0xFF)))


def words(resp: bytes) -> list[str]:
    """Register words of a normal read response, as 4-digit hex strings."""
    assert valid_crc(resp), resp.hex()
    n = resp[2]
    assert len(resp) == 3 + n + 2
    return [f"{resp[3 + i]:02X}{resp[4 + i]:02X}" for i in range(0, n, 2)]


def u32(ws: list[str], i: int) -> int:
    return int(ws[i], 16) << 16 | int(ws[i + 1], 16)


class Clock:
    def __init__(self, t: float = 1000.0):
        self.t = t

    def __call__(self) -> float:
        return self.t


def make(layout=LAB, seed: int = 1, mode: str | None = None, t: float = 1000.0):
    clk = Clock(t)
    lines: list[str] = []
    s = Simulator(layout, seed=seed, log=lines.append, clock=clk)
    if mode:
        s.set_mode("*", parse_mode(mode))
    return s, clk, lines


def read_all(s: Simulator, slave: int = 1) -> list[str]:
    r = s.handle(read_req(slave, 0, 19))
    return words(r.data)


# ================================================================ SIM tests ===
class SimTests(unittest.TestCase):

    def test_SIM001_normal_image_equals_vectors_4_1(self):
        s, _, _ = make()
        self.assertEqual(SPEC_4_1, read_all(s))
        # the three poll groups as the firmware reads them
        self.assertEqual(SPEC_4_1[0:14], words(s.handle(read_req(1, 0, 14)).data))
        self.assertEqual(SPEC_4_1[14:18], words(s.handle(read_req(1, 14, 4)).data))
        self.assertEqual(SPEC_4_1[18:19], words(s.handle(read_req(1, 18, 1)).data))

    def test_SIM001_decoder_image_equals_vectors_4_2(self):
        s, _, _ = make(DECODER)
        self.assertEqual(SPEC_4_2, words(s.handle(read_req(1, 1000, 20)).data))

    def test_SIM001_normal_is_static(self):
        s, clk, _ = make()
        first = read_all(s)
        clk.t += 3600
        self.assertEqual(first, read_all(s))

    def test_SIM002_load_change_is_deterministic(self):
        def run(seed):
            s, clk, _ = make(seed=seed, mode="LOAD_CHANGE:10")
            out = []
            for k in range(6):
                out.append(read_all(s)[0:14])
                clk.t += 10.0
            return out
        a, b, c = run(7), run(7), run(8)
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)
        self.assertGreater(len({tuple(x) for x in a}), 1, "load must actually change between steps")
        for img in a:
            self.assertEqual(SPEC_4_1[0:3], img[0:3])          # voltages unchanged
            p = u32(img, 6)
            self.assertTrue(0 < p < 0x80000000)               # import (positive)

    def test_SIM002_load_constant_within_a_step(self):
        s, clk, _ = make(seed=3, mode="LOAD_CHANGE:10")
        a = read_all(s)[0:14]
        clk.t += 9.9
        self.assertEqual(a, read_all(s)[0:14])

    def test_SIM002_independent_of_request_timing(self):
        s1, c1, _ = make(seed=5, mode="LOAD_CHANGE:10")
        s2, c2, _ = make(seed=5, mode="LOAD_CHANGE:10")
        c1.t += 35.0
        for _ in range(35):                 # s2 polled every second, s1 only at the end
            c2.t += 1.0
            read_all(s2)
        self.assertEqual(read_all(s1)[0:14], read_all(s2)[0:14])

    def test_SIM003_import(self):
        s, clk, _ = make(mode="IMPORT")
        w = read_all(s)
        self.assertEqual(12500, u32(w, 6))
        self.assertEqual(10025, u32(w, 14))
        clk.t += 3600.0                      # 12.5 kW for 1 h
        w = read_all(s)
        self.assertEqual(10025 + 1250, u32(w, 14))
        self.assertEqual(0, u32(w, 16))      # export unchanged

    def test_SIM003_counter_moves_in_small_steps(self):
        s, clk, _ = make(mode="IMPORT")
        seen = []
        for _ in range(10):
            clk.t += 2.88                    # 12.5 kW x 2.88 s = 0.01 kWh
            seen.append(u32(read_all(s), 14))
        self.assertEqual(list(range(10026, 10036)), seen)

    def test_SIM004_export(self):
        s, clk, _ = make(mode="EXPORT")
        w = read_all(s)
        self.assertEqual(["FFFF", "CF2C"], w[6:8])
        self.assertEqual("FC34", w[12])
        clk.t += 3600.0
        w = read_all(s)
        self.assertEqual(1250, u32(w, 16))
        self.assertEqual(10025, u32(w, 14))  # import unchanged

    def test_SIM005_timeout(self):
        s, _, lines = make(mode="TIMEOUT")
        r = s.handle(read_req(1, 0, 14))
        self.assertIsNone(r.data)
        self.assertTrue(any("NO_RESPONSE TIMEOUT" in ln for ln in lines))

    def test_SIM006_exception_02(self):
        s, _, _ = make(mode="EXCEPTION:02")
        r = s.handle(read_req(1, 0, 14))
        self.assertEqual(with_crc(bytes((1, 0x83, 2))), r.data)

    def test_SIM006_exception_targeted_at_group(self):
        s, _, _ = make(mode="EXCEPTION:02@energy")
        self.assertEqual(SPEC_4_1[0:14], words(s.handle(read_req(1, 0, 14)).data))
        self.assertEqual(with_crc(bytes((1, 0x83, 2))), s.handle(read_req(1, 14, 4)).data)
        self.assertEqual(SPEC_4_1[18:19], words(s.handle(read_req(1, 18, 1)).data))

    def test_SIM007_bad_length(self):
        s, _, _ = make(mode="BAD_LENGTH")
        r = s.handle(read_req(1, 0, 14)).data
        self.assertTrue(valid_crc(r))
        self.assertEqual(3 + 28 + 2, len(r))            # payload still 14 registers
        self.assertNotEqual(28, r[2])                    # but byte count says otherwise

    def test_SIM008_bad_crc(self):
        s, _, _ = make(mode="BAD_CRC")
        r = s.handle(read_req(1, 0, 14)).data
        self.assertEqual(3 + 28 + 2, len(r))
        self.assertFalse(valid_crc(r))

    def test_SIM009_out_of_range_frequency(self):
        s, _, _ = make(mode="OUT_OF_RANGE")
        w = read_all(s)
        self.assertEqual(f"{3000:04X}", w[13])
        self.assertEqual(SPEC_4_1[:13], w[:13])

    def test_SIM010_counter_reset_once(self):
        s, clk, lines = make()
        clk.t += 1.0
        s.set_mode("*", parse_mode("COUNTER_RESET"))
        self.assertEqual(0, u32(read_all(s), 14))
        values = []
        for _ in range(5):
            clk.t += 2.88
            values.append(u32(read_all(s), 14))
        self.assertEqual([1, 2, 3, 4, 5], values)     # counts up from the new baseline
        self.assertEqual(1, sum("COUNTER_RESET import counter -> 0" in ln for ln in lines))

    def test_SIM011_slow_400(self):
        s, _, _ = make(mode="SLOW:400")
        r = s.handle(read_req(1, 0, 14))
        self.assertAlmostEqual(0.400, r.delay_s)
        self.assertEqual(SPEC_4_1[0:14], words(r.data))

    def test_SIM011_slow_reply_timing_through_serial_loop(self):
        s, clk, _ = make(mode="SLOW:400")
        port = FakeSerial(clk)
        port.feed(read_req(1, 0, 14), at=clk.t + 0.010)
        run_serve(s, port, until=clk.t + 1.0)
        self.assertEqual(1, len(port.sent))
        t_tx, data = port.sent[0]
        t_rx_done = port.last_rx_time
        self.assertAlmostEqual(0.400, t_tx - t_rx_done, delta=0.020)
        self.assertEqual(SPEC_4_1[0:14], words(data))

    def test_SIM012_writes_rejected_and_image_unchanged(self):
        s, _, _ = make()
        writes = [
            bytes.fromhex("01 05 00 01 FF 00"), bytes.fromhex("01 06 00 00 12 34"),
            bytes.fromhex("01 0F 00 00 00 08 01 55"), bytes.fromhex("01 10 00 00 00 02 04 12 34 AB CD"),
            bytes.fromhex("01 17 00 00 00 02 00 00 00 01 02 00 01"),
            bytes.fromhex("01 01 00 00 00 08"), bytes.fromhex("01 02 00 00 00 08"),
        ]
        for w in writes:
            r = s.handle(with_crc(w)).data
            self.assertEqual(with_crc(bytes((1, w[1] | 0x80, 0x01))), r, w.hex())
        self.assertEqual(SPEC_4_1, read_all(s))


# =============================================================== more modes ===
class ModeTests(unittest.TestCase):

    def test_offline_then_recover(self):
        s, clk, lines = make(mode="OFFLINE_THEN_RECOVER:60")
        self.assertIsNone(s.handle(read_req(1, 18, 1)).data)
        clk.t += 59.9
        self.assertIsNone(s.handle(read_req(1, 18, 1)).data)
        clk.t += 0.2
        self.assertEqual(SPEC_4_1[18:19], words(s.handle(read_req(1, 18, 1)).data))
        self.assertTrue(any("-> NORMAL" in ln for ln in lines))

    def test_two_devices_one_offline(self):
        s, _, _ = make(LAB2)
        s.set_mode("1", parse_mode("TIMEOUT"))
        self.assertIsNone(s.handle(read_req(1, 0, 14)).data)
        r = s.handle(read_req(2, 0, 14)).data
        self.assertEqual(2, r[0])
        self.assertEqual(SPEC_4_1[0:14], words(r))

    def test_mode_parsing(self):
        self.assertEqual("EXCEPTION:02@energy", str(parse_mode("EXCEPTION:02@energy")))
        self.assertEqual(2, parse_mode("EXCEPTION").arg)
        self.assertEqual(0x0B, parse_mode("EXCEPTION:0x0B").arg)
        self.assertEqual(400, parse_mode("SLOW").arg)
        self.assertEqual(60.0, parse_mode("OFFLINE_THEN_RECOVER").arg)
        for bad in ("FOO", "NORMAL:3", "IMPORT@energy", "SLOW:0", "EXCEPTION:300", "timeout"):
            with self.assertRaises(ValueError, msg=bad):
                parse_mode(bad)

    def test_fault_for_unknown_group_never_matches(self):
        s, _, _ = make(mode="TIMEOUT@nosuchgroup")
        self.assertIsNotNone(s.handle(read_req(1, 0, 14)).data)


# ============================================================ request checks ===
class RequestTests(unittest.TestCase):

    def test_address_outside_map_gives_exception_02(self):
        s, _, _ = make()
        for start, count in ((19, 1), (18, 2), (100, 1)):
            self.assertEqual(with_crc(bytes((1, 0x83, 2))), s.handle(read_req(1, start, count)).data)

    def test_wrong_function_code_for_group_gives_exception_02(self):
        s, _, _ = make()
        self.assertEqual(with_crc(bytes((1, 0x84, 2))), s.handle(read_req(1, 0, 14, fc=4)).data)

    def test_quantity_limits(self):
        s, _, _ = make()
        for count in (0, 126):
            self.assertEqual(with_crc(bytes((1, 0x83, 3))), s.handle(read_req(1, 0, count)).data)

    def test_other_slave_broadcast_and_bad_crc_are_ignored(self):
        s, _, _ = make()
        self.assertIsNone(s.handle(read_req(9, 0, 14)).data)
        self.assertIsNone(s.handle(read_req(0, 0, 14)).data)
        bad = bytearray(read_req(1, 0, 14))
        bad[-1] ^= 0x01
        self.assertIsNone(s.handle(bytes(bad)).data)
        self.assertEqual(1, s.counters["bad_request_crc"])
        self.assertEqual(2, s.counters["ignored"])

    def test_overlapping_request_is_logged(self):
        s, clk, lines = make(mode="SLOW:400")
        s.handle(read_req(1, 0, 14))
        clk.t += 0.1
        s.handle(read_req(1, 14, 4))
        self.assertEqual(1, s.counters["overlap"])
        clk.t += 1.0
        s.handle(read_req(1, 14, 4))
        self.assertEqual(1, s.counters["overlap"])

    def test_log_records_mode_and_frames(self):
        s, _, lines = make()
        s.handle(read_req(1, 0, 14))
        rx = [ln for ln in lines if " RX slave=1" in ln]
        self.assertEqual(1, len(rx))
        self.assertIn("mode=NORMAL", rx[0])
        self.assertTrue(any(" TX slave=1 01 03 1C 08 FC" in ln for ln in lines))


# ======================================================== values and layout ===
class ValueTests(unittest.TestCase):

    def test_set_value_for_ratio_vectors_4_3(self):
        s, _, _ = make()
        s.set_value("1", "ac.current.l1", 2.5)
        s.set_value("1", "ac.current.l2", 6.0)
        s.set_value("1", "ac.power.active.total", 1.0)
        w = read_all(s)
        self.assertEqual([f"{250:04X}", f"{600:04X}"], w[3:5])
        self.assertEqual(1000, u32(w, 6))
        s.set_mode("*", parse_mode("NORMAL"))           # overrides survive a mode change
        self.assertEqual(f"{250:04X}", read_all(s)[3])

    def test_set_value_checked_against_register_type(self):
        s, _, _ = make()
        with self.assertRaises(ValueError):
            s.set_value("1", "ac.current.l1", 700.0)     # 70000 > UINT16
        with self.assertRaises(ValueError):
            s.set_value("1", "ac.voltage.l4_n", 1.0)
        with self.assertRaises(ValueError):
            s.set_value("5", "ac.current.l1", 1.0)

    def test_encoding_round_trips_through_an_independent_decoder(self):
        orders = {"ABCD": (0, 1, 2, 3), "CDAB": (2, 3, 0, 1), "BADC": (1, 0, 3, 2), "DCBA": (3, 2, 1, 0)}
        for order, perm in orders.items():
            reg = {"param": "x", "type": "FLOAT32", "order": order, "regs": 2, "scale": 1.0, "offset": 0.0}
            ws = sim_mod.encode_words(reg, 230.5)
            raw = bytes((ws[0] >> 8, ws[0] & 0xFF, ws[1] >> 8, ws[1] & 0xFF))
            abcd = bytes(raw[perm.index(i)] for i in range(4))
            self.assertEqual(230.5, struct.unpack(">f", abcd)[0], order)
        reg = {"param": "x", "type": "INT16", "order": "BA", "regs": 1, "scale": 0.001, "offset": 0.0}
        self.assertEqual([0x34FC], sim_mod.encode_words(reg, -0.972))
        reg = {"param": "x", "type": "BITFIELD16", "order": "AB", "regs": 1, "scale": 1.0,
               "offset": 0.0, "bit_mask": 0x00F0}
        self.assertEqual([0x0050], sim_mod.encode_words(reg, 5))
        reg = {"param": "x", "type": "UINT16", "order": "AB", "regs": 1, "scale": 0.1, "offset": -10.0}
        self.assertEqual([2400], sim_mod.encode_words(reg, 230.0))

    def test_layout_is_the_firmware_configuration(self):
        gen = (ROOT / "main" / "generated" / "ems_config_gen.h").read_text(encoding="utf-8")
        crc = re.search(r"EMS_CONFIG_GEN_CRC32 (0x[0-9A-F]{8})u", gen).group(1)
        self.assertEqual(crc, LAB["config_crc32"])
        self.assertEqual(["electrical", "energy", "status"], [g["name"] for g in LAB["devices"][0]["poll_groups"]])

    def test_control_file_commands(self):
        s, _, lines = make(LAB2)
        s.apply_control("# comment\nmode 1 TIMEOUT\nset 2 ac.frequency 49.5\nmode 3 NORMAL\nbogus\n")
        self.assertIsNone(s.handle(read_req(1, 0, 14)).data)
        self.assertEqual(f"{4950:04X}", words(s.handle(read_req(2, 0, 14)).data)[13])
        self.assertEqual(2, sum("CONTROL line" in ln for ln in lines))   # 'mode 3' and 'bogus'

    def test_crc_reference_value(self):
        self.assertEqual(0x4B37, crc16(b"123456789"))   # CRC-16/MODBUS check value


# ============================================================== serial loop ===
class FakeSerial:
    """In-memory serial port. Bytes become readable at their scheduled time."""

    def __init__(self, clock: Clock, char_s: float = 11 / 9600):
        self.clock, self.char_s = clock, char_s
        self.rx: list[tuple[float, int]] = []
        self.sent: list[tuple[float, bytes]] = []
        self.last_rx_time = 0.0

    def feed(self, data: bytes, at: float) -> None:
        for i, b in enumerate(data):
            self.rx.append((at + i * self.char_s, b))
        self.rx.sort()

    def read(self, n: int) -> bytes:
        if self.rx and self.rx[0][0] <= self.clock.t:
            t, b = self.rx.pop(0)
            self.last_rx_time = t
            return bytes((b,))
        self.clock.t += 0.001          # read timeout elapses
        return b""

    def write(self, data: bytes) -> int:
        self.sent.append((self.clock.t, bytes(data)))
        return len(data)

    def flush(self) -> None:
        pass


def run_serve(s: Simulator, port: FakeSerial, until: float, control_file=None) -> None:
    sim_mod.serve(s, port, stop=lambda: s.clock() >= until, control_file=control_file)


class SerialLoopTests(unittest.TestCase):

    def test_back_to_back_requests_answered_in_order(self):
        s, clk, _ = make(LAB2)
        port = FakeSerial(clk)
        port.feed(read_req(1, 0, 14), at=clk.t + 0.01)
        port.feed(read_req(2, 14, 4), at=clk.t + 0.10)
        run_serve(s, port, until=clk.t + 0.5)
        self.assertEqual([1, 2], [d[0] for _, d in port.sent])
        self.assertEqual(SPEC_4_1[14:18], words(port.sent[1][1]))

    def test_partial_frame_discarded_after_gap(self):
        s, clk, lines = make()
        port = FakeSerial(clk)
        port.feed(read_req(1, 0, 14)[:4], at=clk.t + 0.01)
        port.feed(read_req(1, 18, 1), at=clk.t + 0.10)    # gap > 50 ms but < 200 ms idle rule
        run_serve(s, port, until=clk.t + 0.6)
        self.assertEqual(1, len(port.sent))
        self.assertEqual(SPEC_4_1[18:19], words(port.sent[0][1]))
        self.assertTrue(any("DISCARD" in ln for ln in lines))

    def test_timeout_mode_sends_nothing(self):
        s, clk, _ = make(mode="TIMEOUT")
        port = FakeSerial(clk)
        port.feed(read_req(1, 0, 14), at=clk.t + 0.01)
        run_serve(s, port, until=clk.t + 0.5)
        self.assertEqual([], port.sent)

    def test_control_file_switches_mode_at_runtime(self):
        s, clk, _ = make()
        with tempfile.TemporaryDirectory() as d:
            ctl = Path(d) / "ctl.txt"
            ctl.write_text("mode * TIMEOUT\n", encoding="utf-8")
            port = FakeSerial(clk)
            port.feed(read_req(1, 18, 1), at=clk.t + 0.05)
            run_serve(s, port, until=clk.t + 0.3, control_file=ctl)
            self.assertEqual([], port.sent)


class LineFakeSerial(FakeSerial):
    """FakeSerial whose transmit buffer drains at line rate (out_waiting like pyserial)."""

    def __init__(self, clock: Clock, char_s: float = 11 / 9600):
        super().__init__(clock, char_s)
        self.tx_free_at = 0.0          # time the last queued byte has left the line
        self.idle_gaps: list[float] = []

    @property
    def out_waiting(self) -> int:
        return max(0, int(round((self.tx_free_at - self.clock.t) / self.char_s)))

    def write(self, data: bytes) -> int:
        t = self.clock.t
        if self.sent and t > self.tx_free_at:
            self.idle_gaps.append(t - self.tx_free_at)     # line was idle before this write
        self.tx_free_at = max(self.tx_free_at, t) + len(data) * self.char_s
        return super().write(data)


class BusFaultTests(unittest.TestCase):
    """D8: BABBLE bus fault for ESC-001..005."""

    def test_control_bus_commands(self):
        s, _, lines = make()
        s.apply_control("bus BABBLE\n")
        self.assertTrue(s.babble)
        self.assertTrue(any("BUS BABBLE on" in ln for ln in lines))
        s.apply_control("bus babble\n")                      # case-insensitive, no second log
        self.assertEqual(1, sum("BUS BABBLE on" in ln for ln in lines))
        s.apply_control("bus QUIET\n")
        self.assertFalse(s.babble)
        self.assertTrue(any("BUS QUIET" in ln for ln in lines))
        s.apply_control("bus NOISE\nbus\n")
        self.assertEqual(2, sum("CONTROL line" in ln for ln in lines))
        with self.assertRaises(ValueError):
            s.set_bus("LOUD")

    def test_babble_keeps_the_line_busy_without_gaps(self):
        s, clk, _ = make()
        s.set_bus("BABBLE")
        port = LineFakeSerial(clk)
        t0 = clk.t
        run_serve(s, port, until=t0 + 3.0)
        self.assertEqual([], port.idle_gaps)                  # never idle: recovery sees no silence
        self.assertTrue(port.sent and port.sent[0][0] - t0 < 0.002)
        self.assertTrue(all(set(d) == {sim_mod.BABBLE_BYTE} for _, d in port.sent))
        sent_bytes = sum(len(d) for _, d in port.sent)
        line_bytes = 3.0 / port.char_s
        self.assertGreaterEqual(sent_bytes, line_bytes)       # at least line rate
        self.assertLess(sent_bytes, line_bytes + sim_mod.BABBLE_LOW_WATER + 2 * sim_mod.BABBLE_CHUNK)
        self.assertEqual(sent_bytes, s.counters["babble_bytes"])

    def test_babble_ignores_requests_then_quiet_answers_again(self):
        s, clk, _ = make()
        s.set_bus("BABBLE")
        port = LineFakeSerial(clk)
        port.feed(read_req(1, 18, 1), at=clk.t + 0.05)
        run_serve(s, port, until=clk.t + 0.5)
        self.assertEqual(0, s.counters["rx"])
        self.assertEqual(8, s.counters["babble_rx_dropped"])
        s.set_bus("QUIET")
        n = len(port.sent)
        clk.t = port.tx_free_at + 0.01                         # backlog has drained
        port.feed(read_req(1, 18, 1), at=clk.t + 0.05)
        run_serve(s, port, until=clk.t + 0.5)
        self.assertEqual(n + 1, len(port.sent))               # one reply, no more babble
        self.assertEqual(SPEC_4_1[18:19], words(port.sent[-1][1]))

    def test_babble_via_control_file_at_runtime(self):
        s, clk, _ = make()
        with tempfile.TemporaryDirectory() as d:
            ctl = Path(d) / "ctl.txt"
            ctl.write_text("bus BABBLE\n", encoding="utf-8")
            port = LineFakeSerial(clk)
            run_serve(s, port, until=clk.t + 0.3, control_file=ctl)
            self.assertTrue(s.babble)
            self.assertTrue(port.sent)

    def test_babble_byte_never_starts_a_valid_response(self):
        b = sim_mod.BABBLE_BYTE
        self.assertTrue(b > 247 or b not in (3, 4, 0x83, 0x84))   # FC check fails on byte 2
        self.assertFalse(valid_crc(bytes((b,)) * 8))


# ====================================================================== CLI ===
class CliTests(unittest.TestCase):

    def run_main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = sim_mod.main(list(argv))
        return rc, out.getvalue(), err.getvalue()

    def test_dump(self):
        rc, out, _ = self.run_main("--site", str(SITE / "lab_bench.site.json"), "--dump")
        self.assertEqual(0, rc)
        self.assertIn(" ".join(SPEC_4_1[0:14]), out)
        self.assertIn("SIMULATION_ONLY", out)

    def test_self_test(self):
        rc, out, _ = self.run_main("--site", str(SITE / "lab_bench_2dev.site.json"), "--self-test")
        self.assertEqual(0, rc)
        self.assertIn("SELF-TEST PASS", out)

    def test_bad_arguments(self):
        self.assertEqual(2, self.run_main("--site", str(SITE / "lab_bench.site.json"), "--mode", "FOO", "--dump")[0])
        self.assertEqual(2, self.run_main("--site", str(SITE / "lab_bench.site.json"),
                                          "--set", "1:ac.current.l1=999", "--dump")[0])
        self.assertEqual(2, self.run_main("--site", str(SITE / "lab_bench.site.json"))[0])   # no --port

    def test_layout_file_from_export_sim(self):
        sys.path.insert(0, str(ROOT / "tools" / "profile"))
        import myems_profile_tool as tool
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "lab.sim.json"
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, tool.main(["export-sim", "--site", str(SITE / "lab_bench.site.json"),
                                               "--out", str(out)]))
            rc, text, _ = self.run_main("--layout", str(out), "--dump")
        self.assertEqual(0, rc)
        self.assertIn(" ".join(SPEC_4_1[0:14]), text)

    def test_nan_vector_parsing(self):
        self.assertTrue(math.isnan(sim_mod.parse_test_vector("D8 NaN -> INVALID")))
        self.assertEqual(math.inf, sim_mod.parse_test_vector("D9 +Inf -> INVALID"))
        self.assertEqual(-12500.0, sim_mod.parse_test_vector("D5 -12500"))
        self.assertIsNone(sim_mod.parse_test_vector(""))


if __name__ == "__main__":
    unittest.main()
