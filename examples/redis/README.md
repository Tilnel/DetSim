# Redis SET/GET

`redis/` is the pinned Redis source submodule. `client.c` is the SET/GET client;
`fs_root/redis.conf` supplies the server settings and `simple_start.json` is the
manual two-process configuration. `plugins/` retains this case's plugins.

```sh
python3 examples/redis/run.py --tracer ./tracer
```

Build an NP=2 tracer and the Redis server first. `run.py` builds an owned client
copy and controls a bounded server/client schedule, checking actual SET/GET
replies and the client's zero exit. It does not perform automatic exploration
or explicit saved-edge replay.
The runner starts from `simple_start.json`, replaces tracee and filesystem paths
with explicit absolute inputs, derives the per-binary startup boundary and
writes the exact resulting `<session>/config.json`. Transcripts, command records
and results go beneath `--output` (default `build/redis-check`).

`--diagnostic-strip` strips debug information from an owned server copy only;
this diagnostic profile is recorded separately. The runner never modifies the
source submodule or the original server executable.
