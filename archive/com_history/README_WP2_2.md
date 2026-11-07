# WP2.2 transport recovery implementation

See `WP2_2_IMPLEMENTATION_NOTES.md` at the package root for API usage, integration contract, validation commands, and known limits.

Changed component files:
- `components/rs485_transport/include/rs485_transport.h`
- `components/rs485_transport/src/rs485_transport.c`

This is a WP2.2 transport-only change. The Modbus master is intentionally unchanged; WP2.3 must integrate the recovery API before the stale-response test can pass.
