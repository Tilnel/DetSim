#!/usr/bin/env python3
"""
Trace to Batch Converter

Converts tracer execution trace files into batch scripts that can replay
the exact same execution path in the tracer.

Usage:
    python3 trace2batch.py trace/example.tr > replay.batch
    ./tracer -c config.json
    (ptmc) batch replay.batch

Trace file format supported:
    - State transitions: hash => hash
    - Syscalls: Tracee N: syscall(...) = result
    - Choices: Tracee N (choose X): syscall(...) = result
"""

import re
import sys
import argparse
from pathlib import Path
from typing import List, Tuple, Optional


class TraceEntry:
    """Represents a single trace entry (one step)."""

    def __init__(self, node_id: int, syscall: str, choice: Optional[int] = None):
        self.node_id = node_id
        self.syscall = syscall
        self.choice = choice

    def __repr__(self):
        choice_str = f" (choice={self.choice})" if self.choice is not None else ""
        return f"TraceEntry(node={self.node_id}{choice_str}): {self.syscall}"


def parse_trace_file(filepath: str) -> List[TraceEntry]:
    """
    Parse a trace file and extract execution steps.

    Supports two formats:
    1. example.tr format:
       hash => hash
        Tracee 2: bind(...) = 0

    2. 2-leader.trace format:
        Tracee 0: sendto(...) = 0
        Tracee 0: (choose 0) gettimeofday(...) = 0
    """
    entries = []

    trace_pattern = re.compile(r"^\s*Tracee\s+(\d+)(?:\s+\(choose\s+(\d+)\))?:\s*(.+)$")

    with open(filepath, "r") as f:
        for line_num, line in enumerate(f, 1):
            line = line.rstrip("\n")

            if "=>" in line and not line.strip().startswith("Tracee"):
                continue

            if "steps in total" in line:
                continue

            if not line.strip():
                continue

            match = trace_pattern.match(line)
            if match:
                node_id = int(match.group(1))
                choice_str = match.group(2)
                syscall = match.group(3).strip()

                choice = int(choice_str) if choice_str is not None else None

                entry = TraceEntry(node_id, syscall, choice)
                entries.append(entry)
            else:
                alt_pattern = re.compile(
                    r"^Tracee\s+(\d+)(?:\s+\(choose\s+(\d+)\))?:\s*(.+)$"
                )
                match = alt_pattern.match(line)
                if match:
                    node_id = int(match.group(1))
                    choice_str = match.group(2)
                    syscall = match.group(3).strip()

                    choice = int(choice_str) if choice_str is not None else None

                    entry = TraceEntry(node_id, syscall, choice)
                    entries.append(entry)

    return entries


def generate_batch_script(
    entries: List[TraceEntry], include_comments: bool = True
) -> str:
    """
    Generate batch script from trace entries.

    For each step:
    1. sw N - switch to node N
    2. si [choice] - step execution with optional choice argument
    3. info sock - show receive buffers
    """
    lines = []

    if include_comments:
        lines.append("# Auto-generated batch script from trace")
        lines.append(f"# Total steps: {len(entries)}")
        lines.append("")

    for i, entry in enumerate(entries):
        if include_comments:
            choice_info = (
                f" (choice={entry.choice})" if entry.choice is not None else ""
            )
            lines.append(f"# Step {i + 1}: Node {entry.node_id}{choice_info}")
            lines.append(f"# {entry.syscall}")

        lines.append(f"sw {entry.node_id}")

        # si command with optional choice argument
        if entry.choice is not None:
            lines.append(f"si {entry.choice}")
        else:
            lines.append("si")

        lines.append("info sock")

        if include_comments:
            lines.append("")

    return "\n".join(lines)


def analyze_trace(entries: List[TraceEntry]) -> dict:
    """Analyze trace and return statistics."""
    stats = {
        "total_steps": len(entries),
        "nodes_involved": set(),
        "choices_made": 0,
        "node_distribution": {},
    }

    for entry in entries:
        stats["nodes_involved"].add(entry.node_id)
        if entry.choice is not None:
            stats["choices_made"] += 1

        stats["node_distribution"][entry.node_id] = (
            stats["node_distribution"].get(entry.node_id, 0) + 1
        )

    stats["nodes_involved"] = sorted(stats["nodes_involved"])
    return stats


def main():
    parser = argparse.ArgumentParser(
        description="Convert tracer trace files to batch scripts",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    %(prog)s trace/example.tr > replay.batch
    %(prog)s -a trace/2-leader.trace -o replay.batch
    %(prog)s -s trace/example.tr  # Show statistics only
        """,
    )

    parser.add_argument("trace_file", help="Path to trace file")
    parser.add_argument("-o", "--output", help="Output batch file (default: stdout)")
    parser.add_argument(
        "-a", "--analyze", action="store_true", help="Show trace analysis"
    )
    parser.add_argument(
        "-s",
        "--stats-only",
        action="store_true",
        help="Only show statistics, don't generate batch",
    )
    parser.add_argument(
        "--no-comments", action="store_true", help="Generate batch without comments"
    )

    args = parser.parse_args()

    trace_path = Path(args.trace_file)
    if not trace_path.exists():
        print(f"Error: Trace file not found: {args.trace_file}", file=sys.stderr)
        sys.exit(1)

    try:
        entries = parse_trace_file(str(trace_path))
    except Exception as e:
        print(f"Error parsing trace file: {e}", file=sys.stderr)
        sys.exit(1)

    if not entries:
        print("Warning: No trace entries found in file", file=sys.stderr)
        sys.exit(0)

    stats = analyze_trace(entries)

    if args.analyze or args.stats_only:
        print(f"Trace Analysis: {args.trace_file}")
        print(f"  Total steps: {stats['total_steps']}")
        print(f"  Nodes involved: {stats['nodes_involved']}")
        print(f"  Choices made: {stats['choices_made']}")
        print(f"  Node distribution:")
        for node_id in sorted(stats["node_distribution"].keys()):
            count = stats["node_distribution"][node_id]
            print(f"    Node {node_id}: {count} steps")
        print()

        if args.stats_only:
            sys.exit(0)

    batch_script = generate_batch_script(entries, not args.no_comments)

    if args.output:
        with open(args.output, "w") as f:
            f.write(batch_script)
        print(f"Batch script written to: {args.output}")
        print(f"  Steps: {stats['total_steps']}")
        print(f"  To replay: run tracer, then 'batch {args.output}'")
    else:
        print(batch_script)


if __name__ == "__main__":
    main()
