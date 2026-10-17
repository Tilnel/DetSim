# Shared example support

This package contains host command execution, controlled tracer sessions, saved-edge
replay checks, explicit runtime-file manifests and the common NP=1 CLI/report flow.
It owns no workload, semantic-state decoder or target-specific search policy.

Each example's `run.py` owns its workload, plugin selection and model profile.
`Session` writes the exact configuration to `<session>/config.json`; the enclosing
run writes `results.json`, transcripts, command records and replay evidence under
`--output`. These generated files belong in ignored build/output directories.
