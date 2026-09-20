<!-- reloadable-for: fastcached -->
<!-- Checked against CliOptions() by ctest -R node-reloadable-docs. A setting added to
     or removed from the reloadable column of that table without this row moving is a
     refusal, in BOTH directions: an omission costs a reader a stale page, and an
     over-claim costs an operator, who edits the file, sends SIGHUP, sees no error and
     believes a setting is in force that is not (#1070). -->

| Reloadable | Requires a restart |
|---|---|
| `log_level`, `max_memory`, `requirepass`, `auth_username`, `notify_keyspace_events` | `bind`, `port`, `listen`, `listen_tls`, `storage_path`, `storage_shards`, `storage_durability`, `storage_max_value`, `threads`, `active_expiry_interval`, `active_expiry_scan` |
