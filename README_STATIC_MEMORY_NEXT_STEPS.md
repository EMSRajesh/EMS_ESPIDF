# MyEMS RTU project package notes

This source package retains the tested Modbus RTU self-test and normal polling code, applies the supplied 16 MB partition CSV, and sets ESP-IDF defaults for 16 MB flash and FreeRTOS static allocation support.

**Important:** static allocation support is enabled, but the existing transport/master/scheduler/acquisition components still contain dynamic allocation calls. See `docs/STATIC_MEMORY_AND_PARTITIONS.md`. This package is a configuration/source baseline, not a claim that the no-dynamic-allocation code refactor is complete.
