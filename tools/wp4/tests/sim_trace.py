"""Deterministic simulator traces for the AC regression check (DP-WP5 Test Spec SIM5-001).

Each scenario drives the simulator with a fake clock for 120 s and reads every
poll group of every slave every 0.5 s. The SHA-256 of all replies (data, delay,
note) is compared with digests recorded from the DP-WP4 D9 simulator
(tools/wp4/tests/data/sim_ac_golden_wp4.json), so the role-based value model
of DP-WP5 must reproduce the AC meter exactly.
"""
from __future__ import annotations

import hashlib

# (name, site file, list of (time offset s, control text))
SCENARIOS = [
    (f"{site}:{mode}", site, [(0.0, f"mode * {mode}")])
    for site in ("lab_bench", "lab_bench_2dev")
    for mode in ("NORMAL", "LOAD_CHANGE:10", "IMPORT", "EXPORT", "OUT_OF_RANGE", "COUNTER_RESET",
                 "TIMEOUT", "EXCEPTION:02", "EXCEPTION:02@energy", "OUT_OF_RANGE@electrical",
                 "BAD_LENGTH", "BAD_CRC", "SLOW:400", "OFFLINE_THEN_RECOVER:60")
] + [
    ("lab_bench_2dev:sequence", "lab_bench_2dev",
     [(0.0, "mode 1 IMPORT\nmode 2 EXPORT"), (30.0, "mode 1 EXPORT\nmode 2 LOAD_CHANGE:7"),
      (60.0, "mode 1 COUNTER_RESET\nset 2 ac.current.l1 2.5"), (90.0, "mode * LOAD_CHANGE:5")]),
    ("lab_bench:override", "lab_bench",
     [(0.0, "set 1 ac.power.active.total 40\nmode 1 IMPORT"), (45.0, "mode 1 LOAD_CHANGE:10")]),
]
SEEDS = (1, 7)
DURATION_S, STEP_S = 120.0, 0.5


def trace_digest(sim_mod, layout: dict, steps: list[tuple[float, str]], seed: int) -> str:
    from test_ac_meter_sim import Clock, read_req  # noqa: E402  (same folder)
    clk = Clock(1000.0)
    s = sim_mod.Simulator(layout, seed=seed, clock=clk)
    h = hashlib.sha256()
    pending = sorted(steps)
    t = 0.0
    while t <= DURATION_S:
        while pending and pending[0][0] <= t:
            s.apply_control(pending.pop(0)[1])
        for d in layout["devices"]:
            for g in d["poll_groups"]:
                r = s.handle(read_req(d["slave_id"], g["start"], g["count"], g["function_code"]))
                h.update((r.data or b"-").hex().encode() + f"|{r.delay_s}|{r.note}\n".encode())
        t += STEP_S
        clk.t += STEP_S
    return h.hexdigest()


def all_digests(sim_mod, site_dir) -> dict[str, str]:
    out = {}
    for name, site, steps in SCENARIOS:
        layout = sim_mod.load_layout(site_dir / f"{site}.site.json")
        for seed in SEEDS:
            out[f"{name}#seed{seed}"] = trace_digest(sim_mod, layout, steps, seed)
    return out
