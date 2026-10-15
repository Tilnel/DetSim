#!/usr/bin/env python3
"""Run read-only tools against index-free logs with current metadata archives."""
import ctypes
import pathlib
import struct
import subprocess
import sys
import tempfile

query, verify = [str(pathlib.Path(x).resolve()) for x in sys.argv[1:3]]
nodes = int(sys.argv[3]) if len(sys.argv) > 3 else 1
xxhash = ctypes.CDLL("libxxhash.so.0")
xxhash.XXH3_64bits.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
xxhash.XXH3_64bits.restype = ctypes.c_uint64
legacy_payload = struct.pack("<" + "Q" * nodes + "i" * (nodes + 1), *range(1, nodes + 1), *([1] * nodes), 4)
payload = struct.pack("<III", 0x4453534d, 2, nodes) + legacy_payload + struct.pack("<?QI", False, 0, 0)
identity = struct.pack("<II", 0x44535335, nodes) + payload
key = xxhash.XXH3_64bits(identity, len(identity))
checksum = 0
for byte in payload:
    checksum = (checksum * 31 + byte) & 0xffffffff
record = struct.pack("<IQII", 0x53534545, key, len(payload), checksum) + payload
with tempfile.TemporaryDirectory(prefix="detsim-metadata-tools-") as tmp:
    root = pathlib.Path(tmp)
    store = root / "sstate"
    store.mkdir()
    data = store / "packed.dat"
    data.write_bytes(record)
    def run(program, *args, ok=True):
        before = {p.name: p.read_bytes() for p in store.iterdir()}
        result = subprocess.run([program, *args, str(store)], cwd=root, capture_output=True, text=True, timeout=10)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        assert before == {p.name: p.read_bytes() for p in store.iterdir()}, "tool modified store"
        return result.stdout
    assert f"{key:016X}" in run(query, "list")
    assert "error_bound=4" in run(query, "info", f"{key:x}")
    run(query, "search", "1")
    run(query, "search", "deadbeef", ok=False)
    run(query, "info", "not-hex", ok=False)
    run(query, "verify")
    run(verify, "-v")
    (store / "packed.idx").write_bytes(b"stale")
    (store / "packed.inc").write_bytes(b"partial")
    run(verify)
    for broken in (record + b"x", record[:-1], record[:-1] + bytes([record[-1] ^ 1]), record[:4] + struct.pack("<Q", key ^ 1) + record[12:]):
        data.write_bytes(broken)
        run(query, "verify", ok=False)
        run(verify, ok=False)
    legacy_checksum = 0
    for byte in legacy_payload:
        legacy_checksum = (legacy_checksum * 31 + byte) & 0xffffffff
    legacy_identity = struct.pack("<II", 0x44535334, nodes) + legacy_payload
    legacy_key = xxhash.XXH3_64bits(legacy_identity, len(legacy_identity))
    data.write_bytes(struct.pack("<IQII", 0x53534545, legacy_key, len(legacy_payload), legacy_checksum) + legacy_payload)
    run(query, "verify", ok=False)
    run(verify, ok=False)
    data.write_bytes(b"")
    run(query, "list")
    run(verify)
print("PASS metadata tools: no index, stale index, query/search, checksum/tail/key rejection, read-only")
