#!/usr/bin/env python3
"""Actual Redis 7.4.2 + RedisRaft module cases, distinct from the Raft library."""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

import run as common

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent / "redisraft_full"
SOURCE=ROOT / "build/workloads/redisraft-full"
SERVER=ROOT / "build/workloads/redis-7.4.2/src/redis-server"
BEFORE="""        case RAFT_LOGTYPE_NORMAL:
            executeLogEntry(rr, entry, entry_idx, req);
            break;"""
AFTER="""        case RAFT_LOGTYPE_NORMAL: {
            static int detsim_skipped_normal;
            if (raft_get_nodeid(raft) == 3 && req == NULL && !detsim_skipped_normal) {
                detsim_skipped_normal = 1;
                break;
            }
            executeLogEntry(rr, entry, entry_idx, req);
            break;
        }"""


def command(argv,directory,log,timeout=120):
    result=common.command(argv,directory,timeout=timeout)
    log.write_text(result.stdout)
    common.require(result.returncode==0,"command failed: "+str(argv)+"\n"+result.stdout[-3000:])
    return {"argv":[str(x) for x in argv],"exit":result.returncode,"log":str(log)}


def build(work,variant):
    owned=work / variant
    owned.mkdir()
    copied=owned / "source"
    shutil.copytree(SOURCE,copied,ignore=shutil.ignore_patterns(".git","build","*.o","*.a","*.so","__pycache__"))
    source=copied / "src/raft.c"
    original=source.read_text()
    common.require(original.count(BEFORE)==1,"fault guard match must occur exactly once")
    if variant=="mutant": source.write_text(original.replace(BEFORE,AFTER))
    inputs={str(p.relative_to(SOURCE)):common.digest(p) for p in sorted(SOURCE.rglob("*"))
            if p.is_file() and p.suffix in (".c",".h") and "build" not in p.relative_to(SOURCE).parts}
    manifest={"variant":variant,"source_commit":"ade4aa8e6aa5c3b21678a1998309825f06567d4f",
        "redis_commit":"a0a6f23d","source_root":str(SOURCE),"input_hashes":inputs,
        "mutation":{"before":BEFORE,"after":AFTER,"file":"src/raft.c","count":1} if variant=="mutant" else None,
        "copied_raft_sha256":common.digest(source),"server_sha256":common.digest(SERVER),
        "compiler":common.command(["/usr/bin/gcc","--version"],ROOT).stdout,"commands":[]}
    manifest["commands"].append(command(["cmake","-S",copied,"-B",owned/"cmake","-DCMAKE_C_COMPILER=/usr/bin/gcc","-DCMAKE_BUILD_TYPE=Debug"],ROOT,owned/"configure.log"))
    manifest["commands"].append(command(["cmake","--build",owned/"cmake","--target","redisraft","-j2"],ROOT,owned/"build.log"))
    manifest["module_sha256"]=common.digest(copied/"redisraft.so")
    (owned/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    return copied/"redisraft.so",owned/"manifest.json"


def server_argv(module,node,address,port,log):
    return [str(SERVER),"--port",str(port),"--bind",address,"--protected-mode","no","--daemonize","no","--save","","--appendonly","no",
        "--disable-thp","no","--loadmodule",str(module),"--raft.id",str(node),"--raft.addr",f"{address}:{port}",
        "--raft.log-filename",str(log),"--raft.log-max-file-size","0","--raft.snapshot-disable","yes"]


def observations(text):
    lines=[json.loads(line) for line in text.splitlines()]
    common.require(lines and all(row.get("v")==1 for row in lines),"invalid client observation schema")
    return lines


def native_result(records,returncode,variant):
    acks=[item for item in records if item["event"]=="ack"]
    common.require([(x["key"],x["value"]) for x in acks]==[("alpha","17"),("beta","29")],"native lacks two exact SET ACKs")
    progress={x["node"]:x for x in records if x["event"]=="progress"}
    common.require(set(progress)=={1,2,3} and all(x["applied"]>=acks[-1]["index"] and x["commit"]>=acks[-1]["index"] for x in progress.values()),"native missing applied/commit checkpoint")
    local=[x for x in records if x["event"]=="local"]
    expected={(node,key) for node in (1,2,3) for key in ("alpha","beta")}
    cells={(x["node"],x["key"]) for x in local}
    common.require(len(local)==len(expected) and cells==expected,"native local inspection requires six unique exact cells")
    common.require(all(type(x["present"]) is bool and type(x["matches"]) is bool for x in local),"native local flags must be boolean")
    missing=(3,"alpha") if variant=="mutant" else None
    common.require(all(x["present"]==((x["node"],x["key"])!=missing) and
                       x["matches"]==((x["node"],x["key"])!=missing) for x in local),"native local values differ from exact fixture")
    if variant=="healthy": common.require(returncode==0 and records[-1]["event"]=="done","healthy native property failed")
    else: common.require(returncode==42 and records[-1]["event"]=="violation","mutant did not hit exact skipped local apply")


def native(work,module,client,variant,timeout,checker_test=None):
    directory=work / (variant+"-native")
    directory.mkdir()
    row={"target":"redisraft-full","variant":variant,"backend":"native","NP":4,"status":"failed","artifacts":str(directory)}
    owned=[]
    try:
        reservations=[]
        try:
            for _ in range(3):
                reservation=socket.socket();reservation.bind(("127.0.0.1",0));reservations.append(reservation)
            ports=[reservation.getsockname()[1] for reservation in reservations]
            common.require(len(set(ports))==3,"native server ports must be distinct")
        finally:
            for reservation in reservations: reservation.close()
        for node in range(1,4):
            place=directory/str(node);place.mkdir()
            argv=server_argv(module,node,"127.0.0.1",ports[node-1],place/"raftlog.db")
            output=(place/"server.log").open("w")
            process=subprocess.Popen(argv,cwd=place,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
            owned.append((process,output,argv,place))
        deadline=time.monotonic()+timeout
        while not all("Ready to accept connections" in (place/"server.log").read_text(errors="replace") for _,_,_,place in owned):
            common.require(time.monotonic()<deadline,"native server startup timeout")
            common.require(all(p.poll() is None for p,_,_,_ in owned),"native Redis startup exited")
        argv=[str(client)]+[part for port in ports for part in ("127.0.0.1",str(port))]
        result=common.command(argv,directory,timeout=timeout)
        (directory/"client.log").write_text(result.stdout)
        records=observations(result.stdout)
        native_result(records,result.returncode,variant)
        common.require(checker_test is not None,"native saved-log checker executable required")
        checked=common.command([checker_test,"--native",directory/"client.log"]+[entry[3]/"raftlog.db" for entry in owned],directory)
        (directory/"checker-native.log").write_text(checked.stdout)
        expected=0 if variant=="healthy" else 1
        common.require(checked.returncode==expected,"native saved-log checker verdict differs: "+checked.stdout)
        if variant=="mutant": common.require("acknowledged local value missing at node 3 key alpha" in checked.stdout,"native checker hit unintended property")
        configuration={"servers":[entry[2] for entry in owned],"client":argv,"checker":{"logs":[str(entry[3]/"raftlog.db") for entry in owned]},"environment":common.ENV}
        (directory/"native-config.json").write_text(json.dumps(configuration,indent=2)+"\n")
        row.update(status="pass" if variant=="healthy" else "violation",client_exit=result.returncode,
            checker_exit=checked.returncode,checker_output=checked.stdout,config_sha256=common.digest(directory/"native-config.json"),
            observations=records,client_argv=argv,server_commands=[entry[2] for entry in owned],
            logs=[str(entry[3]/"raftlog.db") for entry in owned])
    except Exception as error:
        row.update(status="failed",diagnostic=str(error))
    finally:
        for process,_,_,_ in owned:
            if process.poll() is None: os.killpg(process.pid,signal.SIGTERM)
        codes=[]
        for process,output,_,_ in owned:
            try: codes.append(process.wait(timeout=10))
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL);codes.append(process.wait())
            output.close()
        row["server_exits"]=codes
        row["cleanup"]="pass" if len(codes)==3 and all(code==0 for code in codes) else "failed"
        if row["cleanup"]!="pass":
            row["status"]="failed"
            row.setdefault("diagnostic","native cleanup failed; server exits="+str(codes))
    (directory/"result.json").write_text(json.dumps(row,indent=2)+"\n")
    return row


class FullSession(common.Session):
    """Use the existing CLI lifecycle with this case's configured public checker."""
    def __init__(self,tracer,tracees,checker,logs,directory,backend,timeout):
        self.directory=directory;directory.mkdir(parents=True)
        self.owned_pids=set()
        self.timeout=timeout;self.started=time.monotonic();self.offset=0;self.commands=[]
        self.log=directory/"transcript.log"
        configuration={"Nodes":4,"Loglevel":4,"Tracee":tracees,
            "Addr":["192.168.0.1","192.168.0.2","192.168.0.3","192.168.0.4"],
            "RuntimeProfile":{"startup_syscall":11},
            "Plugins":[{"Path":str(checker),"Configuration":{"logs":logs}}],
            "FSMap":[{"Host":str(path),"Target":str(path)} for path in
                     (Path(tracees[0][tracees[0].index("--loadmodule")+1]),
                      Path("/usr/lib/x86_64-linux-gnu/libm.so.6"),Path("/usr/lib/x86_64-linux-gnu/libc.so.6"))],
            "StateStore":{"hot_cache_mb":32,"warm_cache_mb":64}}
        (directory/"config.json").write_text(json.dumps(configuration,indent=2)+"\n")
        self.output=self.log.open("wb")
        self.proc=subprocess.Popen([str(tracer),"-n","-c",str(directory/"config.json")],cwd=directory,
            stdin=subprocess.PIPE,stdout=self.output,stderr=subprocess.STDOUT,start_new_session=True,
            env=os.environ|common.ENV|{"DETSIM_SNAPSHOT_BACKEND":backend},preexec_fn=common.limits)


    def current_hash(self):
        reply=self.send("info current")
        matches=re.findall(r"([0-9a-f]{16}) => ([0-9a-f]{16})",reply)
        if matches:return matches[-1][1]
        initial=re.search(r"Current state ([0-9a-f]{16})",reply)
        common.require(initial,"current saved state hash unavailable: "+reply)
        return initial[1]


class ClientAssertion(common.CaseFailure):
    """A workload assertion is a failed case, before abort's later syscalls."""


def control_step(session,node,slot,events,last,observed):
    session.send("sw "+str(node))
    if slot is not None:
        selected=session.send("thread "+str(slot))
        common.require("Switched to thread "+str(slot)+" " in selected,"logical thread selection failed: "+selected)
    item={"node":node,"thread":0 if slot is None else slot,"choice":0,"predecessor":session.current_hash(),
          "executed":False,"status":"attempted"}
    events.append(item)
    try:reply=session.send("si 0")
    except Exception as error:
        item["reply"]=str(error);item["status"]="error";last[node]=str(error)
        if node==3 and re.search(r"\bAssertion\b|assertion failed",str(error),re.IGNORECASE):
            raise ClientAssertion("client assertion during controlled execution: "+str(error)) from error
        raise
    item["reply"]=reply;last[node]=reply
    if "DetSim state/execution error:" in reply or "Fatal signal" in reply:
        item["status"]="error"
        raise common.CaseFailure(reply)
    after=session.current_hash()
    if re.search(r"Thread \d+ is blocked \(pending syscall\)\. Cannot step\.",reply):
        item.update(status="blocked",observed_hash=after)
        common.require(after==item["predecessor"],"blocked attempt changed state hash: "+reply)
        return reply
    item.update(status="completed",executed=True,successor=after)
    if node==3 and re.search(r"\bAssertion\b|assertion failed",reply,re.IGNORECASE):
        raise ClientAssertion("client assertion during controlled execution: "+reply)
    common.require("property violation:" not in reply,"unexpected initialization property violation: "+reply)
    if node==3:
        for match in re.finditer(r'write\(1, [^"\n]+"(\{"v":1,[^\n]*\})\\010", \d+\) = \d+',reply):
            observed.append(json.loads(match[1]))
    return reply


def actor_round(session,events,last,observed):
    """Step each current logical slot once; refresh births on the next round."""
    for node in range(4):
        session.send("sw "+str(node))
        reply=session.send("thread")
        common.require(re.search(r"Threads in process "+str(node)+r" \(pid=\d+\):",reply),"thread listing unavailable: "+reply)
        slots=[int(slot) for slot in re.findall(r"^\s*\*?\s*\[(\d+)\] TID \d+[^\n]*$",reply,re.MULTILINE)]
        if not slots and "(single-threaded, main thread only)" in reply:
            slots=[None] # The empty-table main actor has no selectable thread slot.
        else:
            common.require(slots and slots==list(range(len(slots))),"invalid logical thread slots: "+reply)
        for slot in slots:
            control_step(session,node,slot,events,last,observed)
            if observed and observed[-1].get("event")=="before_set":return


def model_control(work,tracer,module,client,checker,variant,backend,timeout,max_steps):
    """Reach the pre-SET/fault prefix by real requests; never search startup."""
    directory=work/(variant+"-"+backend)
    logs=["/node"+str(node)+"-raftlog.db" for node in range(1,4)]
    tracees=[server_argv(module,node,"192.168.0."+str(node),6379,logs[node-1]) for node in range(1,4)]
    tracees.append([str(client)]+[part for node in range(1,4) for part in ("192.168.0."+str(node),"6379")])
    row={"target":"redisraft-full","variant":variant,"backend":backend,"NP":4,
         "status":"incomplete","artifacts":str(directory),"scope":"hand-controlled initialization to pre-SET fault prefix",
         "bounds":{"control_steps":max_steps,"wall_seconds":timeout,"address_space_bytes":common.MODEL_ADDRESS_SPACE_LIMIT},
         "tracer_sha256":common.digest(tracer),"module_sha256":common.digest(module),
         "client_sha256":common.digest(client),"checker_sha256":common.digest(checker),"server_sha256":common.digest(SERVER)}
    session=None;events=[];observed=[];phase="startup";last=[""]*4
    try:
        session=FullSession(tracer,tracees,checker,logs,directory,backend,timeout)
        session.send()
        children=Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children")
        session.owned_pids={int(pid) for pid in children.read_text().split()}
        common.require(len(session.owned_pids)>=4,"NP4 owned process inventory incomplete")
        row["owned_tracee_pids"]=sorted(session.owned_pids)
        for node in range(3):
            phase="initialize_server_"+str(node+1)
            for _ in range(max_steps):
                if "Ready to accept connections" in control_step(session,node,None,events,last,observed):break
            else: raise common.CaseFailure("server initialization step bound before ready: "+str(node+1))
        phase="client_INIT_JOIN_membership"
        stable=0
        for _ in range(max_steps):
            before=session.current_hash()
            actor_round(session,events,last,observed)
            if observed and observed[-1].get("event")=="before_set":
                common.require(observed[-1].get("key")=="alpha","first fault prefix is not alpha SET")
                row["prefix"]=session.current_hash();row["observations"]=observed
                (directory/"prefix-history.log").write_text(session.send("info"))
                row["diagnostic"]="pre-SET fault prefix reached; bounded fault-event search/replay remains required"
                break
            stable=stable+1 if session.current_hash()==before else 0
            if stable>=16:raise common.CaseFailure("real request pump made no successor progress; pending timers are not auto-expired")
        else:raise common.CaseFailure("membership/fault-prefix control step bound exceeded")
    except Exception as error:
        row.update({"status":"failed","diagnostic":str(error)} if isinstance(error,ClientAssertion)
                   else common.classify_error(error,session.log if session else None))
        row["failure_phase"]=phase
        row["last_successful_hash"]=events[-1].get("predecessor") if events else None
        row["last_replies"]=last
    finally:
        if session:
            (directory/"control-events.json").write_text(json.dumps(events,indent=2)+"\n")
            (directory/"observations.json").write_text(json.dumps(observed,indent=2)+"\n")
            row["config_sha256"]=common.digest(directory/"config.json")
            row["tracer_exit"]=session.close(timeout=30)
            remaining=[pid for pid in session.owned_pids if Path(f"/proc/{pid}").exists()]
            row["remaining_owned_pids"]=remaining
            row["cleanup"]="failed" if remaining else "pass"
            if remaining:row.update(status="failed",cleanup_diagnostic="owned tracees remain after cleanup: "+str(remaining))
            if row["tracer_exit"]!=0 and row["status"]=="pass":row.update(status="failed",diagnostic="tracer exited nonzero")
    return row


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,default=ROOT/"build/redisraft-full-cases")
    parser.add_argument("--variant",choices=("healthy","mutant","all"),default="all")
    parser.add_argument("--native-only",action="store_true")
    parser.add_argument("--tracer",type=Path)
    parser.add_argument("--timeout",type=float,default=180)
    parser.add_argument("--backend",choices=("full","pages","all"),default="all")
    parser.add_argument("--max-control-steps",type=int,default=1500)
    parser.add_argument("--reuse-modules",type=Path)
    parser.add_argument("--address-space-mb",type=int,default=4096)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix="run-",dir=args.output.resolve()))
    rows=[]
    report={"workdir":str(work),"passed":False,"rows":rows,"model_matrix_complete":False,
        "scope":"native-only actual Redis+loaded module" if args.native_only else "native plus hand-controlled NP4 prefix; full model matrix incomplete"}
    try:
        client=work/"client"
        checker_test=work/"checker-test"
        checker=work/"checker.so"
        command(["/usr/bin/g++","-std=c++20","-O2","-g","-Wall","-Wextra","-Werror","-I"+str(ROOT/"src"),"-I"+str(ROOT/"third_party"),HERE/"checker_test.cpp","-o",checker_test],ROOT,work/"checker-test-build.log")
        command([checker_test],ROOT,work/"checker-test.log")
        command(["/usr/bin/g++","-std=c++20","-O2","-g","-Wall","-Wextra","-Werror","-fPIC","-shared","-I"+str(ROOT/"src"),"-I"+str(ROOT/"third_party"),HERE/"checker.cpp","-o",checker],ROOT,work/"checker-build.log")
        command(["/usr/bin/gcc","-std=gnu11","-O0","-g","-gdwarf-4","-fno-pie","-no-pie","-Wall","-Wextra","-Werror",HERE/"client.c","-o",client],ROOT,work/"client-build.log")
        for variant in (("healthy","mutant") if args.variant=="all" else (args.variant,)):
            if args.reuse_modules:
                manifest=args.reuse_modules.resolve()/variant/"manifest.json"
                module=manifest.parent/"source/redisraft.so"
                common.require(module.exists() and common.digest(module)==json.loads(manifest.read_text())["module_sha256"],"reused module does not match manifest")
            else:module,manifest=build(work,variant)
            row=native(work,module,client,variant,args.timeout,checker_test)
            row.update(manifest=str(manifest),client_sha256=common.digest(client),module_sha256=common.digest(module),server_sha256=common.digest(SERVER),
                checker_sha256=common.digest(checker),checker_test_sha256=common.digest(checker_test),runner_sha256=common.digest(Path(__file__)),
                sources={name:common.digest(HERE/name) for name in ("client.c","checker.cpp","checker_test.cpp")})
            rows.append(row)
            if not args.native_only:
                common.require(args.tracer and args.tracer.exists(),"matching immutable NP4 --tracer required")
                common.require(row["status"] in ("pass","violation"),"native prerequisite failed")
                common.MODEL_ADDRESS_SPACE_LIMIT=args.address_space_mb*1024**2
                for backend in (("full","pages") if args.backend=="all" else (args.backend,)):
                    rows.append(model_control(work,args.tracer.resolve(),module,client,checker,variant,backend,args.timeout,args.max_control_steps))
        report["passed"]=bool(rows) and all(row["status"] in ("pass","violation") for row in rows)
    except Exception as error: report["diagnostic"]=str(error)
    (work/"result.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report,indent=2))
    return 0 if report["passed"] else 1

if __name__=="__main__": sys.exit(main())
