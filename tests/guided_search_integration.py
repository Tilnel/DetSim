#!/usr/bin/env python3
"""Finite NP1 two-thread successor oracle for the case CLI enumerator."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"examples/cases"))
import redisraft_full_run as case

FIXTURE=r'''
#include <sys/syscall.h>
#include <unistd.h>
alignas(16) static unsigned char worker_stack[65536];
extern long spawn(void *);
asm(".text\n.globl spawn\n.type spawn,@function\nspawn:\n"
    "mov %rdi,%rsi\nmov $0x50f00,%edi\nxor %edx,%edx\nxor %r10d,%r10d\nxor %r8d,%r8d\n"
    "mov $56,%eax\nsyscall\ntest %rax,%rax\njz 1f\nret\n"
    "1: mov $1,%eax\nmov $1,%edi\nlea letter(%rip),%rsi\nmov $1,%edx\nsyscall\n"
    "mov $60,%eax\nxor %edi,%edi\nsyscall\nud2\n.size spawn,.-spawn\n"
    ".section .rodata\nletter: .ascii \"B\"\n.text\n");
int main(void) {
    if (spawn(worker_stack+sizeof worker_stack)<0) return 71;
    syscall(SYS_write,1,"A",1);
    syscall(SYS_exit,0);
    return 72;
}
'''.replace('alignas(16)','_Alignas(16)')


class Session(case.common.Session):
    current_hash=case.FullSession.current_hash


def run_backend(work,tracer,binary,backend):
    session=Session(tracer,[str(binary)],[],work/backend,backend,120)
    row={"backend":backend,"status":"failed"}
    owned=set()
    try:
        session.send()
        owned={int(pid) for pid in Path(f"/proc/{session.proc.pid}/task/{session.proc.pid}/children").read_text().split()}
        for _ in range(120):
            actors=case.thread_actors(session,0)
            if len(actors)==2 and all(state=="runnable" for _,state,_ in actors):break
            case.control_step(session,0,0,[],[""]*4,[])
        else:raise RuntimeError("fixture did not reach two runnable logical actors")
        source=session.current_hash();reference={}
        for slot in (0,1):
            case.restore_source(session,source)
            session.send("sw 0");session.send("thread "+str(slot))
            reply=session.send("si 0")
            case.common.require("DetSim state/execution error:" not in reply and "Cannot step" not in reply,reply)
            target=session.current_hash();reference[slot]={"target":target,"reply":reply}
        expected={item["target"] for item in reference.values()}
        case.common.require(len(expected)==2,"manual reference lacks two distinct successors")
        enumerations=[]
        for alternate in (0,1,0,1):
            case.restore_source(session,reference[alternate]["target"])
            edges=[]
            candidates=case.search_layer(session,source,{"observations":[],"client_exit":None},edges,process_count=1)
            actual={candidate["edge"]["successor"] for candidate in candidates}
            case.common.require(actual==expected and len(edges)==2,"case enumerator differs from manual actor successor set")
            case.common.require(all(edge["predecessor"]==source for edge in edges),"source derived from wrong incoming edge")
            enumerations.append(edges)
        case.restore_source(session,source)
        session.send("sw 0");session.send("thread 0")
        dfs=session.send("dfs 1 --quiet")
        case.common.require("DetSim state/execution error:" not in dfs,dfs)
        dfs_target=session.current_hash()
        (work/backend/"builtin-dfs.log").write_text(dfs)
        counts=re.findall(r"total unique: (\d+)",dfs)
        row.update(status="pass",source=source,reference=reference,enumerations=enumerations,
                   builtin_dfs={"reply":dfs,"last_target":dfs_target,
                                "scope":"builtin last target is observation only; not a complete successor-set API",
                                "summary_unique_states":int(counts[-1]) if counts else None,
                                "reference_root_plus_successors":1+len(expected),
                                "C03":"case enumerator verifies both logical actors; builtin DFS actor completeness is not established"})
    except Exception as error:row["diagnostic"]=str(error)
    finally:
        row["tracer_exit"]=session.close(timeout=20)
        row["remaining_owned_pids"]=[pid for pid in owned if Path(f"/proc/{pid}").exists()]
        row["cleanup"]="pass" if not row["remaining_owned_pids"] and row["tracer_exit"]==0 else "failed"
        if row["cleanup"]!="pass":row["status"]="failed"
    return row


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracer",type=Path,default=Path(os.environ.get("DETSIM_TRACER",ROOT/"tracer")))
    parser.add_argument("--output",type=Path,default=ROOT/"build/guided-search-integration")
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    work=Path(tempfile.mkdtemp(prefix="run-",dir=args.output.resolve()))
    source=work/"actors.c";source.write_text(FIXTURE);binary=work/"actors"
    built=case.common.command(["/usr/bin/gcc","-std=gnu11","-O0","-g","-gdwarf-4","-fno-pie","-no-pie","-Wall","-Wextra","-Werror",source,"-o",binary],ROOT)
    (work/"build.log").write_text(built.stdout)
    case.common.require(built.returncode==0,built.stdout)
    native=case.common.command([binary],work)
    case.common.require(native.returncode==0 and sorted(native.stdout)==["A","B"],"native two-actor fixture failed: "+native.stdout)
    rows=[run_backend(work,args.tracer.resolve(),binary,backend) for backend in ("full","pages")]
    report={"passed":all(row["status"]=="pass" for row in rows),"workdir":str(work),"rows":rows,
            "tracer_sha256":case.common.digest(args.tracer),"fixture_sha256":case.common.digest(source),
            "binary_sha256":case.common.digest(binary),"runner_sha256":case.common.digest(case.__file__),
            "scope":"finite two-runnable-actor successor equivalence; not full-module or general scheduling verification"}
    (work/"result.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(report,indent=2));return 0 if report["passed"] else 1


if __name__=="__main__":sys.exit(main())
