# AUTH

**Protocols:** Redis RESP2 · **Status:** Rejected

fastcached does not implement Redis authentication. Any `AUTH` request
returns `-ERR Client sent AUTH, but no password is set`. Run the
daemon behind a network ACL or TLS-terminating proxy if access
control is required.

## Synopsis

```text
AUTH [username] password
```

## Response

```text
-ERR ...
```
