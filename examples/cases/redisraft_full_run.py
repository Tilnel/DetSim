#!/usr/bin/env python3
"""Actual Redis 7.4.2 + RedisRaft module cases, distinct from the Raft library."""
import argparse
import copy
from contextlib import contextmanager
import math
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
PREFIX_TIMEOUT_POLICY="fresh post-normal eligibility; locally idle node RR; earliest saved deadline then slot within node"
CLOCK_PROFILE={"id":"cases.redisraft-full.clock-input","syscall":96,"normal_choices":[0,1],
               "advance_microseconds":10000,"startup_choice":0,"business_choice":1,"prefix_timeout_policy":PREFIX_TIMEOUT_POLICY}
GUIDE_POLICY="RR(node,slot) among normals and finite timeouts on locally idle nodes; normal preset1 when configured else0; all siblings checked; variant/property independent"


def time_policy_context(plugin):
    return copy.deepcopy(CLOCK_PROFILE)|{"plugin_sha256":common.digest(plugin),"source_sha256":common.digest(HERE/"time_choice.cpp")}


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


def native(work,module,client,variant,timeout,checker_test=None,time_policy=None):
    directory=work / (variant+"-native")
    directory.mkdir()
    row={"target":"redisraft-full","variant":variant,"backend":"native","NP":4,"status":"failed","artifacts":str(directory),"time_policy":time_policy}
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
        configuration={"servers":[entry[2] for entry in owned],"client":argv,"checker":{"logs":[str(entry[3]/"raftlog.db") for entry in owned]},"environment":common.ENV,"model_time_policy":time_policy}
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
    def __init__(self,tracer,tracees,checker,logs,directory,backend,timeout,time_plugin=None):
        self.directory=directory;directory.mkdir(parents=True)
        self.owned_pids=set();self.normal_choices=(0,1);self.timeout_node_cursor=None
        time_plugin=time_plugin or directory.parent/"time-choice.so"
        self.timeout=timeout;self.started=time.monotonic();self.offset=0;self.commands=[]
        self.log=directory/"transcript.log"
        configuration={"Nodes":4,"Loglevel":4,"Tracee":tracees,
            "Addr":["192.168.0.1","192.168.0.2","192.168.0.3","192.168.0.4"],
            "RuntimeProfile":{"startup_syscall":11},
            "Plugins":[{"Path":str(checker),"Configuration":{"logs":logs}},{"Path":str(time_plugin)}],
            "ChoosePoint":[{"syscall":96,"choose":2}],
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


def configured_normal_choices(session):
    choices=tuple(getattr(session,"normal_choices",(0,)))
    common.require(choices in ((0,),(0,1)) and all(type(choice) is int for choice in choices),"unknown normal-choice profile")
    return choices


def control_step(session,node,slot,events,last,observed,choice=0,allow_violation=False):
    common.require(choice==-2 or choice in configured_normal_choices(session),"unknown controller action")
    session.send("sw "+str(node))
    if slot is not None:
        selected=session.send("thread "+str(slot))
        common.require("Switched to thread "+str(slot)+" " in selected,"logical thread selection failed: "+selected)
    item={"node":node,"thread":0 if slot is None else slot,"choice":choice,"predecessor":session.current_hash(),
          "executed":False,"status":"attempted"}
    events.append(item)
    try:reply=session.send("si timeout" if choice==-2 else "si "+str(choice))
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
    if choice==-2:
        item.update(status="error",observed_hash=after)
        common.require("Timeout completed." in reply and after!=item["predecessor"],
                       "timeout action rejected or unconfirmed: "+reply)
    if re.search(r"Thread \d+ is blocked \(pending syscall\)\. Cannot step\.",reply):
        item.update(status="blocked",observed_hash=after)
        common.require(after==item["predecessor"],"blocked attempt changed state hash: "+reply)
        return reply
    item.update(status="completed",executed=True,successor=after)
    if node==3 and re.search(r"\bAssertion\b|assertion failed",reply,re.IGNORECASE):
        raise ClientAssertion("client assertion during controlled execution: "+reply)
    violations=re.findall(r"([^\n]*): property violation: ([^\n]+)",reply)
    item["verdict"]="violation" if violations else "pass"
    item["diagnostic"]=violations[-1][1] if violations else ""
    common.require("Reach illegal state" not in reply or violations,"illegal state lacks product diagnostic: "+reply)
    common.require(not violations or allow_violation,"unexpected initialization property violation: "+reply)
    if violations:
        common.require(len(violations)==1 and violations[0][0].strip().endswith("cases.redisraft-full"),
                       "unexpected product property: "+reply)
    if node==3:
        for match in re.finditer(r'write\(1, [^"\n]+"(\{"v":1,[^\n]*\})\\010", \d+\) = \d+',reply):
            observed.append(json.loads(match[1]))
    return reply


def thread_actors(session,node):
    """Read the current saved slots, states and optional absolute deadlines."""
    session.send("sw "+str(node))
    reply=session.send("thread")
    common.require(re.search(r"Threads in process "+str(node)+r" \(pid=\d+\):",reply),"thread listing unavailable: "+reply)
    rows=[line for line in reply.splitlines() if re.match(r"\s*\*?\s*\[",line)]
    actors=[]
    for line in rows:
        match=re.fullmatch(r"\s*\*?\s*\[(\d+)\] TID \d+ state=(runnable|waiting|exited)"
                           r"(?: \(main\))?(?:\s+deadline=([^\s]+))?\s*",line)
        common.require(match,"missing/unknown thread state or deadline; matching tracer required: "+line)
        deadline=None
        if match[3] is not None:
            parsed=re.fullmatch(r"(0|[1-9][0-9]{0,9})\.([0-9]{9})",match[3])
            common.require(parsed and match[2]=="waiting","invalid saved deadline: "+line)
            deadline=(int(parsed[1]),int(parsed[2]))
            common.require(deadline[0]*1000000000+deadline[1]<=2**63-1,"saved deadline exceeds ktime range: "+line)
        actors.append((int(match[1]),match[2],deadline))
    single=[line for line in reply.splitlines() if "(single-threaded, main thread only)" in line]
    if not actors and single:
        common.require(len(single)==1 and re.fullmatch(r"\s*\(single-threaded, main thread only\) state=runnable\s*",single[0]),
                       "invalid empty-table thread state or deadline: "+reply)
        return [(None,"runnable",None)] # Empty-table main actor has no selectable slot.
    common.require(actors and [slot for slot,_,_ in actors]==list(range(len(actors))),
                   "invalid logical thread slots/state; matching tracer required: "+reply)
    return actors


def actor_round(session,events,last,observed):
    """Run normal slots, then one timeout on a freshly observed locally idle node."""
    start=len(events)
    for node in range(4):
        for slot,state,_ in thread_actors(session,node):
            if state!="runnable":continue
            control_step(session,node,slot,events,last,observed,choice=max(configured_normal_choices(session)))
            if observed and observed[-1].get("event")=="before_set":return
    progressed=any(item["executed"] for item in events[start:])
    # Always re-read after normals: a peer may have made a waiting actor ready.
    current=[(node,slot,state,deadline) for node in range(4)
             for slot,state,deadline in thread_actors(session,node)]
    runnable_nodes={node for node,_,state,_ in current if state=="runnable"}
    candidates=[(deadline,node,slot) for node,slot,state,deadline in current
                if state=="waiting" and deadline is not None and node not in runnable_nodes]
    if not candidates:
        common.require(progressed or runnable_nodes,"no runnable actor or finite saved deadline; real request pump cannot progress")
        return
    nodes=sorted({node for _,node,_ in candidates})
    cursor=getattr(session,"timeout_node_cursor",None)
    node=next((node for node in nodes if cursor is None or node>cursor),nodes[0])
    deadline,node,slot=min(candidate for candidate in candidates if candidate[1]==node)
    control_step(session,node,slot,events,last,observed,choice=-2)
    events[-1]["deadline"]="%d.%09d"%deadline
    session.timeout_node_cursor=node


def check_search_deadline(deadline):
    if time.monotonic()>=deadline:raise TimeoutError("guided search wall budget")


@contextmanager
def bounded_search_commands(session,deadline):
    """Bound this case's CLI waits and check completion time after every command."""
    original={name:(name in vars(session),getattr(session,name)) for name in ("send","current_hash")}
    original_timeout=getattr(session,"timeout",None)
    def bounded(call):
        def invoke(*args,**kwargs):
            check_search_deadline(deadline)
            reply=call(*args,**kwargs)
            check_search_deadline(deadline)
            return reply
        return invoke
    try:
        for name,(_,call) in original.items():setattr(session,name,bounded(call))
        if original_timeout is not None and hasattr(session,"started"):
            session.timeout=min(original_timeout,max(0,deadline-session.started))
        yield
    finally:
        for name,(owned,value) in original.items():
            if owned:setattr(session,name,value)
            else:delattr(session,name)
        if original_timeout is not None:session.timeout=original_timeout


def restore_source(session,source):
    reply=session.send("load "+source)
    common.require("Loaded state" in reply and session.current_hash()==source,"search source restore failed: "+reply)


def search_actions(session,process_count=4):
    actions=[]
    for node in range(process_count):
        for slot,state,deadline in thread_actors(session,node):
            if state=="runnable":actions.extend((node,slot,choice) for choice in configured_normal_choices(session))
            elif state=="waiting" and deadline is not None:actions.append((node,slot,-2))
    return sorted(actions)


def search_layer(session,source,driver,events,process_count=4,deadline=float("inf")):
    check_search_deadline(deadline)
    restore_source(session,source)
    check_search_deadline(deadline)
    actions=search_actions(session,process_count)
    check_search_deadline(deadline)
    candidates=[]
    for node,slot,choice in actions:
        check_search_deadline(deadline)
        restore_source(session,source)
        branch=copy.deepcopy(driver);count=len(branch["observations"])
        control_step(session,node,slot,events,[""]*max(4,process_count),branch["observations"],choice,allow_violation=True)
        check_search_deadline(deadline)
        edge=events[-1]
        common.require(edge["executed"] and edge["predecessor"]==source,"enumerated actor did not execute from source")
        edge["observations"]=copy.deepcopy(branch["observations"][count:])
        if node==3 and choice>=0:
            exited=re.search(r"\bexit_group\((\d+)\)",edge["reply"])
            if exited:branch["client_exit"]=int(exited[1])
        edge["client_exit"]=branch["client_exit"]
        candidates.append({"action":(node,slot,choice),"edge":edge,"driver":branch})
    check_search_deadline(deadline)
    return candidates


def model_outcome(driver,variant,verdict,diagnostic):
    common.require(verdict in ("pass","violation"),"unknown product verdict")
    records=driver["observations"]
    if verdict=="violation":
        common.require(variant=="mutant" and diagnostic=="acknowledged local value missing at node 3 key alpha",
                       "unexpected property violation: "+diagnostic)
        acks=[x for x in records if x["event"]=="ack"]
        common.require([(x["key"],x["value"]) for x in acks]==[("alpha","17"),("beta","29")] and
                       0<acks[0]["index"]<acks[1]["index"],"model violation lacks exact ACKs")
        progress={x["node"]:x for x in records if x["event"]=="progress"}
        common.require(set(progress)=={1,2,3} and all(x["commit"]>=acks[1]["index"] and
                       x["applied"]>=acks[1]["index"] for x in progress.values()),"model violation lacks applied checkpoints")
        local=[x for x in records if x["event"]=="local"]
        expected={(node,key) for node in (1,2) for key in ("alpha","beta")}|{(3,"alpha")}
        common.require(len(local)==5 and {(x["node"],x["key"]) for x in local}==expected,
                       "model terminal violation lacks exact local observation prefix")
        common.require(all(type(x["present"]) is bool and type(x["matches"]) is bool and
                       x["present"]==((x["node"],x["key"])!=(3,"alpha")) and x["matches"]==x["present"]
                       for x in local),"model terminal violation has another local failure")
        return "violation"
    if driver["client_exit"] is not None:
        common.require(variant=="healthy" and driver["client_exit"]==0,"client exited without matching product violation")
        native_result(records,0,"healthy")
        return "pass"
    return None


def replay_candidate(session,source,driver,candidate,variant,results=None,deadline=float("inf")):
    if results is None:results=[]
    for _ in range(3):
        check_search_deadline(deadline)
        restore_source(session,source)
        replay_driver=copy.deepcopy(driver);events=[]
        node,slot,choice=candidate["action"]
        control_step(session,node,slot,events,[""]*4,replay_driver["observations"],choice,allow_violation=True)
        check_search_deadline(deadline)
        event=events[-1]
        if node==3 and choice>=0:
            exited=re.search(r"\bexit_group\((\d+)\)",event["reply"])
            if exited:replay_driver["client_exit"]=int(exited[1])
        expected=candidate["edge"]
        common.require(event["executed"] and event.get("successor")==expected["successor"] and
                       event["verdict"]==expected["verdict"] and event["diagnostic"]==expected["diagnostic"] and
                       replay_driver==candidate["driver"],"terminal edge replay changed hash/property/observations/exit")
        outcome=model_outcome(replay_driver,variant,event["verdict"],event["diagnostic"])
        common.require(outcome in ("pass","violation"),"terminal replay lacks matching outcome")
        results.append({"source":source,"action":list(candidate["action"]),"target":event["successor"],
                        "verdict":event["verdict"],"diagnostic":event["diagnostic"],
                        "observations":replay_driver["observations"],"client_exit":replay_driver["client_exit"]})
    check_search_deadline(deadline)
    return results


def select_continuation(candidates,cursor):
    runnable_nodes={candidate["action"][0] for candidate in candidates if candidate["action"][2]>=0}
    pool=[candidate for candidate in candidates if candidate["action"][2]>=0 or candidate["action"][0] not in runnable_nodes]
    actors=sorted({candidate["action"][:2] for candidate in pool})
    actor=next((actor for actor in actors if cursor is None or actor>cursor),actors[0])
    return max((candidate for candidate in pool if candidate["action"][:2]==actor),key=lambda candidate:candidate["action"][2])


def guided_search(session,root,driver,variant,max_depth,wall_seconds):
    common.require(max_depth>0 and math.isfinite(wall_seconds) and wall_seconds>0,"positive finite guided search bounds required")
    started=time.monotonic()
    result={"status":"incomplete","termination":"depth_cutoff","root":root,"root_observations":copy.deepcopy(driver["observations"]),"beam_width":1,"seed":0,
            "wall_cutoff":False,
            "bounds":{"depth":max_depth,"wall_seconds":wall_seconds},"expanded_layers":0,"attempted_edges":0,
            "unique_successors":0,"duplicate_successor_edges":0,"pruned_successors":0,"partial_last_layer":False,"branching_observed":False,
            "selection":GUIDE_POLICY,"normal_choices":list(configured_normal_choices(session)),
            "cursor":None,"edges":[],"path":[],"replays":[],"replay_verified":False}
    source=root;source_driver=copy.deepcopy(driver);cursor=None;visited={root}
    deadline=min(started+wall_seconds,getattr(session,"started",float("inf"))+getattr(session,"timeout",0))
    result["bounds"]["effective_wall_seconds"]=max(0,deadline-started)
    try:
        with bounded_search_commands(session,deadline):
            for layer in range(max_depth):
                start=len(result["edges"]);result["partial_last_layer"]=True
                candidates=search_layer(session,source,source_driver,result["edges"],deadline=deadline)
                result["partial_last_layer"]=False;result["expanded_layers"]+=1
                if not candidates:
                    result["termination"]="no_legal_actions";break
                successors={x["edge"]["successor"] for x in candidates}
                result["duplicate_successor_edges"]+=len(candidates)-len(successors)
                result["branching_observed"]|=len(successors)>=2
                terminal=[]
                for candidate in candidates:
                    outcome=model_outcome(candidate["driver"],variant,candidate["edge"]["verdict"],candidate["edge"]["diagnostic"])
                    candidate["edge"]["layer"]=layer
                    candidate["edge"]["outcome"]=outcome
                    if outcome:terminal.append((candidate,outcome))
                if terminal:
                    candidate,outcome=terminal[0]
                    result["witness"]={"source":source,"action":list(candidate["action"]),"target":candidate["edge"]["successor"],
                                       "diagnostic":candidate["edge"]["diagnostic"],"driver":candidate["driver"]}
                    candidate["edge"]["selected"]=True
                    result["path"].append({"source":source,"action":list(candidate["action"]),"target":candidate["edge"]["successor"],
                                           "edge_index":start+candidates.index(candidate)})
                    result["pruned_successors"]+=len(successors-{candidate["edge"]["successor"]})
                    if not result["branching_observed"]:
                        result["termination"]="degenerate_single_path";break
                    replay_candidate(session,source,source_driver,candidate,variant,result["replays"],deadline)
                    check_search_deadline(deadline)
                    result.update(status=outcome,termination="healthy_completion" if outcome=="pass" else "exact_violation",replay_verified=True)
                    break
                candidate=select_continuation(candidates,cursor)
                result["pruned_successors"]+=len(successors-{candidate["edge"]["successor"]})
                cursor=candidate["action"][:2];result["cursor"]=list(cursor)
                candidate["edge"]["selected"]=True
                result["path"].append({"source":source,"action":list(candidate["action"]),"target":candidate["edge"]["successor"],"edge_index":start+candidates.index(candidate)})
                source=candidate["edge"]["successor"];source_driver=candidate["driver"]
                if source in visited:
                    result["termination"]="retained_path_cycle";break
                visited.add(source)
    except (TimeoutError,subprocess.TimeoutExpired) as error:
        result.update(status="timeout",termination="wall_cutoff",wall_cutoff=True,replay_verified=False,diagnostic=str(error))
    except Exception as error:
        result.update(status="failed",termination="error",diagnostic=str(error))
    result["attempted_edges"]=len(result["edges"])
    result["unique_successors"]=len({edge["successor"] for edge in result["edges"] if "successor" in edge})
    result["elapsed_seconds"]=max(0,time.monotonic()-started)
    if result["elapsed_seconds"]>=result["bounds"]["effective_wall_seconds"]:
        result.update(status="timeout",termination="wall_cutoff",wall_cutoff=True,replay_verified=False,
                      diagnostic="guided search wall budget exhausted before final acceptance")
    return result


def matrix_acceptance(rows,variants,backends,native_only=False):
    def digest_ok(value,width=64):
        return type(value) is str and re.fullmatch(r"[0-9a-f]{%d}"%width,value) is not None
    source_fields={"client.c","checker.cpp","checker_test.cpp","time_choice.cpp","time_choice_test.cpp","common_runner.py"}
    expected={(variant,"native") for variant in variants}
    if not native_only:expected|={(variant,backend) for variant in variants for backend in backends}
    keys=[(row["variant"],row["backend"]) for row in rows];errors=[]
    if len(keys)!=len(set(keys)) or set(keys)!=expected:errors.append("missing, duplicate or unexpected matrix rows")
    for row in rows:
        try:
            variant=row["variant"];expected_status="pass" if variant=="healthy" else "violation"
            common.require(row.get("status")==expected_status and row.get("cleanup")=="pass" and
                           type(row.get("NP")) is int and row["NP"]==4,
                           "row lacks expected outcome/NP4/cleanup")
            common.require(all(digest_ok(row.get(name)) for name in
                           ("module_sha256","client_sha256","checker_sha256","server_sha256","runner_sha256","config_sha256")),
                           "required code/config hash missing or malformed")
            common.require(isinstance(row.get("sources"),dict) and set(row["sources"])==source_fields and
                           all(digest_ok(value) for value in row["sources"].values()),"complete six-source hash set required")
            policy=row["time_policy"]
            common.require(all(policy.get(name)==value for name,value in CLOCK_PROFILE.items()) and
                           all(digest_ok(policy.get(name)) for name in ("plugin_sha256","source_sha256")),
                           "clock input policy missing or different")
            common.require(policy["source_sha256"]==row["sources"].get("time_choice.cpp"),"clock policy source hash differs")
            if row["backend"]=="native":
                common.require(digest_ok(row.get("checker_test_sha256")) and type(row.get("client_exit")) is int and
                               type(row.get("checker_exit")) is int,"native checker hash/exit evidence missing")
                native_result(row["observations"],row["client_exit"],variant)
                common.require(row.get("checker_exit")== (0 if variant=="healthy" else 1) and
                               row.get("server_exits")==[0,0,0] and all(type(code) is int for code in row["server_exits"]),
                               "native saved checker verdict/cleanup missing")
                expected_check="NATIVE_CHECK verdict=0 diagnostic=" if variant=="healthy" else \
                    "NATIVE_CHECK verdict=1 diagnostic=acknowledged local value missing at node 3 key alpha"
                common.require(row.get("checker_output","").strip()==expected_check,"native checker diagnostic differs")
                continue
            prerequisite=next(x for x in rows if x["variant"]==variant and x["backend"]=="native")
            for name in ("module_sha256","client_sha256","checker_sha256","server_sha256","runner_sha256","sources","time_policy"):
                common.require(name in row and row[name]==prerequisite.get(name),"model/native inputs differ: "+name)
            common.require(digest_ok(row.get("tracer_sha256")),"model tracer hash missing or malformed")
            owned=row.get("owned_tracee_pids")
            common.require(isinstance(owned,list) and len(owned)>=4 and all(type(pid) is int and pid>0 for pid in owned) and
                           len(set(owned))==len(owned),"model owned process inventory missing or invalid")
            common.require(isinstance(row.get("remaining_owned_pids"),list) and row["remaining_owned_pids"]==[],
                           "model cleanup lacks explicit empty remaining process inventory")
            search=row["search"];witness=search["witness"];edges=search["edges"]
            common.require(type(row.get("tracer_exit")) is int and row["tracer_exit"]==0 and search["status"]==expected_status and
                           search.get("branching_observed") is True and search.get("replay_verified") is True and
                           search.get("beam_width")==1 and search.get("partial_last_layer") is False and
                           search.get("expanded_layers",0)>0 and search.get("attempted_edges")==len(edges),"search evidence incomplete")
            common.require(search.get("termination")==("healthy_completion" if variant=="healthy" else "exact_violation"),
                           "search did not reach exact business outcome")
            common.require(search.get("selection")==GUIDE_POLICY and search.get("normal_choices")==[0,1],
                           "search guide or normal-choice profile differs")
            prefix=row["observations"];root=search["root"];path=search["path"]
            common.require(digest_ok(root,16) and root==row.get("prefix"),"search root differs from saved prefix")
            common.require(prefix and prefix[-1].get("event")=="before_set" and prefix[-1].get("key")=="alpha" and
                           not any(item.get("event")=="ack" for item in prefix) and search["root_observations"]==prefix,
                           "search root lacks pre-SET alpha observations without ACKs")
            elapsed=search["elapsed_seconds"];effective=search["bounds"]["effective_wall_seconds"]
            requested=search["bounds"]["wall_seconds"]
            common.require(all(type(value) in (int,float) and math.isfinite(value) for value in (elapsed,effective,requested)) and
                           0<=elapsed<effective<=requested and search.get("wall_cutoff") is False,"search exceeded or lacks wall evidence")
            common.require(isinstance(path,list) and path and len(path)==search["expanded_layers"] and
                           type(search["bounds"]["depth"]) is int and 0<len(path)<=search["bounds"]["depth"],
                           "search path is empty/incomplete or exceeds depth")
            source=root;selected=set();observations=copy.deepcopy(prefix);previous=-1
            source_histories=[];source_exits=[];client_exit=None
            for layer,step in enumerate(path):
                common.require(step["source"]==source and digest_ok(step["target"],16),"search path is not contiguous")
                index=step["edge_index"]
                common.require(type(index) is int and previous<index<len(edges),"invalid path edge index")
                edge=edges[index];action=step["action"]
                common.require(isinstance(action,list) and len(action)==3 and all(type(value) is int for value in action) and
                               0<=action[0]<4 and action[1]>=0 and action[2] in (0,1,-2),"invalid path actor/action")
                common.require(edge.get("selected") is True and edge.get("executed") is True and edge.get("status")=="completed" and
                               type(edge["layer"]) is int and edge["layer"]==layer and edge["predecessor"]==source and
                               edge["successor"]==step["target"] and [edge["node"],edge["thread"],edge["choice"]]==action,
                               "path does not reference its actual selected candidate")
                source_histories.append(observations[:]);source_exits.append(client_exit)
                common.require(isinstance(edge["observations"],list),"candidate observations must be a list")
                observations.extend(edge["observations"]);client_exit=edge["client_exit"]
                source=step["target"];previous=index;selected.add(index)
            common.require({index for index,edge in enumerate(edges) if edge.get("selected")}==selected,
                           "selected candidate is detached from retained path")
            last=path[-1]
            common.require(all(last[name]==witness[name] for name in ("source","action","target")) and
                           observations==witness["driver"]["observations"] and
                           edges[last["edge_index"]]["client_exit"]==witness["driver"]["client_exit"],
                           "witness does not end the rooted path with its actual observations/exit")
            groups={};actions_seen={};outcomes={}
            for index,edge in enumerate(edges):
                layer=edge["layer"]
                common.require(type(layer) is int and 0<=layer<len(path) and
                               edge["predecessor"]==path[layer]["source"] and digest_ok(edge["successor"],16),
                               "candidate is detached from its expanded path source")
                action=(edge["node"],edge["thread"],edge["choice"])
                common.require(all(type(value) is int for value in action) and 0<=action[0]<4 and action[1]>=0 and
                               action[2] in (0,1,-2),"invalid candidate actor/action")
                common.require(action not in actions_seen.setdefault(layer,set()),"duplicate candidate actor/action")
                actions_seen[layer].add(action)
                common.require(edge.get("executed") is True and edge.get("status")=="completed" and
                               ("selected" not in edge or type(edge["selected"]) is bool),"candidate did not complete")
                common.require(edge.get("verdict") in ("pass","violation") and type(edge.get("diagnostic")) is str and
                               (edge["verdict"]!="pass" or edge["diagnostic"]==""),"candidate verdict/diagnostic missing or invalid")
                common.require(isinstance(edge["observations"],list) and
                               all(isinstance(item,dict) for item in edge["observations"]) and
                               (edge["client_exit"] is None or type(edge["client_exit"]) is int),"candidate driver evidence invalid")
                if edge["node"]!=3 or edge["choice"]==-2:
                    common.require(not edge["observations"] and edge["client_exit"]==source_exits[layer],
                                   "non-client action changed client observations/exit")
                driver={"observations":source_histories[layer]+edge["observations"],"client_exit":edge["client_exit"]}
                outcome=model_outcome(driver,variant,edge["verdict"],edge["diagnostic"])
                common.require("outcome" in edge and edge["outcome"]==outcome,"candidate recorded outcome differs from actual verdict")
                common.require(outcome is None or layer==len(path)-1,"terminal candidate followed by additional retained layers")
                outcomes[index]=outcome
                groups.setdefault(layer,set()).add(edge["successor"])
            common.require(any(len(targets)>=2 for targets in groups.values()),"no expanded source has two distinct successors")
            terminal=edges[last["edge_index"]];verdict=terminal["verdict"]
            common.require(outcomes[last["edge_index"]]==expected_status and terminal["diagnostic"]==witness["diagnostic"] and
                           model_outcome(witness["driver"],variant,verdict,terminal["diagnostic"])==expected_status,
                           "witness business evidence differs")
            replays=search["replays"]
            common.require(len(replays)==3 and all(replay["source"]==witness["source"] and replay["target"]==witness["target"] and
                           replay["action"]==witness["action"] and replay["verdict"]==verdict and
                           replay["diagnostic"]==witness["diagnostic"] and
                           replay["observations"]==witness["driver"]["observations"] and
                           replay["client_exit"]==witness["driver"]["client_exit"] for replay in replays),"three exact replays missing")
        except Exception as error:errors.append(str((row.get("variant"),row.get("backend")))+": "+str(error))
    valid=not errors
    full=valid and not native_only and expected=={(v,b) for v in ("healthy","mutant") for b in ("native","full","pages")}
    return {"requested_matrix":[list(key) for key in sorted(expected)],"requested_matrix_complete":valid,
            "model_matrix_complete":bool(full),"passed":valid,"matrix_errors":errors}


def model_control(work,tracer,module,client,checker,variant,backend,timeout,max_steps,search_depth=2000,search_seconds=180,time_plugin=None):
    """Reach the pre-SET/fault prefix by real requests; never search startup."""
    time_plugin=time_plugin or work/"time-choice.so"
    directory=work/(variant+"-"+backend)
    logs=["/node"+str(node)+"-raftlog.db" for node in range(1,4)]
    tracees=[server_argv(module,node,"192.168.0."+str(node),6379,logs[node-1]) for node in range(1,4)]
    tracees.append([str(client)]+[part for node in range(1,4) for part in ("192.168.0."+str(node),"6379")])
    row={"target":"redisraft-full","variant":variant,"backend":backend,"NP":4,
         "status":"incomplete","artifacts":str(directory),"scope":"hand-controlled initialization to pre-SET, then bounded case-side beam1 search",
         "bounds":{"control_steps":max_steps,"wall_seconds":timeout,"search_depth":search_depth,"search_seconds":search_seconds,
                   "address_space_bytes":common.MODEL_ADDRESS_SPACE_LIMIT},
         "tracer_sha256":common.digest(tracer),"module_sha256":common.digest(module),
         "client_sha256":common.digest(client),"checker_sha256":common.digest(checker),"server_sha256":common.digest(SERVER),
         "runner_sha256":common.digest(Path(__file__)),"time_policy":time_policy_context(time_plugin),
         "sources":{name:common.digest(HERE/name) for name in ("client.c","checker.cpp","checker_test.cpp","time_choice.cpp","time_choice_test.cpp")} |
                   {"common_runner.py":common.digest(Path(common.__file__))}}
    session=None;events=[];observed=[];phase="startup";last=[""]*4
    try:
        session=FullSession(tracer,tracees,checker,logs,directory,backend,timeout,time_plugin)
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
                common.require(not any(item.get("event")=="ack" for item in observed),"search root follows a SET acknowledgement")
                phase="guided_search"
                row["search"]=guided_search(session,row["prefix"],{"observations":observed,"client_exit":None},
                                             variant,search_depth,search_seconds)
                row.update(status=row["search"]["status"],diagnostic=row["search"].get("diagnostic",row["search"]["termination"]))
                break
            stable=stable+1 if session.current_hash()==before else 0
            if stable>=16:raise common.CaseFailure("real request pump made no successor progress under explicit finite-timeout policy")
        else:raise common.CaseFailure("membership/fault-prefix control step bound exceeded")
    except Exception as error:
        row.update({"status":"failed","diagnostic":str(error)} if isinstance(error,ClientAssertion)
                   else common.classify_error(error,session.log if session else None))
        row["failure_phase"]=phase
        row["last_successful_hash"]=events[-1].get("predecessor") if events else None
        row["last_replies"]=last
    finally:
        if session:
            if "search" in row:(directory/"search.json").write_text(json.dumps(row["search"],indent=2)+"\n")
            (directory/"control-events.json").write_text(json.dumps(events,indent=2)+"\n")
            (directory/"observations.json").write_text(json.dumps(observed,indent=2)+"\n")
            row["config_sha256"]=common.digest(directory/"config.json")
            row["tracer_exit"]=session.close(timeout=30)
            remaining=[pid for pid in session.owned_pids if Path(f"/proc/{pid}").exists()]
            row["remaining_owned_pids"]=remaining
            row["cleanup"]="failed" if remaining else "pass"
            if remaining:row.update(status="failed",cleanup_diagnostic="owned tracees remain after cleanup: "+str(remaining))
            if row["tracer_exit"]!=0 and row["status"] in ("pass","violation"):row.update(status="failed",diagnostic="tracer exited nonzero")
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
    parser.add_argument("--search-depth",type=int,default=2000)
    parser.add_argument("--search-seconds",type=float,default=180)
    parser.add_argument("--reuse-modules",type=Path)
    parser.add_argument("--address-space-mb",type=int,default=4096)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix="run-",dir=args.output.resolve()))
    variants=("healthy","mutant") if args.variant=="all" else (args.variant,)
    backends=("full","pages") if args.backend=="all" else (args.backend,)
    common.require(args.search_depth>0 and args.search_seconds>0,"positive search bounds required")
    rows=[]
    report={"workdir":str(work),"passed":False,"rows":rows,"model_matrix_complete":False,
        "scope":"native-only actual Redis+loaded module" if args.native_only else "native prerequisites plus bounded case-side beam1 search; unretained frontiers remain uncovered"}
    try:
        client=work/"client"
        checker_test=work/"checker-test"
        checker=work/"checker.so"
        time_plugin=work/"time-choice.so"
        time_test=work/"time-choice-test"
        command(["/usr/bin/g++","-std=c++20","-O2","-g","-Wall","-Wextra","-Werror","-I"+str(ROOT/"src"),HERE/"time_choice.cpp",HERE/"time_choice_test.cpp","-o",time_test],ROOT,work/"time-choice-test-build.log")
        command([time_test],ROOT,work/"time-choice-test.log")
        command(["/usr/bin/g++","-std=c++20","-O2","-g","-Wall","-Wextra","-Werror","-fPIC","-shared","-I"+str(ROOT/"src"),HERE/"time_choice.cpp","-o",time_plugin],ROOT,work/"time-choice-build.log")
        policy=time_policy_context(time_plugin)
        command(["/usr/bin/g++","-std=c++20","-O2","-g","-Wall","-Wextra","-Werror","-I"+str(ROOT/"src"),"-I"+str(ROOT/"third_party"),HERE/"checker_test.cpp","-o",checker_test],ROOT,work/"checker-test-build.log")
        command([checker_test],ROOT,work/"checker-test.log")
        command(["/usr/bin/g++","-std=c++20","-O2","-g","-Wall","-Wextra","-Werror","-fPIC","-shared","-I"+str(ROOT/"src"),"-I"+str(ROOT/"third_party"),HERE/"checker.cpp","-o",checker],ROOT,work/"checker-build.log")
        command(["/usr/bin/gcc","-std=gnu11","-O0","-g","-gdwarf-4","-fno-pie","-no-pie","-Wall","-Wextra","-Werror",HERE/"client.c","-o",client],ROOT,work/"client-build.log")
        for variant in variants:
            if args.reuse_modules:
                manifest=args.reuse_modules.resolve()/variant/"manifest.json"
                module=manifest.parent/"source/redisraft.so"
                common.require(module.exists() and common.digest(module)==json.loads(manifest.read_text())["module_sha256"],"reused module does not match manifest")
            else:module,manifest=build(work,variant)
            row=native(work,module,client,variant,args.timeout,checker_test,policy)
            row.update(manifest=str(manifest),client_sha256=common.digest(client),module_sha256=common.digest(module),server_sha256=common.digest(SERVER),
                checker_sha256=common.digest(checker),checker_test_sha256=common.digest(checker_test),runner_sha256=common.digest(Path(__file__)),
                sources={name:common.digest(HERE/name) for name in ("client.c","checker.cpp","checker_test.cpp","time_choice.cpp","time_choice_test.cpp")} |
                        {"common_runner.py":common.digest(Path(common.__file__))})
            rows.append(row)
            if not args.native_only:
                common.require(args.tracer and args.tracer.exists(),"matching immutable NP4 --tracer required")
                common.require(row["status"] in ("pass","violation"),"native prerequisite failed")
                common.MODEL_ADDRESS_SPACE_LIMIT=args.address_space_mb*1024**2
                for backend in backends:
                    rows.append(model_control(work,args.tracer.resolve(),module,client,checker,variant,backend,args.timeout,args.max_control_steps,
                                              args.search_depth,args.search_seconds,time_plugin))
    except Exception as error: report["diagnostic"]=str(error)
    report.update(matrix_acceptance(rows,variants,backends,args.native_only))
    (work/"result.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report,indent=2))
    return 0 if report["passed"] else 1

if __name__=="__main__": sys.exit(main())
