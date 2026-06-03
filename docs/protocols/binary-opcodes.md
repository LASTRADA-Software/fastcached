# Binary opcodes

The full opcode table fastcached recognises.

| Hex   | Name        | Status     |
|-------|-------------|------------|
| `0x00` | Get        | Implemented |
| `0x01` | Set        | Implemented |
| `0x02` | Add        | Implemented |
| `0x03` | Replace    | Implemented |
| `0x04` | Delete     | Implemented |
| `0x05` | Increment  | Implemented (with auto-vivify) |
| `0x06` | Decrement  | Implemented (with auto-vivify) |
| `0x07` | Quit       | Implemented |
| `0x08` | Flush      | Implemented |
| `0x09` | GetQ       | Implemented |
| `0x0a` | NoOp       | Implemented |
| `0x0b` | Version    | Implemented |
| `0x0c` | GetK       | Implemented |
| `0x0d` | GetKQ      | Implemented |
| `0x0e` | Append     | Implemented |
| `0x0f` | Prepend    | Implemented |
| `0x10` | Stat       | Implemented |
| `0x11` | SetQ       | Implemented |
| `0x12` | AddQ       | Implemented |
| `0x13` | ReplaceQ   | Implemented |
| `0x14` | DeleteQ    | Implemented |
| `0x15` | IncrementQ | Implemented |
| `0x16` | DecrementQ | Implemented |
| `0x17` | QuitQ      | Implemented |
| `0x18` | FlushQ     | Implemented |
| `0x19` | AppendQ    | Implemented |
| `0x1a` | PrependQ   | Implemented |
| `0x1b` | Verbosity  | Accepted, no-op |
| `0x1c` | Touch      | Implemented |
| `0x1d` | GAT        | Implemented |
| `0x1e` | GATQ       | Implemented |
| `0x20` | SaslList   | Rejected (AuthError) |
| `0x21` | SaslAuth   | Rejected (AuthError) |
| `0x22` | SaslStep   | Rejected (AuthError) |
| `0x23` | GATK       | Implemented |
| `0x24` | GATKQ      | Implemented |
| other  | —          | UnknownCommand |
