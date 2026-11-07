# MyEMS firmware repository layout

Established in DP-WP4 milestone D0 (see `docs/D0_CHECKLIST.md`). Baseline tag: `com-09_057`.

```text
<project root>
├── CMakeLists.txt, sdkconfig.defaults, partitions_myems_esp32s3_16mb.csv
├── sdkconfig.<mode>          Named build configurations (production, selftest, wp24_*, wp25_*).
│                             Kept in the root: COM build scripts and runners load them from here.
├── README.md, BUILD_MODES.md
├── components/               rs485_transport, modbus_rtu_master, ems_stack_diag (COM track)
├── main/                     app_main, poll scheduler, acquisition, COM self-test
├── tools/                    Build scripts, simulators, test runners
│   ├── d0/                   D0 housekeeping and REG-01 verification scripts
│   └── test_results/         Test evidence (on disk only, ignored by Git)
├── docs/
│   ├── concurrency_contract.md, docs_architecture.md, STATIC_MEMORY_AND_MODBUS_SERVICES.md,
│   │   STATIC_MEMORY_AND_PARTITIONS.md, MyEMS ESP32-S3 Modbus RTU Project Report.docx
│   ├── com_tests/            How to run the COM self-test and WP24/WP25 tests
│   ├── REPOSITORY_LAYOUT.md  This file
│   └── D0_CHECKLIST.md       D0 steps and exit record
└── archive/                  Read-only history (see archive/README.md)
    ├── com_history/          Change notes and patch manifests of finished COM work packages
    ├── patches/              .patch / .diff files already applied to the source
    ├── snapshots/            Old source copies and delivered patch packages
    └── test_logs/            Loose logs from early WP2.2 runs
```

## Conventions

- **Track prefixes:** `COM-` for the communication track (COM-WP2.4, COM-WP2.5 …), `DP-` for the device-profile track (DP-WP4 …).
- **Branches:** `main` holds accepted work. Feature work goes on `dp-wp4/<milestone>` or `com-wp2.x/<topic>` branches.
- **Tags:** `com-09_057` = baseline before DP-WP4. Milestone exits are tagged `dp-wp4-d<N>`.
- **Build folders** are never committed; each mode uses its own `build_<mode>` folder.
- **sdkconfig files:** ESP-IDF writes defaults back into the `sdkconfig.<mode>` file it is given. After a build, review `git diff sdkconfig.*` and commit intended changes only.
- **Planned move:** `sdkconfig.<mode>` files move to `configs/` after COM-WP2.4 is closed and the COM scripts are updated in the same commit.
