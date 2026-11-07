#!/usr/bin/env python3
"""MyEMS profile tool (DP-WP4 sub-package 4A, milestone D3).

Validates device profiles, register maps and site files and turns them into
const C tables for the firmware. Standard library only.

Commands (run from the project root):
  validate   --site FILE [--target sim|prod]
  generate   --site FILE --target sim|prod [--out-dir DIR] [--check]
  budget     --site FILE
  export-sim --site FILE [--out FILE]

Design reference: DP-WP4 Design Specification v1.1, Sections 5-9.
Rule IDs V-01 .. V-15 are those of Design Section 5.4.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import sys
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import umd_gen  # noqa: E402  (same folder)

TOOL_VERSION = "1.0.0"
PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG_DIR = PROJECT_ROOT / "config"
DEFAULT_OUT_DIR = PROJECT_ROOT / "main" / "generated"
DEFAULT_SIM_DIR = PROJECT_ROOT / "build_sim"

SCHEMAS = {"profile": "myems.profile", "regmap": "myems.regmap", "site": "myems.site"}
SUPPORTED_MAJOR, SUPPORTED_MINOR = 1, 1

# --- constants shared with the firmware ----------------------------------------
# Limits: components/ems_profile/include/ems_config_types.h and ems_state_store.h
LIMITS = {
    "buses": 2, "devices": 4, "maps": 4, "groups_per_map": 8,
    "poll_entries": 16, "mappings_per_map": 32, "mappings_total": 128,
}
# Timing constants of components/modbus_rtu_master/src/modbus_rtu_master.c
LATE_RESPONSE_GUARD_MS = 1000.0
RETRY_QUIET_MS = 100.0
ASSUMED_DEVICE_LATENCY_MS = 50.0    # Design 9.6
MAX_NOMINAL_BUS_LOAD = 0.30         # V-14
MAX_PROBE_BUS_LOAD = 0.50           # V-14

CLASSIFICATIONS = {"PRODUCTION": 0, "SIMULATION_ONLY": 1, "TEST_ONLY": 2}
RATIO_HANDLING = {"MYEMS_APPLIES": 0, "METER_REPORTS_PRIMARY": 1}
PARITY = {"NONE": 0, "EVEN": 1, "ODD": 2}
VALID_BAUD = (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200)

# Register types: name -> (C enumerator, value in ems_reg_type_t, registers)
REG_TYPES = {
    "UINT16": ("EMS_REG_UINT16", 0, 1), "INT16": ("EMS_REG_INT16", 1, 1),
    "UINT32": ("EMS_REG_UINT32", 2, 2), "INT32": ("EMS_REG_INT32", 3, 2),
    "FLOAT32": ("EMS_REG_FLOAT32", 4, 2), "BITFIELD16": ("EMS_REG_BITFIELD16", 5, 1),
}
# Word orders: name -> (C enumerator, value in ems_word_order_t, allowed register counts)
ORDERS = {
    "AB": ("EMS_ORDER_AB", 0, (1,)), "BA": ("EMS_ORDER_BA", 2, (1,)),
    "ABCD": ("EMS_ORDER_ABCD", 0, (2,)), "CDAB": ("EMS_ORDER_CDAB", 1, (2,)),
    "BADC": ("EMS_ORDER_BADC", 2, (2,)), "DCBA": ("EMS_ORDER_DCBA", 3, (2,)),
}
FLAGS = {"APPLY_CT": 0x01, "APPLY_PT": 0x02, "COUNTER": 0x04, "ROLLOVER": 0x08}
FLAG_MACROS = {"APPLY_CT": "EMS_MAP_F_APPLY_CT", "APPLY_PT": "EMS_MAP_F_APPLY_PT",
               "COUNTER": "EMS_MAP_F_COUNTER", "ROLLOVER": "EMS_MAP_F_ROLLOVER"}

# Capability -> dictionary parameters that implement it (rule V-11).
CAPABILITIES = {
    "phase_voltage": ("ac.voltage.l1_n", "ac.voltage.l2_n", "ac.voltage.l3_n"),
    "phase_current": ("ac.current.l1", "ac.current.l2", "ac.current.l3"),
    "active_power": ("ac.power.active.total",),
    "reactive_power": ("ac.power.reactive.total",),
    "apparent_power": ("ac.power.apparent.total",),
    "power_factor": ("ac.power.factor.total",),
    "frequency": ("ac.frequency",),
    "active_energy_import": ("ac.energy.import.active",),
    "active_energy_export": ("ac.energy.export.active",),
    "device_status": ("dev.status.code",),
    "power_quality": (),
    "demand_measurement": (),
}
ACTIVE_POWER_PARAM = "ac.power.active.total"

ID_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
GROUP_RE = re.compile(r"^[a-z][a-z0-9_]*$")
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
DOC_KEYS = {"$schema", "note", "description"}


# ============================================================== reporting ===
@dataclass
class Issue:
    rule: str
    file: str
    where: str
    message: str

    def __str__(self) -> str:
        loc = f"{self.file}: {self.where}" if self.where else self.file
        return f"ERROR [{self.rule}] {loc}: {self.message}"


class Report:
    def __init__(self, config_dir: Path):
        self.config_dir = config_dir
        self.errors: list[Issue] = []

    def rel(self, path: Path) -> str:
        try:
            return path.resolve().relative_to(self.config_dir.resolve()).as_posix()
        except ValueError:
            return path.as_posix()

    def error(self, rule: str, path: Path | str, where: str, message: str) -> None:
        f = self.rel(path) if isinstance(path, Path) else path
        self.errors.append(Issue(rule, f, where, message))

    def rules(self) -> set[str]:
        return {e.rule for e in self.errors}

    @property
    def ok(self) -> bool:
        return not self.errors


class ToolError(Exception):
    """Validation failed; the report holds the details."""


# ================================================================== model ===
@dataclass
class Mapping:
    mapping_id: int
    param: str
    runtime_id: int
    group_index: int
    group_name: str
    address: int
    type: str
    order: str
    scale: float
    offset: float
    unit: str
    min: float
    max: float
    flags: list[str]
    bit_mask: int
    test_vector: str = ""

    @property
    def regs(self) -> int:
        return REG_TYPES[self.type][2]

    @property
    def flag_bits(self) -> int:
        bits = 0
        for f in self.flags:
            bits |= FLAGS[f]
        return bits


@dataclass
class Group:
    name: str
    function_code: int
    start: int
    count: int
    interval_ms: int
    offset_ms: int
    stale_timeout_ms: int


@dataclass
class RegMap:
    path: Path
    data: dict
    map_id: str
    version: str
    classification: str
    groups: list[Group] = field(default_factory=list)
    mappings: list[Mapping] = field(default_factory=list)

    def group_index(self, name: str) -> int:
        for i, g in enumerate(self.groups):
            if g.name == name:
                return i
        return -1


@dataclass
class Profile:
    path: Path
    data: dict
    profile_id: str
    version: str
    classification: str
    device_type: str
    map_ref: tuple[str, str]
    ratio_handling: str
    defaults: dict
    capabilities: dict


@dataclass
class Bus:
    bus_id: str
    uart: int
    tx: int
    rx: int
    rts: int
    baud: int
    parity: str
    stop_bits: int


@dataclass
class Device:
    instance_id: str
    slot: int
    profile_id: str
    bus_id: str
    slave_id: int
    ct: float
    pt: float
    location: str
    enabled: bool
    timeout_ms: int = 0
    max_retries: int = 0
    comm_lost_threshold: int = 0
    probe_group: str = ""
    probe_interval_ms: int = 0


@dataclass
class Model:
    site_path: Path
    site: dict
    site_id: str
    target: str
    buses: list[Bus]
    devices: list[Device]
    profiles: list[Profile]          # in order of first use
    maps: list[RegMap]               # in order of first use
    umd: dict
    umd_params: dict

    def profile(self, pid: str) -> Profile:
        return next(p for p in self.profiles if p.profile_id == pid)

    def map_for(self, prof: Profile) -> RegMap:
        return next(m for m in self.maps if (m.map_id, m.version) == prof.map_ref)

    def bus_index(self, bus_id: str) -> int:
        return next(i for i, b in enumerate(self.buses) if b.bus_id == bus_id)

    @property
    def classification(self) -> str:
        worst = max([CLASSIFICATIONS[p.classification] for p in self.profiles] +
                    [CLASSIFICATIONS[m.classification] for m in self.maps] + [0])
        return next(k for k, v in CLASSIFICATIONS.items() if v == worst)


# ============================================================ JSON helpers ===
def _load_json(path: Path, rep: Report) -> dict | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        rep.error("V-01", path, "", "file not found")
        return None
    except json.JSONDecodeError as exc:
        rep.error("V-01", path, f"line {exc.lineno}", f"invalid JSON: {exc.msg}")
        return None
    if not isinstance(data, dict):
        rep.error("V-01", path, "", "top level must be a JSON object")
        return None
    return data


def _check_schema(data: dict, kind: str, path: Path, rep: Report) -> bool:
    val = data.get("schema")
    m = re.match(r"^([a-z.]+)/(\d+)\.(\d+)$", str(val))
    if not m or m.group(1) != SCHEMAS[kind]:
        rep.error("V-01", path, "schema", f"expected '{SCHEMAS[kind]}/{SUPPORTED_MAJOR}.{SUPPORTED_MINOR}', got {val!r}")
        return False
    major, minor = int(m.group(2)), int(m.group(3))
    if major != SUPPORTED_MAJOR or minor > SUPPORTED_MINOR:
        rep.error("V-01", path, "schema",
                  f"unsupported schema version {major}.{minor} (this tool supports {SUPPORTED_MAJOR}.0-{SUPPORTED_MAJOR}.{SUPPORTED_MINOR})")
        return False
    return True


def _require(obj: dict, spec: dict[str, Any], path: Path, where: str, rep: Report,
             extra_ok: set[str] = frozenset()) -> bool:
    """spec: key -> type or tuple of types. Reports missing/wrong/unknown keys (V-01)."""
    good = True
    for key, typ in spec.items():
        label = f"{where}.{key}" if where else key
        if key not in obj:
            rep.error("V-01", path, label, "required field missing")
            good = False
            continue
        val = obj[key]
        types = typ if isinstance(typ, tuple) else (typ,)
        # bool is an int in Python: do not accept it where a number is expected
        if isinstance(val, bool) and bool not in types:
            ok = False
        else:
            ok = isinstance(val, types)
        if not ok:
            names = "/".join(t.__name__ for t in types)
            rep.error("V-01", path, label, f"must be {names}, got {type(val).__name__}")
            good = False
    for key in obj:
        if key not in spec and key not in extra_ok and key not in DOC_KEYS:
            label = f"{where}.{key}" if where else key
            rep.error("V-01", path, label, "unknown field")
            good = False
    return good


NUM = (int, float)


def _finite(x: Any) -> bool:
    return isinstance(x, NUM) and not isinstance(x, bool) and math.isfinite(float(x))


# =========================================================== file loading ===
def _index_files(config_dir: Path, sub: str, suffix: str, kind: str, id_key: str,
                 rep: Report) -> dict[str, list[tuple[Path, dict]]]:
    index: dict[str, list[tuple[Path, dict]]] = {}
    for path in sorted((config_dir / sub).glob(f"*{suffix}")):
        data = _load_json(path, rep)
        if data is None:
            continue
        key = data.get(id_key)
        if isinstance(key, str):
            index.setdefault(key, []).append((path, data))
        else:
            rep.error("V-01", path, id_key, "required field missing")
    return index


def _parse_map(path: Path, data: dict, umd_params: dict, rep: Report) -> RegMap | None:
    if not _check_schema(data, "regmap", path, rep):
        return None
    if not _require(data, {"schema": str, "map_id": str, "map_version": str, "classification": str,
                           "poll_groups": list, "mappings": list}, path, "", rep):
        return None
    if not ID_RE.match(data["map_id"]):
        rep.error("V-01", path, "map_id", "must be upper case letters, digits and '_'")
    if not SEMVER_RE.match(data["map_version"]):
        rep.error("V-01", path, "map_version", "must be MAJOR.MINOR.PATCH")
    if data["classification"] not in CLASSIFICATIONS:
        rep.error("V-01", path, "classification", f"must be one of {sorted(CLASSIFICATIONS)}")
        return None
    rm = RegMap(path, data, data["map_id"], data["map_version"], data["classification"])

    # ---- poll groups
    names: set[str] = set()
    for i, g in enumerate(data["poll_groups"]):
        w = f"poll_groups[{i}]"
        if not isinstance(g, dict) or not _require(
                g, {"name": str, "function_code": int, "start": int, "count": int,
                    "interval_ms": int, "offset_ms": int, "stale_timeout_ms": int}, path, w, rep):
            continue
        if not GROUP_RE.match(g["name"]):
            rep.error("V-01", path, f"{w}.name", "must be lower case letters, digits and '_'")
        if g["name"] in names:
            rep.error("V-02", path, f"{w}.name", f"duplicate group name '{g['name']}'")
        names.add(g["name"])
        if g["function_code"] not in (3, 4):
            rep.error("V-07", path, f"{w}.function_code",
                      f"{g['function_code']} not allowed; only FC03/FC04 (read-only, DR-21)")
        if not 1 <= g["count"] <= 125:
            rep.error("V-07", path, f"{w}.count", f"{g['count']} outside 1..125")
        if g["start"] < 0 or g["start"] + g["count"] > 65536:
            rep.error("V-07", path, f"{w}.start", "block must lie within addresses 0..65535")
        if g["interval_ms"] < 1000:
            rep.error("V-09", path, f"{w}.interval_ms", f"{g['interval_ms']} ms < 1000 ms")
        if g["stale_timeout_ms"] <= g["interval_ms"]:
            rep.error("V-09", path, f"{w}.stale_timeout_ms",
                      f"{g['stale_timeout_ms']} ms must be greater than interval {g['interval_ms']} ms")
        if g["offset_ms"] < 0:
            rep.error("V-09", path, f"{w}.offset_ms", "must be >= 0")
        rm.groups.append(Group(g["name"], g["function_code"], g["start"], g["count"],
                               g["interval_ms"], g["offset_ms"], g["stale_timeout_ms"]))
    if len(rm.groups) > LIMITS["groups_per_map"]:
        rep.error("V-15", path, "poll_groups", f"{len(rm.groups)} groups > limit {LIMITS['groups_per_map']}")

    # ---- mappings
    ids: set[int] = set()
    params: set[str] = set()
    for i, m in enumerate(data["mappings"]):
        w = f"mappings[{i}]"
        if not isinstance(m, dict) or not _require(
                m, {"mapping_id": int, "param": str, "group": str, "address": int, "type": str,
                    "order": str, "scale": NUM, "offset": NUM, "unit": str, "min": NUM, "max": NUM,
                    "flags": list}, path, w, rep, extra_ok={"bit_mask", "test_vector"}):
            continue
        tag = f"{w} ({m['param']})"
        # V-02 uniqueness
        if m["mapping_id"] in ids:
            rep.error("V-02", path, f"{tag}.mapping_id", f"duplicate mapping_id {m['mapping_id']}")
        ids.add(m["mapping_id"])
        if not 1 <= m["mapping_id"] <= 65535:
            rep.error("V-01", path, f"{tag}.mapping_id", "must be 1..65535")
        if m["param"] in params:
            rep.error("V-02", path, f"{tag}.param", "parameter mapped twice in one map")
        params.add(m["param"])
        # V-04 dictionary
        info = umd_params.get(m["param"])
        if info is None:
            rep.error("V-04", path, f"{tag}.param", f"'{m['param']}' is not in the measurement dictionary")
        elif m["unit"] != info.unit:
            rep.error("V-04", path, f"{tag}.unit",
                      f"'{m['unit']}' is not the canonical unit '{info.unit}' of {m['param']}")
        flags = m["flags"]
        bad_flags = [f for f in flags if f not in FLAGS]
        if bad_flags or len(set(flags)) != len(flags):
            rep.error("V-01", path, f"{tag}.flags", f"unknown or repeated flags {flags}")
            flags = [f for f in dict.fromkeys(flags) if f in FLAGS]
        if info is not None and (info.value_type == "counter") != ("COUNTER" in flags):
            rep.error("V-04", path, f"{tag}.flags",
                      f"COUNTER flag must be set exactly for counter parameters ({m['param']} is '{info.value_type}')")
        # V-05 type / order / flag compatibility
        typ, order = m["type"], m["order"]
        if typ not in REG_TYPES:
            rep.error("V-05", path, f"{tag}.type", f"unknown type '{typ}'")
            continue
        nregs = REG_TYPES[typ][2]
        if order not in ORDERS or nregs not in ORDERS[order][2]:
            allowed = [k for k, v in ORDERS.items() if nregs in v[2]]
            rep.error("V-05", path, f"{tag}.order", f"'{order}' not allowed for {typ}; use one of {allowed}")
        mask = m.get("bit_mask", 0)
        if "bit_mask" in m and (typ != "BITFIELD16" or not isinstance(mask, int) or not 1 <= mask <= 0xFFFF):
            rep.error("V-05", path, f"{tag}.bit_mask", "bit_mask (1..65535) only for BITFIELD16")
        if "ROLLOVER" in flags and ("COUNTER" not in flags or typ != "UINT32"):
            rep.error("V-05", path, f"{tag}.flags", "ROLLOVER only for UINT32 counters")
        if "COUNTER" in flags and typ not in ("UINT16", "UINT32"):
            rep.error("V-05", path, f"{tag}.flags", "counters must be UINT16 or UINT32")
        # V-06 group membership
        gi = rm.group_index(m["group"])
        if gi < 0:
            rep.error("V-06", path, f"{tag}.group", f"undefined poll group '{m['group']}'")
        else:
            g = rm.groups[gi]
            if m["address"] < g.start or m["address"] + nregs > g.start + g.count:
                rep.error("V-06", path, f"{tag}.address",
                          f"{typ} at {m['address']} needs registers {m['address']}..{m['address'] + nregs - 1}, "
                          f"outside group '{g.name}' block {g.start}..{g.start + g.count - 1}")
        # V-08 numbers
        if not _finite(m["scale"]) or float(m["scale"]) == 0.0:
            rep.error("V-08", path, f"{tag}.scale", "must be finite and non-zero")
        if not _finite(m["offset"]):
            rep.error("V-08", path, f"{tag}.offset", "must be finite")
        if not (_finite(m["min"]) and _finite(m["max"])) or not float(m["min"]) < float(m["max"]):
            rep.error("V-08", path, f"{tag}.min", "min and max must be finite with min < max")
        rm.mappings.append(Mapping(
            mapping_id=m["mapping_id"], param=m["param"],
            runtime_id=info.runtime_id if info else 0, group_index=max(gi, 0), group_name=m["group"],
            address=m["address"], type=typ, order=order if order in ORDERS else "AB",
            scale=float(m["scale"]), offset=float(m["offset"]), unit=m["unit"],
            min=float(m["min"]), max=float(m["max"]), flags=flags,
            bit_mask=mask if isinstance(mask, int) else 0, test_vector=str(m.get("test_vector", ""))))
    if len(rm.mappings) > LIMITS["mappings_per_map"]:
        rep.error("V-15", path, "mappings", f"{len(rm.mappings)} mappings > limit {LIMITS['mappings_per_map']}")
    return rm


def _parse_profile(path: Path, data: dict, rep: Report) -> Profile | None:
    if not _check_schema(data, "profile", path, rep):
        return None
    if not _require(data, {"schema": str, "profile_id": str, "profile_version": str, "classification": str,
                           "device_type": str, "vendor": dict, "protocol": str, "register_map_ref": dict,
                           "comm_defaults": dict, "capabilities": dict, "conventions": dict}, path, "", rep):
        return None
    ok = True
    if not ID_RE.match(data["profile_id"]):
        rep.error("V-01", path, "profile_id", "must be upper case letters, digits and '_'")
    if not SEMVER_RE.match(data["profile_version"]):
        rep.error("V-01", path, "profile_version", "must be MAJOR.MINOR.PATCH")
    if data["classification"] not in CLASSIFICATIONS:
        rep.error("V-01", path, "classification", f"must be one of {sorted(CLASSIFICATIONS)}")
        ok = False
    if data["protocol"] != "MODBUS_RTU":
        rep.error("V-01", path, "protocol", "only MODBUS_RTU is supported in DP-WP4")
    ref = data["register_map_ref"]
    ok &= _require(ref, {"map_id": str, "map_version": str}, path, "register_map_ref", rep)
    cd = data["comm_defaults"]
    ok &= _require(cd, {"timeout_ms": int, "max_retries": int, "comm_lost_threshold": int,
                        "probe_group": str, "probe_interval_ms": int}, path, "comm_defaults", rep)
    conv = data["conventions"]
    ok &= _require(conv, {"power_sign": str, "pf_sign": str, "ratio_handling": str}, path, "conventions", rep)
    if ok and conv["ratio_handling"] not in RATIO_HANDLING:
        rep.error("V-01", path, "conventions.ratio_handling", f"must be one of {sorted(RATIO_HANDLING)}")
        ok = False
    caps = data["capabilities"]
    for k, v in caps.items():
        if k not in CAPABILITIES:
            rep.error("V-11", path, f"capabilities.{k}", "unknown capability")
        elif not isinstance(v, bool):
            rep.error("V-01", path, f"capabilities.{k}", "must be true or false")
    if not ok:
        return None
    return Profile(path, data, data["profile_id"], data["profile_version"], data["classification"],
                   data["device_type"], (ref["map_id"], ref["map_version"]), conv["ratio_handling"],
                   dict(cd), dict(caps))


def _check_capabilities(prof: Profile, rm: RegMap, rep: Report) -> None:
    mapped = {m.param for m in rm.mappings}
    for cap, val in prof.capabilities.items():
        if val is True and cap in CAPABILITIES and not mapped.intersection(CAPABILITIES[cap]):
            rep.error("V-11", prof.path, f"capabilities.{cap}",
                      f"capability is true but map {rm.map_id} has no mapping for it")
    true_params = {p for cap, val in prof.capabilities.items() if val is True for p in CAPABILITIES.get(cap, ())}
    for m in rm.mappings:
        if m.param not in true_params:
            rep.error("V-11", rm.path, f"mappings ({m.param})",
                      f"parameter is mapped but no capability of profile {prof.profile_id} that covers it is true")


# ============================================================== validation ===
def build_model(site_path: Path, target: str, config_dir: Path = DEFAULT_CONFIG_DIR,
                umd_path: Path | None = None) -> tuple[Model | None, Report]:
    rep = Report(config_dir)
    site_path = Path(site_path)
    if target not in ("sim", "prod"):
        raise ValueError("target must be 'sim' or 'prod'")

    try:
        umd_data, umd_list = umd_gen.load_umd(umd_path or (config_dir / "umd" / "umd_v1.json"))
    except umd_gen.UmdError as exc:
        rep.error("V-04", "umd/umd_v1.json", "", str(exc))
        return None, rep
    umd_params = {p.name: p for p in umd_list}

    site = _load_json(site_path, rep)
    if site is None:
        return None, rep
    if not _check_schema(site, "site", site_path, rep):
        return None, rep
    if not _require(site, {"schema": str, "site_id": str, "buses": list, "devices": list}, site_path, "", rep):
        return None, rep
    if not ID_RE.match(site["site_id"]):
        rep.error("V-01", site_path, "site_id", "must be upper case letters, digits and '_'")

    # ---- buses
    buses: list[Bus] = []
    for i, b in enumerate(site["buses"]):
        w = f"buses[{i}]"
        if not isinstance(b, dict) or not _require(
                b, {"bus_id": str, "uart": int, "tx_gpio": int, "rx_gpio": int, "rts_gpio": int,
                    "baud": int, "parity": str, "stop_bits": int}, site_path, w, rep):
            continue
        if any(x.bus_id == b["bus_id"] for x in buses):
            rep.error("V-02", site_path, f"{w}.bus_id", f"duplicate bus_id '{b['bus_id']}'")
        if b["baud"] not in VALID_BAUD:
            rep.error("V-10", site_path, f"{w}.baud", f"{b['baud']} not in {VALID_BAUD}")
        if b["parity"] not in PARITY:
            rep.error("V-10", site_path, f"{w}.parity", f"must be one of {sorted(PARITY)}")
        if b["stop_bits"] not in (1, 2):
            rep.error("V-10", site_path, f"{w}.stop_bits", "must be 1 or 2")
        for k in ("uart", "tx_gpio", "rx_gpio", "rts_gpio"):
            if not 0 <= b[k] <= 255:
                rep.error("V-10", site_path, f"{w}.{k}", "must be 0..255")
        buses.append(Bus(b["bus_id"], b["uart"], b["tx_gpio"], b["rx_gpio"], b["rts_gpio"],
                         b["baud"], b["parity"] if b["parity"] in PARITY else "NONE", b["stop_bits"]))
    if len(buses) > LIMITS["buses"]:
        rep.error("V-15", site_path, "buses", f"{len(buses)} buses > limit {LIMITS['buses']}")

    # ---- profiles and maps available in the config folder
    prof_index = _index_files(config_dir, "profiles", ".profile.json", "profile", "profile_id", rep)
    map_index = _index_files(config_dir, "maps", ".map.json", "regmap", "map_id", rep)
    for pid, lst in prof_index.items():
        if len(lst) > 1:
            rep.error("V-02", lst[1][0], "profile_id",
                      f"profile_id '{pid}' also defined in {rep.rel(lst[0][0])}")
    for mid, lst in map_index.items():
        if len(lst) > 1 and len({d.get('map_version') for _, d in lst}) != len(lst):
            rep.error("V-02", lst[1][0], "map_id", f"map '{mid}' with the same version defined twice")

    profiles: list[Profile] = []
    maps: list[RegMap] = []
    devices: list[Device] = []
    slots: set[int] = set()
    slaves: dict[tuple[str, int], str] = {}

    for i, d in enumerate(site["devices"]):
        w = f"devices[{i}]"
        if not isinstance(d, dict) or not _require(
                d, {"device_instance_id": str, "slot": int, "profile_id": str, "bus_id": str, "slave_id": int,
                    "ct_ratio": NUM, "pt_ratio": NUM, "location": str, "enabled": bool},
                site_path, w, rep, extra_ok={"comm_overrides"}):
            continue
        tag = f"{w} ({d['device_instance_id']})"
        if any(x.instance_id == d["device_instance_id"] for x in devices):
            rep.error("V-02", site_path, f"{tag}.device_instance_id", "duplicate device_instance_id")
        if d["slot"] in slots:
            rep.error("V-02", site_path, f"{tag}.slot", f"duplicate slot {d['slot']}")
        elif d["slot"] != i:
            rep.error("V-02", site_path, f"{tag}.slot", f"slots must be 0..n-1 in file order (expected {i})")
        slots.add(d["slot"])
        if not any(b.bus_id == d["bus_id"] for b in buses):
            rep.error("V-03", site_path, f"{tag}.bus_id", f"unknown bus '{d['bus_id']}'")
        if not 1 <= d["slave_id"] <= 247:
            rep.error("V-10", site_path, f"{tag}.slave_id", f"{d['slave_id']} outside 1..247")
        key = (d["bus_id"], d["slave_id"])
        if key in slaves:
            rep.error("V-10", site_path, f"{tag}.slave_id",
                      f"slave {d['slave_id']} already used by {slaves[key]} on {d['bus_id']}")
        slaves[key] = d["device_instance_id"]
        for k in ("ct_ratio", "pt_ratio"):
            if not _finite(d[k]) or float(d[k]) <= 0.0:
                rep.error("V-10", site_path, f"{tag}.{k}", "must be finite and > 0")

        # profile
        cand = prof_index.get(d["profile_id"])
        if not cand:
            rep.error("V-03", site_path, f"{tag}.profile_id", f"no profile '{d['profile_id']}' in {rep.rel(config_dir / 'profiles')}")
            continue
        prof = next((p for p in profiles if p.profile_id == d["profile_id"]), None)
        if prof is None:
            prof = _parse_profile(cand[0][0], cand[0][1], rep)
            if prof is None:
                continue
            profiles.append(prof)
            # map
            mcand = [(p, dd) for p, dd in map_index.get(prof.map_ref[0], []) if dd.get("map_version") == prof.map_ref[1]]
            if not mcand:
                have = sorted({dd.get('map_version') for _, dd in map_index.get(prof.map_ref[0], [])})
                rep.error("V-03", prof.path, "register_map_ref",
                          f"map {prof.map_ref[0]} version {prof.map_ref[1]} not found"
                          + (f" (available: {', '.join(have)})" if have else ""))
            elif not any((m.map_id, m.version) == prof.map_ref for m in maps):
                rm = _parse_map(mcand[0][0], mcand[0][1], umd_params, rep)
                if rm is not None:
                    maps.append(rm)
            rm = next((m for m in maps if (m.map_id, m.version) == prof.map_ref), None)
            if rm is not None:
                _check_capabilities(prof, rm, rep)

        rm = next((m for m in maps if (m.map_id, m.version) == prof.map_ref), None)
        dev = Device(d["device_instance_id"], d["slot"], d["profile_id"], d["bus_id"], d["slave_id"],
                     float(d["ct_ratio"]), float(d["pt_ratio"]), d["location"], d["enabled"])

        # resolved communication values: profile defaults + site overrides
        comm = dict(prof.defaults)
        ov = d.get("comm_overrides", {})
        if not isinstance(ov, dict):
            rep.error("V-01", site_path, f"{tag}.comm_overrides", "must be an object")
            ov = {}
        for k, v in ov.items():
            if k not in ("timeout_ms", "max_retries", "comm_lost_threshold", "probe_interval_ms") or \
                    not isinstance(v, int) or isinstance(v, bool):
                rep.error("V-01", site_path, f"{tag}.comm_overrides.{k}", "unknown field or not an integer")
            else:
                comm[k] = v
        dev.timeout_ms, dev.max_retries = comm["timeout_ms"], comm["max_retries"]
        dev.comm_lost_threshold, dev.probe_group = comm["comm_lost_threshold"], comm["probe_group"]
        dev.probe_interval_ms = comm["probe_interval_ms"]
        src = prof.path if not ov else site_path
        if not 50 <= dev.timeout_ms <= 5000:
            rep.error("V-10", src, f"{tag} timeout_ms", f"{dev.timeout_ms} outside 50..5000 ms")
        if not 0 <= dev.max_retries <= 5:
            rep.error("V-10", src, f"{tag} max_retries", f"{dev.max_retries} outside 0..5")
        if not 1 <= dev.comm_lost_threshold <= 10:
            rep.error("V-10", src, f"{tag} comm_lost_threshold", f"{dev.comm_lost_threshold} outside 1..10")
        if dev.probe_interval_ms < 1000:
            rep.error("V-10", src, f"{tag} probe_interval_ms", "must be >= 1000 ms")
        if rm is not None and rm.group_index(dev.probe_group) < 0:
            rep.error("V-10", prof.path, "comm_defaults.probe_group",
                      f"probe group '{dev.probe_group}' is not a poll group of map {rm.map_id}")
        # V-12 ratio rule
        if prof.ratio_handling == "METER_REPORTS_PRIMARY" and (dev.ct != 1.0 or dev.pt != 1.0):
            rep.error("V-12", site_path, f"{tag}.ct_ratio",
                      f"profile {prof.profile_id} uses METER_REPORTS_PRIMARY: CT and PT must be 1.0 "
                      f"(got CT {dev.ct:g}, PT {dev.pt:g}) to avoid applying ratios twice")
        devices.append(dev)

    if not devices and not rep.errors:
        rep.error("V-01", site_path, "devices", "at least one device is required")
    if len(devices) > LIMITS["devices"]:
        rep.error("V-15", site_path, "devices", f"{len(devices)} devices > limit {LIMITS['devices']}")
    if len(maps) > LIMITS["maps"]:
        rep.error("V-15", site_path, "devices", f"{len(maps)} different maps > limit {LIMITS['maps']}")
    if sum(len(m.mappings) for m in maps) > LIMITS["mappings_total"]:
        rep.error("V-15", site_path, "devices", f"more than {LIMITS['mappings_total']} mappings in total")

    model = Model(site_path, site, site.get("site_id", ""), target, buses, devices, profiles, maps,
                  umd_data, umd_params)

    if rep.ok:
        entries = sum(len(model.map_for(model.profile(d.profile_id)).groups) for d in devices if d.enabled)
        if entries > LIMITS["poll_entries"]:
            rep.error("V-15", site_path, "devices", f"{entries} poll entries > limit {LIMITS['poll_entries']}")
        # V-13 classification
        if target == "prod":
            for p in profiles:
                if p.classification != "PRODUCTION":
                    rep.error("V-13", p.path, "classification",
                              f"{p.classification} profile cannot be used with --target prod")
            for m in maps:
                if m.classification != "PRODUCTION":
                    rep.error("V-13", m.path, "classification",
                              f"{m.classification} map cannot be used with --target prod")
        # V-14 timing budget
        for line in budget(model).violations:
            rep.error("V-14", site_path, line[0], line[1])
    return (model if rep.ok else None), rep


# ================================================================== budget ===
@dataclass
class BudgetResult:
    rows: list[dict]
    devices: list[dict]
    buses: list[dict]
    violations: list[tuple[str, str]]


def _char_ms(bus: Bus) -> float:
    bits = 1 + 8 + (0 if bus.parity == "NONE" else 1) + bus.stop_bits
    return bits * 1000.0 / bus.baud


def budget(model: Model) -> BudgetResult:
    rows, devs, buses, viol = [], [], [], []
    for b in model.buses:
        ch = _char_ms(b)
        nominal_load = 0.0
        probe_load = 0.0
        for d in model.devices:
            if d.bus_id != b.bus_id or not d.enabled:
                continue
            rm = model.map_for(model.profile(d.profile_id))
            t_req = 8 * ch
            for g in rm.groups:
                resp = 5 + 2 * g.count
                t_frame = (8 + resp) * ch
                nominal = t_frame + ASSUMED_DEVICE_LATENCY_MS + 2 * 3.5 * ch
                load = nominal / g.interval_ms
                nominal_load += load
                rows.append({"bus": b.bus_id, "device": d.instance_id, "group": g.name, "regs": g.count,
                             "t_frame_ms": t_frame, "nominal_ms": nominal, "interval_ms": g.interval_ms,
                             "load": load})
            r = d.max_retries
            t_wc = (r + 1) * (t_req + d.timeout_ms + LATE_RESPONSE_GUARD_MS) + r * RETRY_QUIET_MS
            min_iv = min(g.interval_ms for g in rm.groups)
            probe = t_wc / d.probe_interval_ms
            probe_load += probe
            devs.append({"bus": b.bus_id, "device": d.instance_id, "timeout_ms": d.timeout_ms, "retries": r,
                         "t_wc_ms": t_wc, "min_interval_ms": min_iv, "probe_load": probe})
            if t_wc >= min_iv:
                viol.append((f"devices ({d.instance_id})",
                             f"worst-case failed transaction {t_wc / 1000:.2f} s (timeout {d.timeout_ms} ms, "
                             f"retries {r}) is not below the smallest poll interval {min_iv / 1000:.1f} s"))
        buses.append({"bus": b.bus_id, "nominal_load": nominal_load, "probe_load": probe_load})
        if nominal_load > MAX_NOMINAL_BUS_LOAD:
            viol.append((f"buses ({b.bus_id})", f"nominal bus load {nominal_load:.1%} > {MAX_NOMINAL_BUS_LOAD:.0%}"))
        if probe_load > MAX_PROBE_BUS_LOAD:
            viol.append((f"buses ({b.bus_id})",
                         f"bus load with all devices offline (probing) {probe_load:.1%} > {MAX_PROBE_BUS_LOAD:.0%}"))
    return BudgetResult(rows, devs, buses, viol)


def budget_text(model: Model) -> str:
    res = budget(model)
    out = [f"Timing budget for site {model.site_id} (Design 9.6)", ""]
    out.append(f"{'bus':<12} {'device':<20} {'group':<11} {'regs':>4} {'frame ms':>9} {'nominal ms':>11} {'interval':>9} {'load':>7}")
    for r in res.rows:
        out.append(f"{r['bus']:<12} {r['device']:<20} {r['group']:<11} {r['regs']:>4} {r['t_frame_ms']:>9.1f} "
                   f"{r['nominal_ms']:>11.1f} {r['interval_ms']:>8}ms {r['load']:>7.2%}")
    out += ["", f"{'device':<20} {'timeout':>8} {'retries':>7} {'T_wc':>8} {'min interval':>13} {'probe load':>11}"]
    for d in res.devices:
        out.append(f"{d['device']:<20} {d['timeout_ms']:>6}ms {d['retries']:>7} {d['t_wc_ms'] / 1000:>7.2f}s "
                   f"{d['min_interval_ms'] / 1000:>12.1f}s {d['probe_load']:>11.1%}")
    out.append("")
    for b in res.buses:
        out.append(f"bus {b['bus']}: nominal load {b['nominal_load']:.1%} (limit {MAX_NOMINAL_BUS_LOAD:.0%}), "
                   f"all devices offline {b['probe_load']:.1%} (limit {MAX_PROBE_BUS_LOAD:.0%})")
    out.append(f"assumptions: device latency {ASSUMED_DEVICE_LATENCY_MS:.0f} ms, late-response guard "
               f"{LATE_RESPONSE_GUARD_MS:.0f} ms, retry quiet time {RETRY_QUIET_MS:.0f} ms")
    out.append("RESULT: " + ("PASS" if not res.violations else "FAIL"))
    for _, msg in res.violations:
        out.append(f"  - {msg}")
    return "\n".join(out) + "\n"


# ========================================================= canonical CRC ===
def canonical_bytes(model: Model) -> bytes:
    """Must match ems_config_crc32() in components/ems_profile/src/ems_config_crc.c."""
    out = bytearray()
    u8 = lambda v: out.extend(struct.pack("<B", v))            # noqa: E731
    u16 = lambda v: out.extend(struct.pack("<H", v))           # noqa: E731
    u32 = lambda v: out.extend(struct.pack("<I", v))           # noqa: E731
    f64 = lambda v: out.extend(struct.pack("<d", v))           # noqa: E731
    u8(len(model.buses)); u8(len(model.maps)); u8(len(model.profiles)); u8(len(model.devices))
    u8(1 if model.target == "sim" else 0); u8(CLASSIFICATIONS[model.classification])
    for b in model.buses:
        u32(b.baud); u8(b.uart); u8(b.tx); u8(b.rx); u8(b.rts); u8(PARITY[b.parity]); u8(b.stop_bits)
    for m in model.maps:
        u8(len(m.groups)); u16(len(m.mappings)); u8(CLASSIFICATIONS[m.classification])
        for g in m.groups:
            u32(g.interval_ms); u32(g.offset_ms); u32(g.stale_timeout_ms); u16(g.start); u16(g.count); u8(g.function_code)
        for mp in m.mappings:
            f64(mp.scale); f64(mp.offset); f64(mp.min); f64(mp.max)
            u16(mp.mapping_id); u16(mp.runtime_id); u16(mp.address); u16(mp.bit_mask)
            u8(mp.group_index); u8(REG_TYPES[mp.type][1]); u8(ORDERS[mp.order][1]); u8(mp.flag_bits)
    for p in model.profiles:
        f64(counter_max_rate(model, p)); u8(model.maps.index(model.map_for(p)))
        u8(CLASSIFICATIONS[p.classification]); u8(RATIO_HANDLING[p.ratio_handling])
    for d in model.devices:
        p = model.profile(d.profile_id)
        f64(d.ct); f64(d.pt); u32(d.probe_interval_ms); u16(d.timeout_ms); u8(d.slot)
        u8(model.profiles.index(p)); u8(model.bus_index(d.bus_id)); u8(d.slave_id); u8(d.max_retries)
        u8(d.comm_lost_threshold); u8(model.map_for(p).group_index(d.probe_group)); u8(1 if d.enabled else 0)
    return bytes(out)


def config_crc32(model: Model) -> int:
    return zlib.crc32(canonical_bytes(model)) & 0xFFFFFFFF


def counter_max_rate(model: Model, prof: Profile) -> float:
    """Counter plausibility limit per hour: largest |active power| allowed by the map (Design 8.5)."""
    rm = model.map_for(prof)
    for m in rm.mappings:
        if m.param == ACTIVE_POWER_PARAM:
            return max(abs(m.min), abs(m.max))
    return 0.0


def _canon(obj: Any) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def inputs_hash(model: Model) -> str:
    h = hashlib.sha256()
    h.update(_canon(model.site))
    for p in model.profiles:
        h.update(_canon(p.data))
    for m in model.maps:
        h.update(_canon(m.data))
    h.update(_canon(model.umd))
    return h.hexdigest()[:16]


def inputs_summary(model: Model) -> str:
    parts = [f"site:{model.site_id}"]
    parts += [f"profile:{p.profile_id}@{p.version}" for p in model.profiles]
    parts += [f"map:{m.map_id}@{m.version}" for m in model.maps]
    parts.append(f"umd:{model.umd['umd_version']}")
    return "; ".join(parts)


# ============================================================ C rendering ===
def _c_double(x: float) -> str:
    r = repr(float(x))
    return r if any(c in r for c in ".eEn") else r + ".0"


def _c_str(s: str) -> str:
    if not s.isascii():
        raise ValueError(f"non-ASCII text cannot be emitted: {s!r}")
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _c_flags(flags: list[str]) -> str:
    return " | ".join(FLAG_MACROS[f] for f in flags) if flags else "0u"


def render(model: Model) -> tuple[str, str]:
    crc = config_crc32(model)
    ih = inputs_hash(model)
    head = [
        "/*",
        " * GENERATED FILE - DO NOT EDIT.",
        f" * Generator: tools/profile/myems_profile_tool.py {TOOL_VERSION}",
        f" * Site:      {model.site_path.name} ({model.site_id}), target {model.target}",
        f" * Inputs:    {inputs_summary(model)}",
        f" * Inputs sha256 (first 16): {ih}",
        f" * Config CRC32: 0x{crc:08X}   Classification: {model.classification}",
        f" * Regenerate: python tools/profile/myems_profile_tool.py generate --site config/site/{model.site_path.name} --target {model.target}",
        " */",
    ]
    if model.classification != "PRODUCTION":
        head.insert(-1, " * SIMULATION/TEST CONFIGURATION: synthetic register addresses, never use with real equipment.")

    h = head + [
        "#ifndef EMS_CONFIG_GEN_H",
        "#define EMS_CONFIG_GEN_H",
        "",
        '#include "ems_config_types.h"',
        "",
        f"#define EMS_CONFIG_GEN_CRC32 0x{crc:08X}u",
        f'#define EMS_CONFIG_GEN_SITE_ID "{model.site_id}"',
        f"#define EMS_CONFIG_GEN_DEVICE_COUNT {len(model.devices)}u",
        "",
        "extern const ems_config_build_t ems_config_build;",
        "",
        "#endif /* EMS_CONFIG_GEN_H */",
        "",
    ]

    c = head + ['#include "ems_config_gen.h"', '#include "ems_param_ids.h"', ""]
    for mi, m in enumerate(model.maps):
        c.append(f"/* ---- map {mi}: {m.map_id} {m.version} ({m.classification}) ---- */")
        c.append(f"static const ems_poll_group_t s_map{mi}_groups[{len(m.groups)}] = {{")
        for gi, g in enumerate(m.groups):
            c.append(f"    [{gi}] = {{ .interval_ms = {g.interval_ms}u, .offset_ms = {g.offset_ms}u, "
                     f".stale_timeout_ms = {g.stale_timeout_ms}u, .start_address = {g.start}u, "
                     f".count = {g.count}u, .function_code = {g.function_code}u }},  /* {g.name} */")
        c.append("};")
        c.append("")
        c.append(f"static const ems_reg_mapping_t s_map{mi}_mappings[{len(m.mappings)}] = {{")
        for mp in m.mappings:
            macro = "EMS_PARAM_" + mp.param.upper().replace(".", "_")
            c.append(f"    {{ /* {mp.mapping_id}: {mp.param} @ {mp.address} {mp.type} {mp.order}, {mp.unit}"
                     + (f"; {mp.test_vector}" if mp.test_vector else "") + " */")
            c.append(f"        .scale = {_c_double(mp.scale)}, .offset = {_c_double(mp.offset)}, "
                     f".min = {_c_double(mp.min)}, .max = {_c_double(mp.max)},")
            c.append(f"        .mapping_id = {mp.mapping_id}u, .param_id = {macro}, .address = {mp.address}u, "
                     f".bit_mask = 0x{mp.bit_mask:04X}u,")
            c.append(f"        .group_index = {mp.group_index}u, .type = {REG_TYPES[mp.type][0]}, "
                     f".order = {ORDERS[mp.order][0]}, .flags = {_c_flags(mp.flags)} }},")
        c.append("};")
        c.append("")

    c.append(f"static const ems_cfg_map_t s_maps[{len(model.maps)}] = {{")
    for mi, m in enumerate(model.maps):
        c.append(f"    [{mi}] = {{ .map_id = {_c_str(m.map_id)}, .map_version = {_c_str(m.version)}, "
                 f".groups = s_map{mi}_groups, .mappings = s_map{mi}_mappings, "
                 f".mapping_count = {len(m.mappings)}u, .group_count = {len(m.groups)}u, "
                 f".classification = EMS_CLASS_{m.classification} }},")
    c += ["};", ""]

    c.append(f"static const ems_cfg_profile_t s_profiles[{len(model.profiles)}] = {{")
    for pi, p in enumerate(model.profiles):
        c.append(f"    [{pi}] = {{ .profile_id = {_c_str(p.profile_id)}, .profile_version = {_c_str(p.version)}, "
                 f".device_type = {_c_str(p.device_type)},")
        c.append(f"            .counter_max_rate_per_hour = {_c_double(counter_max_rate(model, p))}, "
                 f".map_index = {model.maps.index(model.map_for(p))}u, "
                 f".classification = EMS_CLASS_{p.classification}, .ratio_handling = EMS_RATIO_{p.ratio_handling} }},")
    c += ["};", ""]

    c.append(f"static const ems_cfg_bus_t s_buses[{len(model.buses)}] = {{")
    for bi, b in enumerate(model.buses):
        c.append(f"    [{bi}] = {{ .bus_id = {_c_str(b.bus_id)}, .baud = {b.baud}u, .uart_num = {b.uart}u, "
                 f".tx_gpio = {b.tx}u, .rx_gpio = {b.rx}u, .rts_gpio = {b.rts}u, "
                 f".parity = EMS_PARITY_{b.parity}, .stop_bits = {b.stop_bits}u }},")
    c += ["};", ""]

    c.append(f"static const ems_cfg_device_t s_devices[{len(model.devices)}] = {{")
    for d in model.devices:
        p = model.profile(d.profile_id)
        rm = model.map_for(p)
        c.append(f"    [{d.slot}] = {{ .instance_id = {_c_str(d.instance_id)}, .location = {_c_str(d.location)},")
        c.append(f"            .ct_ratio = {_c_double(d.ct)}, .pt_ratio = {_c_double(d.pt)}, "
                 f".probe_interval_ms = {d.probe_interval_ms}u, .timeout_ms = {d.timeout_ms}u,")
        c.append(f"            .slot = {d.slot}u, .profile_index = {model.profiles.index(p)}u, "
                 f".bus_index = {model.bus_index(d.bus_id)}u, .slave_id = {d.slave_id}u, "
                 f".max_retries = {d.max_retries}u, .comm_lost_threshold = {d.comm_lost_threshold}u,")
        c.append(f"            .probe_group = {rm.group_index(d.probe_group)}u /* {d.probe_group} */, "
                 f".enabled = {'true' if d.enabled else 'false'} }},")
    c += ["};", ""]

    c += [
        "const ems_config_build_t ems_config_build = {",
        f"    .site_id = {_c_str(model.site_id)},",
        f"    .tool_version = {_c_str(TOOL_VERSION)},",
        f"    .umd_version = {_c_str(model.umd['umd_version'])},",
        f"    .inputs_sha256 = {_c_str(ih)},",
        f"    .inputs_summary = {_c_str(inputs_summary(model))},",
        "    .buses = s_buses, .maps = s_maps, .profiles = s_profiles, .devices = s_devices,",
        f"    .crc32 = 0x{crc:08X}u,",
        f"    .bus_count = {len(model.buses)}u, .map_count = {len(model.maps)}u, "
        f".profile_count = {len(model.profiles)}u, .device_count = {len(model.devices)}u,",
        f"    .target = EMS_TARGET_{model.target.upper()}, .classification = EMS_CLASS_{model.classification},",
        "};",
        "",
    ]
    return "\n".join(h), "\n".join(c)


# ============================================================ export-sim ===
def export_sim(model: Model) -> dict:
    """Register layout for the simulator, from the same model as the C tables (GEN-008)."""
    devices = []
    for d in model.devices:
        p = model.profile(d.profile_id)
        rm = model.map_for(p)
        devices.append({
            "slot": d.slot, "instance_id": d.instance_id, "slave_id": d.slave_id, "enabled": d.enabled,
            "bus_id": d.bus_id, "ct_ratio": d.ct, "pt_ratio": d.pt, "ratio_handling": p.ratio_handling,
            "profile_id": p.profile_id, "map_id": rm.map_id, "map_version": rm.version,
            "classification": rm.classification,
            "poll_groups": [{"name": g.name, "function_code": g.function_code, "start": g.start, "count": g.count,
                             "interval_ms": g.interval_ms} for g in rm.groups],
            "registers": [{"mapping_id": m.mapping_id, "param": m.param, "runtime_id": m.runtime_id,
                           "group": m.group_name, "address": m.address, "regs": m.regs, "type": m.type,
                           "order": m.order, "scale": m.scale, "offset": m.offset, "unit": m.unit,
                           "min": m.min, "max": m.max, "flags": m.flags, "bit_mask": m.bit_mask,
                           "test_vector": m.test_vector} for m in rm.mappings],
        })
    return {"format": "myems.simlayout/1.0", "generated_by": f"myems_profile_tool.py {TOOL_VERSION}",
            "site_id": model.site_id, "target": model.target, "config_crc32": f"0x{config_crc32(model):08X}",
            "inputs_sha256": inputs_hash(model), "buses": [vars(b) for b in model.buses], "devices": devices}


# ===================================================================== CLI ===
def _normalise(text: str) -> str:
    return text.replace("\r\n", "\n")


def _print_errors(rep: Report) -> None:
    for e in rep.errors:
        print(str(e), file=sys.stderr)
    print(f"{len(rep.errors)} error(s); nothing generated.", file=sys.stderr)


def _summary(model: Model) -> str:
    lines = [f"site {model.site_id}: {len(model.devices)} device(s), {len(model.buses)} bus(es), "
             f"classification {model.classification}, target {model.target}"]
    for d in model.devices:
        p = model.profile(d.profile_id)
        rm = model.map_for(p)
        lines.append(f"  slot {d.slot} {d.instance_id}: slave {d.slave_id} on {d.bus_id}, profile {p.profile_id} "
                     f"{p.version}, map {rm.map_id} {rm.version}: {len(rm.groups)} poll groups, "
                     f"{len(rm.mappings)} mappings{'' if d.enabled else ', DISABLED'}")
    lines.append(f"config CRC32 0x{config_crc32(model):08X}, inputs sha256 {inputs_hash(model)}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="myems_profile_tool.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--config-dir", type=Path, default=DEFAULT_CONFIG_DIR, help="config folder (default: config/)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("validate", "generate", "budget", "export-sim"):
        sp = sub.add_parser(name)
        sp.add_argument("--site", type=Path, required=True)
        if name == "generate":
            sp.add_argument("--target", choices=("sim", "prod"), required=True)
            sp.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
            sp.add_argument("--check", action="store_true", help="do not write; exit 1 if files differ")
        elif name == "validate":
            sp.add_argument("--target", choices=("sim", "prod"), default="sim")
        elif name == "export-sim":
            sp.add_argument("--out", type=Path, default=None)
    args = ap.parse_args(argv)

    target = getattr(args, "target", "sim")
    model, rep = build_model(args.site, target, args.config_dir)
    if model is None:
        _print_errors(rep)
        return 1

    if args.cmd == "validate":
        print(_summary(model))
        print("VALID")
        return 0
    if args.cmd == "budget":
        print(budget_text(model), end="")
        return 0
    if args.cmd == "export-sim":
        out = args.out or (DEFAULT_SIM_DIR / f"{model.site_id.lower()}.sim.json")
        out.parent.mkdir(parents=True, exist_ok=True)
        with open(out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(export_sim(model), fh, indent=2)
            fh.write("\n")
        print(f"wrote {out}")
        return 0

    # generate
    header, source = render(model)
    files = ((args.out_dir / "ems_config_gen.h", header), (args.out_dir / "ems_config_gen.c", source))
    if args.check:
        stale = [str(p) for p, t in files if not p.is_file() or _normalise(p.read_text(encoding="utf-8")) != t]
        if stale:
            print("OUT OF DATE (run generate):", *stale, sep="\n  ", file=sys.stderr)
            return 1
        print(f"OK: generated files match {args.site.name} (CRC32 0x{config_crc32(model):08X})")
        return 0
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for path, text in files:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print(f"wrote {path}")
    print(_summary(model))
    return 0


if __name__ == "__main__":
    sys.exit(main())
