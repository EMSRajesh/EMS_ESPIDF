# USB-RS485 test utility added

The `tools/` directory now contains a Python USB-to-RS485 Modbus RTU master test utility,
its `pyserial` dependency file, and setup/wiring instructions. Its offline protocol self-test
passed in the generation environment.

Important scope note: this package addition does not mean the ESP-IDF C firmware has been
compiled or hardware-tested in this environment. The source-level concurrency findings in
`docs/concurrency_contract.md` remain to be addressed and validated separately before
production use. In particular, completion-queue overflow handling, task-stop/resource lifetime
coordination, and stats synchronization must be hardened and retested.
