#!/usr/bin/env python3
"""MyEMS Universal Measurement Dictionary (UMD) code generator.

DP-WP4 sub-package 4B, milestone D1.

Reads config/umd/umd_v1.json and writes
  components/ems_measurement/include/ems_param_ids.h
  components/ems_measurement/src/ems_param_table.c

Rules enforced (DP-WP4 Design v1.1 Section 6.1, DP-WP5 Design v1.0 Section 4.5):
  * string IDs are lower-case dotted names whose first part is a known domain;
  * runtime IDs are 16-bit, unique, and their high byte equals the domain code;
  * units and value types come from the lists in the JSON file;
  * runtime IDs are permanent: if a generated header already exists, every
    parameter in it must keep its runtime ID, and a parameter may only
    disappear by being listed under "retired"; retired names and IDs are
    never reused;
  * (dictionary 1.1) every parameter 'role' is in the 'roles' list, and every
    capability lists only existing parameters (U-04);
  * every dictionary under config/umd/released/ is still honoured: each of its
    parameters keeps its runtime ID, unit and value type, or is retired (U-06).

Roles and capabilities are host-side data (profile tool, simulator); they are
not written into the C files.

Output is deterministic (sorted by runtime ID, LF line endings, no time
stamps), so it can be committed and checked with --check.

Usage (from the project root):
  python tools/profile/umd_gen.py            # validate and (re)generate
  python tools/profile/umd_gen.py --check    # exit 1 if generated files are out of date
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

GENERATOR_VERSION = "1.1.0"
SCHEMAS = ("myems.umd/1.0", "myems.umd/1.1")
SCHEMA = SCHEMAS[-1]

PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UMD = PROJECT_ROOT / "config" / "umd" / "umd_v1.json"
DEFAULT_RELEASED_DIR = PROJECT_ROOT / "config" / "umd" / "released"
DEFAULT_HEADER = PROJECT_ROOT / "components" / "ems_measurement" / "include" / "ems_param_ids.h"
DEFAULT_SOURCE = PROJECT_ROOT / "components" / "ems_measurement" / "src" / "ems_param_table.c"

# JSON unit symbol -> C enumerator (ems_unit_t in ems_measurement.h)
UNIT_ENUM = {
    "-": "EMS_UNIT_NONE",
    "V": "EMS_UNIT_V",
    "A": "EMS_UNIT_A",
    "kW": "EMS_UNIT_KW",
    "kvar": "EMS_UNIT_KVAR",
    "kVA": "EMS_UNIT_KVA",
    "1": "EMS_UNIT_RATIO",
    "Hz": "EMS_UNIT_HZ",
    "kWh": "EMS_UNIT_KWH",
}
# JSON value type -> C enumerator (ems_value_type_t)
VALUE_TYPE_ENUM = {
    "float": "EMS_VT_FLOAT",
    "counter": "EMS_VT_COUNTER",
    "enum": "EMS_VT_ENUM",
}

NAME_RE = re.compile(r"^[a-z][a-z0-9]*(\.[a-z0-9_]+)+$")
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
HEX16_RE = re.compile(r"^0x[0-9A-Fa-f]{4}$")
HEX8_RE = re.compile(r"^0x[0-9A-Fa-f]{2}$")
ROLE_RE = re.compile(r"^[a-z]+(\.[a-z]+)*$")
CAP_RE = re.compile(r"^[a-z][a-z0-9_]*$")
DEFINE_RE = re.compile(r"^#define\s+(EMS_PARAM_[A-Z0-9_]+)\s+\(\(uint16_t\)0x([0-9A-F]{4})u\)\s*$")


class UmdError(Exception):
    """Validation error; message names the rule and the offending entry."""


@dataclass(frozen=True)
class Param:
    name: str
    runtime_id: int
    unit: str
    value_type: str
    description: str
    role: str = ""

    @property
    def macro(self) -> str:
        return "EMS_PARAM_" + self.name.upper().replace(".", "_")


def _canonical_hash(data: dict) -> str:
    """SHA-256 of the JSON content, independent of formatting and line endings."""
    canon = json.dumps(data, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(canon.encode("ascii")).hexdigest()


def load_umd(path: Path) -> tuple[dict, list[Param]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise UmdError(f"{path}: invalid JSON: {exc}") from exc
    return data, validate(data, str(path))


def validate(data: dict, origin: str = "umd") -> list[Param]:
    def err(msg: str) -> UmdError:
        return UmdError(f"{origin}: {msg}")

    for key in ("schema", "umd_version", "domains", "units", "value_types", "parameters", "retired"):
        if key not in data:
            raise err(f"missing required field '{key}'")
    if data["schema"] not in SCHEMAS:
        raise err(f"unsupported schema '{data['schema']}', expected one of {list(SCHEMAS)}")
    v11 = data["schema"] != SCHEMAS[0]
    roles: set[str] = set()
    if v11:
        for key in ("roles", "capabilities"):
            if key not in data:
                raise err(f"missing required field '{key}' (dictionary 1.1)")
        if not isinstance(data["roles"], list) or not all(isinstance(r, str) and ROLE_RE.match(r)
                                                          for r in data["roles"]):
            raise err("U-04: 'roles' must be a list of lower-case dotted names")
        if len(set(data["roles"])) != len(data["roles"]):
            raise err("U-04: 'roles' lists a role twice")
        roles = set(data["roles"])
    else:
        for key in ("roles", "capabilities"):
            if key in data:
                raise err(f"'{key}' needs schema '{SCHEMAS[1]}'")
    if not SEMVER_RE.match(str(data["umd_version"])):
        raise err(f"umd_version '{data['umd_version']}' is not MAJOR.MINOR.PATCH")

    domains: dict[str, int] = {}
    for dom, code in data["domains"].items():
        if not re.match(r"^[a-z][a-z0-9]*$", dom) or not isinstance(code, str) or not HEX8_RE.match(code):
            raise err(f"domain '{dom}': code must be a string like '0x01'")
        value = int(code, 16)
        if value in domains.values():
            raise err(f"domain '{dom}': code {code} used twice")
        domains[dom] = value

    units = data["units"]
    for u in units:
        if u not in UNIT_ENUM:
            raise err(f"unit '{u}' has no C enumerator; add it to UNIT_ENUM and ems_unit_t together")
    vtypes = data["value_types"]
    for v in vtypes:
        if v not in VALUE_TYPE_ENUM:
            raise err(f"value type '{v}' has no C enumerator")

    retired_names: set[str] = set()
    retired_ids: set[int] = set()
    for r in data["retired"]:
        if "id" not in r or "runtime_id" not in r or not HEX16_RE.match(str(r["runtime_id"])):
            raise err(f"retired entry {r!r} needs 'id' and 'runtime_id' ('0x....')")
        retired_names.add(r["id"])
        retired_ids.add(int(r["runtime_id"], 16))

    params: list[Param] = []
    seen_names: set[str] = set()
    seen_ids: dict[int, str] = {}
    for i, p in enumerate(data["parameters"]):
        where = f"parameters[{i}]"
        for key in ("id", "runtime_id", "unit", "value_type", "description"):
            if key not in p:
                raise err(f"{where}: missing '{key}'")
        name = p["id"]
        where = f"parameter '{name}'"
        if not isinstance(name, str) or not NAME_RE.match(name):
            raise err(f"{where}: id must be lower-case dotted, e.g. 'ac.voltage.l1_n'")
        dom = name.split(".", 1)[0]
        if dom not in domains:
            raise err(f"{where}: unknown domain '{dom}'")
        if not isinstance(p["runtime_id"], str) or not HEX16_RE.match(p["runtime_id"]):
            raise err(f"{where}: runtime_id must be a string like '0x0101'")
        rid = int(p["runtime_id"], 16)
        if (rid >> 8) != domains[dom]:
            raise err(f"{where}: runtime_id {p['runtime_id']} is outside domain '{dom}' (0x{domains[dom]:02X}xx)")
        if name in seen_names:
            raise err(f"{where}: duplicate id")
        if rid in seen_ids:
            raise err(f"{where}: runtime_id {p['runtime_id']} already used by '{seen_ids[rid]}'")
        if name in retired_names:
            raise err(f"{where}: id is retired and cannot be reused")
        if rid in retired_ids:
            raise err(f"{where}: runtime_id {p['runtime_id']} is retired and cannot be reused")
        if p["unit"] not in units:
            raise err(f"{where}: unit '{p['unit']}' not in units list")
        if p["value_type"] not in vtypes:
            raise err(f"{where}: value_type '{p['value_type']}' not in value_types list")
        desc = p["description"]
        if not isinstance(desc, str) or not desc.strip():
            raise err(f"{where}: description must be non-empty")
        if '"' in desc or "\\" in desc or not desc.isascii():
            raise err(f"{where}: description must be ASCII without quotes or backslashes")
        role = p.get("role", "")
        if role and not v11:
            raise err(f"{where}: 'role' needs schema '{SCHEMAS[1]}'")
        if role and role not in roles:
            raise err(f"U-04: {where}: role '{role}' is not in the 'roles' list")
        seen_names.add(name)
        seen_ids[rid] = name
        params.append(Param(name, rid, p["unit"], p["value_type"], desc, role))

    if not params:
        raise err("no parameters defined")
    if v11:
        caps = data["capabilities"]
        if not isinstance(caps, dict):
            raise err("U-04: 'capabilities' must be an object: capability -> list of parameter ids")
        for cap, plist in caps.items():
            if not CAP_RE.match(cap):
                raise err(f"U-04: capability '{cap}': name must be lower case letters, digits and '_'")
            if not isinstance(plist, list) or not all(isinstance(x, str) for x in plist):
                raise err(f"U-04: capability '{cap}': value must be a list of parameter ids")
            for x in plist:
                if x not in seen_names:
                    raise err(f"U-04: capability '{cap}': unknown parameter '{x}'")
    return sorted(params, key=lambda q: q.runtime_id)


def capabilities(data: dict) -> dict[str, tuple[str, ...]]:
    """Capability -> parameter ids (dictionary 1.1). Empty for a 1.0 dictionary."""
    return {k: tuple(v) for k, v in data.get("capabilities", {}).items()}


def check_released(params: list[Param], data: dict, released_dir: Path = DEFAULT_RELEASED_DIR) -> None:
    """U-06: every released dictionary is honoured (same runtime ID, unit, value type, or retired)."""
    if not released_dir.is_dir():
        return
    current = {p.name: p for p in params}
    retired = {r["id"] for r in data["retired"]}
    for path in sorted(released_dir.glob("*.json")):
        try:
            rel = json.loads(path.read_text(encoding="utf-8"))
            rel_params = validate(rel, str(path))
        except (json.JSONDecodeError, UmdError) as exc:
            raise UmdError(f"U-06: released dictionary {path.name} is not valid: {exc}") from exc
        for rp in rel_params:
            cp = current.get(rp.name)
            if cp is None:
                if rp.name not in retired:
                    raise UmdError(f"U-06: '{rp.name}' of released dictionary {rel['umd_version']} "
                                   "disappeared. Move it to 'retired' instead of deleting it.")
                continue
            for attr in ("runtime_id", "unit", "value_type"):
                if getattr(cp, attr) != getattr(rp, attr):
                    raise UmdError(f"U-06: '{rp.name}' {attr} changed from {getattr(rp, attr)!r} "
                                   f"(released {rel['umd_version']}) to {getattr(cp, attr)!r}")


def read_existing_ids(header: Path) -> dict[str, int]:
    """Macro name -> runtime ID from a previously generated header (empty if none)."""
    if not header.is_file():
        return {}
    ids: dict[str, int] = {}
    for line in header.read_text(encoding="utf-8").splitlines():
        m = DEFINE_RE.match(line)
        if m:
            ids[m.group(1)] = int(m.group(2), 16)
    return ids


def check_stability(params: list[Param], data: dict, existing: dict[str, int]) -> None:
    """Runtime IDs of already generated parameters must never change or vanish silently."""
    current = {p.macro: p.runtime_id for p in params}
    retired_macros = {"EMS_PARAM_" + r["id"].upper().replace(".", "_") for r in data["retired"]}
    for macro, rid in existing.items():
        if macro in current and current[macro] != rid:
            raise UmdError(
                f"stability: {macro} was 0x{rid:04X} in the existing header but is "
                f"0x{current[macro]:04X} now. Runtime IDs are permanent.")
        if macro not in current and macro not in retired_macros:
            raise UmdError(
                f"stability: {macro} (0x{rid:04X}) disappeared. Move it to 'retired' instead of deleting it.")


def render_header(data: dict, params: list[Param]) -> str:
    h = _canonical_hash(data)
    width = max(len(p.macro) for p in params)
    out = [
        "/*",
        " * GENERATED FILE - DO NOT EDIT.",
        " * Source:    config/umd/umd_v1.json",
        f" * UMD:       {data['umd_version']}  (sha256 {h[:16]})",
        f" * Generator: tools/profile/umd_gen.py {GENERATOR_VERSION}",
        " * Regenerate: python tools/profile/umd_gen.py",
        " */",
        "#ifndef EMS_PARAM_IDS_H",
        "#define EMS_PARAM_IDS_H",
        "",
        "#include <stdint.h>",
        "",
        f'#define EMS_UMD_VERSION "{data["umd_version"]}"',
        f'#define EMS_UMD_SHA256_PREFIX "{h[:16]}"',
        f"#define EMS_PARAM_COUNT {len(params)}u",
        "",
        "/* Domain codes: high byte of a runtime ID. */",
    ]
    for dom, code in sorted(data["domains"].items(), key=lambda kv: int(kv[1], 16)):
        out.append(f"#define EMS_DOMAIN_{dom.upper()} 0x{int(code, 16):02X}u")
    out += ["", "/* Parameter runtime IDs (permanent). */"]
    for p in params:
        out.append(f"#define {p.macro.ljust(width)} ((uint16_t)0x{p.runtime_id:04X}u)")
    out += ["", "#endif /* EMS_PARAM_IDS_H */", ""]
    return "\n".join(out)


def render_source(data: dict, params: list[Param]) -> str:
    h = _canonical_hash(data)
    out = [
        "/*",
        " * GENERATED FILE - DO NOT EDIT.",
        " * Source:    config/umd/umd_v1.json",
        f" * UMD:       {data['umd_version']}  (sha256 {h[:16]})",
        f" * Generator: tools/profile/umd_gen.py {GENERATOR_VERSION}",
        " */",
        '#include "ems_measurement.h"',
        '#include "ems_param_ids.h"',
        "",
        "/* Sorted by runtime ID. */",
        "const ems_param_info_t ems_param_table[EMS_PARAM_COUNT] = {",
    ]
    for p in params:
        out.append(
            f'    {{ {p.macro}, {UNIT_ENUM[p.unit]}, {VALUE_TYPE_ENUM[p.value_type]}, '
            f'"{p.name}", "{p.description}" }},')
    out += [
        "};",
        "",
        "const size_t ems_param_table_len = EMS_PARAM_COUNT;",
        "",
    ]
    return "\n".join(out)


def _normalise(text: str) -> str:
    return text.replace("\r\n", "\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--umd", type=Path, default=DEFAULT_UMD)
    ap.add_argument("--out-header", type=Path, default=DEFAULT_HEADER)
    ap.add_argument("--out-source", type=Path, default=DEFAULT_SOURCE)
    ap.add_argument("--released-dir", type=Path, default=DEFAULT_RELEASED_DIR,
                    help="folder of released dictionaries checked by rule U-06")
    ap.add_argument("--check", action="store_true",
                    help="do not write; exit 1 if the generated files differ from the JSON")
    args = ap.parse_args(argv)

    try:
        data, params = load_umd(args.umd)
        check_released(params, data, args.released_dir)
        check_stability(params, data, read_existing_ids(args.out_header))
    except UmdError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    header = render_header(data, params)
    source = render_source(data, params)

    if args.check:
        stale = [str(p) for p, text in ((args.out_header, header), (args.out_source, source))
                 if not p.is_file() or _normalise(p.read_text(encoding="utf-8")) != text]
        if stale:
            print("OUT OF DATE (run python tools/profile/umd_gen.py):", *stale, sep="\n  ", file=sys.stderr)
            return 1
        print(f"OK: generated files match UMD {data['umd_version']} ({len(params)} parameters)")
        return 0

    for path, text in ((args.out_header, header), (args.out_source, source)):
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
        print(f"wrote {path}")
    print(f"UMD {data['umd_version']}: {len(params)} parameters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
