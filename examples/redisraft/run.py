#!/usr/bin/env python3
"""Bounded RedisLabs Raft library cases; this does not run the Redis module."""
import argparse
import json
from pathlib import Path
import re
import shlex
import shutil
import sys
import tempfile
import time

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from _support import session as common
from raft.run import integer, leader_clock_ready, saved_budget

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
LIBRARY = ROOT / "examples/redisraft/raft"
SOURCES = ("raft_server.c", "raft_server_properties.c", "raft_node.c", "raft_log.c")
MUTATION = ("if (resp->vote_granted) {\n        if (node) {", "if (1) {\n        if (node) {")


def build_variant(output, variant, cc, *, quorum=False, fixture=None, mutation=None, extra_defines=()):
    """Copy only library inputs, never vendor objects/Makefiles/git metadata."""
    owned = output / variant
    owned.mkdir(parents=True, exist_ok=True)
    sources = owned / "library"
    sources.mkdir(exist_ok=True)
    inputs = {}
    for subdir in ("include", "src"):
        destination = sources / subdir
        destination.mkdir(exist_ok=True)
        for path in sorted((LIBRARY / subdir).iterdir()):
            if path.suffix not in (".c", ".h"):
                continue
            shutil.copy2(path, destination / path.name)
            inputs[str(path.relative_to(LIBRARY))] = common.digest(path)
    server = sources / "src/raft_server.c"
    original = server.read_text()
    mutation = mutation or (("raft_index_t commit = indexes[num_voters / 2];", "raft_index_t commit = indexes[0];") if quorum else MUTATION)
    fixture = Path(fixture) if fixture is not None else HERE / "redisraft_fixture.c"
    common.require(original.count(mutation[0]) == 1, "mutation match must occur exactly once")
    if variant == "mutant":
        server.write_text(original.replace(*mutation))
    manifest = {"library": "RedisLabs/raft", "variant": variant,
        "source_root": str(LIBRARY), "inputs_sha256": inputs,
        "mutation": {"file": "src/raft_server.c", "before": mutation[0],
                     "after": mutation[1], "count": 1} if variant == "mutant" else None,
        "mutated_source_sha256": common.digest(server),
        "driver_sha256": common.digest(fixture),
        "adapter_sha256": common.digest(HERE / "redisraft_fixture.c"),
        "toolchain": common.command(cc + ["--version"], ROOT).stdout,
        "runner_sha256": common.digest(HERE / "run.py"),
        "profile": {"nodes": 3, "commands": ["set alpha 17", "set beta 29"],
                    "raft_compaction": False, "snapshot_backends": ["full", "pages"]}}
    flags = ["-std=gnu11", "-O0", "-g", "-gdwarf-4", "-fno-pie", "-no-pie",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-I" + str(sources / "include")]
    flags.extend(extra_defines)
    if quorum:
        flags.append("-DRAFT_QUORUM_CASE")
    manifest["scenario"] = ("log-repair" if fixture.name == "redisraft_repair_fixture.c"
                            else "replication-quorum" if quorum else "election")
    paths = [str(fixture)] + [str(sources / "src" / name) for name in SOURCES]
    manifest["build_commands"] = []
    for name, defines in (("native-reference", ["-DRAFT_NATIVE_REFERENCE"]), ("raft-case", [])):
        argv = cc + flags + defines + paths + ["-o", str(owned / name)]
        result = common.command(argv, ROOT)
        (owned / (name + "-build.log")).write_text(result.stdout)
        common.require(result.returncode == 0, result.stdout)
        manifest["build_commands"].append(argv)
    native = common.command([owned / "native-reference", variant], owned)
    (owned / "native.log").write_text(native.stdout)
    marker = "NATIVE_EXPECTED_VIOLATION" if variant == "mutant" else "NATIVE_OK"
    common.require(native.returncode == 0 and marker in native.stdout, "native oracle failed: " + native.stdout)
    manifest["native"] = {"exit": native.returncode, "output": native.stdout,
                          "binary_sha256": common.digest(owned / "native-reference")}
    manifest["binary_sha256"] = common.digest(owned / "raft-case")
    (owned / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return owned, manifest


def inspect(session):
    nodes = []
    for node in range(3):
        values = {name: integer(session, node, "((raft_server*)r)->" + name)
                  for name in ("state", "current_term", "voted_for", "commit_idx", "last_applied_idx", "snapshot_last_idx")}
        values["node"] = node
        nodes.append(values)
    return nodes



def committed_commands(session):
    """Decode exact saved application bytes, independent of entry IDs."""
    evidence=[]
    expected=(b"", b"set alpha 17\0", b"set beta 29\0")
    for node in range(3):
        log=integer(session,node,"(long)((raft_server*)r)->log")
        fields={name: integer(session,node,f"(long)((raft_log_t*){log})->{name}")
                for name in ("base","count","size","front","entries")}
        common.require(fields["base"]==0 and fields["count"]==3, "expected uncompacted three-entry log")
        records=[]
        for index, command in enumerate(expected,1):
            slot=(fields["front"]+index-1)%fields["size"]
            pointer=integer(session,node,f"(long)((raft_entry_t**){fields['entries']})[{slot}]")
            term=integer(session,node,f"((raft_entry_t*){pointer})->term")
            kind=integer(session,node,f"((raft_entry_t*){pointer})->type")
            length=integer(session,node,f"((raft_entry_t*){pointer})->data_len")
            common.require(term==1 and kind==(4 if index==1 else 0) and length==len(command),
                           "unexpected committed entry metadata")
            payload=bytes(integer(session,node,f"(long)((raft_entry_t*){pointer})->data[{offset}]") & 255
                          for offset in range(length))
            common.require(payload==command, "saved committed application bytes differ")
            records.append({"index":index,"term":term,"type":kind,"bytes_hex":payload.hex()})
        evidence.append({"node":node,"entries":records})
    return evidence


def expected_mutant_violation(reply, nodes, receipt):
    diagnostic = "redisraft.safety: property violation: multiple leaders in term 1"
    if diagnostic not in reply.splitlines() or not any(
            marker in reply for marker in ("Reach illegal state", "Stopped for illegal state")):
        return False
    by_node = {node.get("node"): node for node in nodes}
    return (len(nodes) == 3 and set(by_node) == {0, 1, 2}
            and all(by_node[node].get("state") == 4
                    and by_node[node].get("current_term") == 1 for node in (0, 1))
            and by_node[2].get("voted_for") == 0
            and receipt.get("count", 0) > 0
            and receipt.get("from") in (0, 2)
            and all(receipt.get(field) == expected for field, expected in (
                ("kind", 2), ("to", 1), ("reply.vote_granted", 0),
                ("reply.prevote", 0), ("reply.request_term", 1))))


def model_case(tracer, binary, checker, choose, directory, backend, variant, timeout, depth, *, prefix_hook=None, prepare_leader=None):
    row = {"target": "redisraft-library", "variant": variant, "backend": backend, "NP": 3,
           "artifacts": str(directory), "scope": "controlled real pre-vote/election prefix and bounded suffix",
           "expected_denials": {"0": "A voted for itself", "2": "C already voted for A"}}
    session = common.Session(tracer, [str(binary)], [checker, choose], directory, backend, timeout,
        tracees=[[str(binary), str(i)] for i in range(3)],
        addresses=["192.168.0.1", "192.168.0.2", "192.168.0.3"],
        choose_points=[{"syscall": 96, "choose": 2}])
    events = []
    steps = 0
    last_reply = [""] * 3
    def value(node, field):
        return integer(session, node, field)
    def step(node, choice=0):
        nonlocal steps
        session.send("sw " + str(node))
        before = session.current_hash()
        reply = session.send("si " + str(choice))
        steps += 1
        last_reply[node] = reply
        common.require("DetSim state/execution error:" not in reply and "Fatal signal" not in reply, reply)
        after = session.current_hash()
        events.append({"node": node, "choice": choice, "predecessor": before,
                       "successor": after, "reply": reply})
        return reply
    def until(node, predicate, choice=0, maximum=60):
        for _ in range(maximum):
            if predicate():
                return
            reply=step(node, choice)
            common.require("Reach illegal state" not in reply, "unexpected prefix violation: " + reply)
        raise common.CaseFailure("bounded prefix failed to reach node " + str(node) + " checkpoint")
    def received(node):
        count=value(node, "received_events")
        if not count:
            return {"count": 0}
        return {"count": count, **{field: value(node, "last_received." + field)
                for field in ("kind", "from", "to", "vote.prevote", "reply.vote_granted", "reply.prevote", "reply.request_term")}}
    try:
        session.send()
        # All sockets bind and all membership initializes before any timeout choice.
        for node in range(3):
            until(node, lambda node=node: value(node, "(void*)r") != 0)
        row["initialized"] = inspect(session)
        # Park all three immediately after an empty receive before A emits traffic.
        # Then B can take its timeout before consuming A's first pre-vote request.
        for node in range(3):
            common.require(leader_clock_ready(step(node)), "initial empty receive checkpoint missing")
        # Real prevote requests/grants precede the term1 candidate elections.
        for node in (0, 1):
            for _ in range(60):
                if value(node, "sent_events") >= 2:
                    break
                reply=step(node, 1 if leader_clock_ready(last_reply[node]) else 0)
                common.require("Reach illegal state" not in reply, "unexpected election violation: " + reply)
            common.require(value(node, "sent_events") >= 2, "pre-vote timeout was not reached")
            common.require(value(node, "((raft_server*)r)->state") == 2,
                           "timeout did not start real pre-vote")
        for received_count, sender in ((1, 0), (2, 1)):
            until(2, lambda: value(2, "received_events") >= received_count and value(2, "sent_events") >= received_count)
            receipt=received(2)
            common.require(receipt["kind"]==1 and receipt["from"]==sender and receipt["vote.prevote"]==1,
                           "C did not receive expected pre-vote request")
        for node in (0, 1):
            until(node, lambda node=node: value(node, "((raft_server*)r)->state") == 3 and value(node, "sent_events") >= 5)
            common.require(value(node, "((raft_server*)r)->current_term") == 1,
                           "pre-vote grant did not produce candidate term1")
        row["candidates"] = inspect(session)
        requests=[]
        for received_count, sender in ((3, 0), (4, 1)):
            until(2, lambda: value(2, "received_events") >= received_count and value(2, "sent_events") >= received_count)
            receipt=received(2); requests.append(receipt)
            common.require(receipt["kind"]==1 and receipt["from"]==sender and receipt["vote.prevote"]==0,
                           "C did not receive expected actual vote request")
        common.require(value(2,"((raft_server*)r)->voted_for") == 0,"C changed its vote")
        row["C_requests"]=requests
        if prepare_leader is not None:
            prepare_leader(step)
        until(0, lambda: value(0, "((raft_server*)r)->state") == 4)
        grant=received(0)
        common.require(grant["kind"]==2 and grant["from"]==2 and grant["reply.vote_granted"]==1
                       and grant["reply.prevote"]==0 and grant["reply.request_term"]==1,
                       "A did not become leader from C actual grant")
        row["A_grant"] = grant
        if prefix_hook is not None:
            row.update(binary_sha256=common.digest(binary), checker_sha256=common.digest(checker),
                       choose_sha256=common.digest(choose), tracer_sha256=common.digest(tracer),
                       bounds={"depth":depth,"wall_seconds":timeout,"address_space_bytes":2*1024**3})
            prefix_hook(session, row, step, until, value, received, events)
            return row
        # Consume only B's stale prevote reply; leave A's real denied reply for search.
        until(1, lambda: value(1, "received_events") >= 3)
        stale=received(1)
        common.require(stale["kind"]==2 and stale["from"]==0 and stale["reply.prevote"]==1,
                       "B did not consume the expected stale prevote")
        prefix=session.current_hash()
        row["prefix"] = prefix
        (directory / "prefix-events.json").write_text(json.dumps(events, indent=2))
        row["search_initial_budget"] = saved_budget(directory, prefix)
        common.require(row["search_initial_budget"] > 0, "prefix exhausted search budget")
        if variant == "mutant":
            # Search actual syscall successors; B's next FIFO packet is A's denied vote.
            seed_before=int(time.time())
            reply=session.send("dfs " + str(depth) + " --quiet")
            row["search_seed_epoch_candidates"] = list(range(seed_before,int(time.time())+1))
            (directory / "search.log").write_text(reply)
            common.require("Reach illegal state" in reply or "Stopped for illegal state" in reply,
                           "bounded mutant search did not find a safety violation: " + reply[-1500:])
            common.require(expected_mutant_violation(reply, inspect(session), received(1)),
                           "searched violation is not the controlled denied-vote counterexample")
            (directory / "witness-history.log").write_text(session.send("info"))
            searched=re.search(r"Searched for (\d+) sys_states \(new: (\d+)\)",reply)
            common.require(searched and int(searched[1])>1, "mutant search did not explore successors")
            row["search_states"] = int(searched[1])
            row["search_new_states"] = int(searched[2])
            history=session.send("info current")
            edges=re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})",history)
            common.require(edges,"counterexample lacks predecessor")
            predecessor,successor=edges[-1]
            # Current history prints actor and choice; infer by replaying at most six legal edges,
            # always restoring the captured predecessor. Only matching property+hash is accepted.
            action=None
            for node in range(3):
                for choice in range(2):
                    session.send("load " + predecessor)
                    session.send("sw " + str(node))
                    attempt=session.send("si " + str(choice))
                    if (session.current_hash()==successor and
                            expected_mutant_violation(attempt, inspect(session), received(1))):
                        action=(node,choice); break
                if action is not None: break
            common.require(action is not None,"cannot reproduce searched violation edge")
            for _ in range(3):
                common.require("Loaded state" in session.send("load " + predecessor),"missing predecessor")
                session.send("sw " + str(action[0]))
                replay=session.send("si " + str(action[1]))
                common.require(session.current_hash()==successor and
                               expected_mutant_violation(replay, inspect(session), received(1)),
                               "violation replay changed expected diagnostic, election evidence or identity")
            row.update(status="violation",search="bounded_counterexample",predecessor=predecessor,
                       successor=successor,replay="same_process_hash_match_3x",action=action,
                       nodes=inspect(session),B_receipt=received(1))
        else:
            # Deliver a real denied vote to B while AppendEntries remain queued.
            until(1,lambda: value(1,"received_events") >= 4)
            denial=received(1)
            common.require(denial["kind"]==2 and denial["from"] in (0,2) and denial["reply.vote_granted"]==0
                           and denial["reply.prevote"]==0 and denial["reply.request_term"]==1,
                           "B did not receive a native-proven denied actual vote")
            common.require(value(1,"((raft_server*)r)->state") == 3,
                           "healthy B incorrectly became leader")
            row["B_denial"]=denial
            completed=False
            for round_number in range(120):
                for node in range(3):
                    # Clock advancement is limited to leader heartbeat opportunities.
                    reply=step(node, 0)
                    common.require("Reach illegal state" not in reply, "healthy safety violation: " + reply)
                if round_number%3==0:
                    nodes=inspect(session)
                    if min(node["commit_idx"] for node in nodes)>=3 and min(node["last_applied_idx"] for node in nodes)>=3:
                        completed=True; break
                # Advance a leader clock explicitly; no follower election timeouts.
                if leader_clock_ready(last_reply[0]):
                    reply=step(0,1)
                    common.require(re.search(r"(?:^|\n)gettimeofday\(", reply),
                                   "leader heartbeat choice did not execute gettimeofday: " + reply)
            common.require(completed,"healthy profile did not commit two commands on all nodes")
            checkpoint=session.current_hash()
            row["committed_commands"] = committed_commands(session)
            initial_budget=saved_budget(directory, checkpoint)
            common.require(initial_budget > 0, "healthy schedule exhausted search budget")
            row["search_initial_budget"] = initial_budget
            # Finite search is separate from the fixed successful business schedule.
            seed_before=int(time.time())
            search=session.send("dfs " + str(depth) + " --quiet")
            row["search_seed_epoch_candidates"] = list(range(seed_before,int(time.time())+1))
            (directory / "search.log").write_text(search)
            (directory / "search-history.log").write_text(session.send("info"))
            common.require("Complete explore all" in search and "illegal state" not in search,
                           "healthy bounded suffix failed: " + search[-1500:])
            searched=re.search(r"Complete explore all (\d+) sys_states", search)
            common.require(searched and int(searched[1]) > 1, "healthy suffix did not explore successors")
            row["search_states"] = int(searched[1])
            row["search_final_budget"] = saved_budget(directory, session.current_hash())
            row["search_budget_cutoff_observed"] = row["search_final_budget"] <= 0
            row["search_scope"] = "bounded suffix under explicit depth and model budget"
            # Replay an observed real final edge three times.
            final=events[-1]
            for _ in range(3):
                session.send("load " + final["predecessor"])
                session.send("sw " + str(final["node"]))
                replay=session.send("si " + str(final["choice"]))
                common.require("Reach illegal state" not in replay and session.current_hash()==final["successor"],
                               "healthy completion replay changed identity")
            row.update(status="pass",search="bounded_suffix_completed",checkpoint=checkpoint,
                       nodes=inspect(session),replay="same_process_hash_match_3x")
        row.update(steps=steps,depth=depth,tracer_sha256=common.digest(tracer),
                   binary_sha256=common.digest(binary),checker_sha256=common.digest(checker),
                   choose_sha256=common.digest(choose), seed="DFS enumerates actor/choice; host seed is time-based",
                   bounds={"depth":depth,"wall_seconds":timeout,"address_space_bytes":2*1024**3},
                   config_sha256=common.digest(directory / "config.json"))
    except Exception as error:
        row.update(common.classify_error(error,session.log))
    finally:
        (directory / "events.json").write_text(json.dumps(events,indent=2))
        row["steps"]=steps
        row["tracer_exit"]=session.close(timeout=30)
        if row["tracer_exit"] != 0 and row.get("status") in ("pass","violation"):
            row.update(status="failed",diagnostic="tracer cleanup failed")
    return row

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/redisraft-library-cases")
    parser.add_argument("--tracer", type=Path)
    parser.add_argument("--checker", type=Path)
    parser.add_argument("--choose", type=Path)
    parser.add_argument("--native-only", action="store_true")
    parser.add_argument("--backend", choices=("full", "pages", "all"), default="all")
    parser.add_argument("--variant", choices=("healthy", "mutant", "all"), default="all")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--cc", default="/usr/bin/gcc")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=args.output.resolve()))
    variants = ("healthy", "mutant") if args.variant == "all" else (args.variant,)
    rows = []
    try:
        choose=args.choose
        checker=args.checker
        if not args.native_only:
            common.require(args.tracer and args.tracer.exists(), "matching NP3 --tracer required")
            if choose is None:
                choose=work / "time-choice.so"
                common.build_plugin(ROOT / "examples/redisraft/plugins/choose.cpp", choose, ["/usr/bin/g++"])
            if checker is None:
                checker=work / "checker.so"
                common.build_plugin(ROOT / "examples/redisraft/plugins/check.cpp", checker, ["/usr/bin/g++"])
        for variant in variants:
            owned, manifest = build_variant(work, variant, shlex.split(args.cc))
            if args.native_only:
                rows.append({"target": "redisraft-library", "variant": variant, "native": "pass",
                             "manifest": str(owned / "manifest.json")})
            else:
                backends=("full","pages") if args.backend=="all" else (args.backend,)
                for backend in backends:
                    row=model_case(args.tracer.resolve(),owned / "raft-case",checker,choose,
                        work / (variant + "-" + backend),backend,variant,args.timeout,args.depth)
                    row.update(native="pass",manifest=str(owned / "manifest.json"))
                    rows.append(row)
        result = {"passed": all(row.get("status", "pass") in ("pass","violation") for row in rows),
                  "scope": "native reference only" if args.native_only else "bounded controlled NP3 cases",
                  "workdir": str(work), "rows": rows}
    except Exception as error:
        result = {"passed": False, "workdir": str(work), "rows": rows, "diagnostic": str(error)}
    (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
