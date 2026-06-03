# Binary status codes

Every status fastcached emits in the binary response header.

| Hex    | Name              | Meaning |
|--------|-------------------|---------|
| `0x00` | Ok                | Success |
| `0x01` | KeyNotFound       | The key does not exist |
| `0x02` | KeyExists         | The key exists, but the operation required it absent (or CAS mismatch) |
| `0x03` | ValueTooLarge     | The value exceeded the configured `max_item_size` |
| `0x04` | InvalidArguments  | The request was malformed |
| `0x05` | ItemNotStored     | The store failed (precondition) |
| `0x06` | IncrOnNonNumeric  | `Increment` / `Decrement` on a non-numeric value |
| `0x20` | AuthError         | SASL not supported (every SASL opcode returns this) |
| `0x81` | UnknownCommand    | Unrecognised opcode |
| `0x82` | OutOfMemory       | Allocation failure |
| `0x83` | NotSupported      | Reserved — defined for parity, not emitted yet |
| `0x84` | InternalError     | Reserved — defined for parity, not emitted yet |
| `0x85` | Busy              | Reserved — defined for parity, not emitted yet |
| `0x86` | TemporaryFailure  | Reserved — defined for parity, not emitted yet |
