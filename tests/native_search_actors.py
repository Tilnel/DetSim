#!/usr/bin/env python3
"""Actual native CLI BFS/DFS actor/timeout execution oracle; Python never searches."""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'examples/cases'))
from run import Session,command,digest,require

MODES={'actors':'a','choices':'c','waiting':'w','race':'r','deadlines':'d','mixed':'m','pending':'p'}


def events(reply):
    return [json.loads(line)for line in re.findall(r'NATIVE_SEARCH_EVENT (\{[^\n]+\})',reply)]


def current(session):
    reply=session.send('info current');edge=re.findall(r'([0-9a-f]{16}) => ([0-9a-f]{16})',reply)
    if edge:return edge[-1][1]
    initial=re.search(r'Current state ([0-9a-f]{16}) \(no predecessor\)',reply)
    require(initial,'current identity absent: '+reply);return initial[1]


def checked(session,text):
    reply=session.send(text)
    require('DetSim state/execution error:'not in reply and 'Reach illegal state'not in reply,reply)
    return reply


def actors(session):
    return {int(slot):(state,'deadline='in line)for line in checked(session,'thread').splitlines()
            for slot,state in re.findall(r'\[(\d+)\] TID \d+ state=(\w+)',line)}


def prepare(session,mode):
    session.send();checked(session,'sw 0')
    for _ in range(100):
        if 1 in actors(session):break
        checked(session,'thread 0');checked(session,'si 0')
    else:raise RuntimeError('worker clone did not publish')
    if mode in ('waiting','race','deadlines'):
        checked(session,'thread 0');require('<pending:waiting>'in checked(session,'si 0'),'main did not wait')
    if mode in ('waiting','deadlines','mixed','pending'):
        checked(session,'thread 1');require('<pending:waiting>'in checked(session,'si 0'),'worker did not wait')
    if mode=='pending':
        checked(session,'thread 0');checked(session,'si 0')
    inventory=actors(session)
    if mode=='waiting':require(inventory=={0:('waiting',False),1:('waiting',True)},str(inventory))
    elif mode=='race':require(inventory=={0:('waiting',True),1:('runnable',False)},str(inventory))
    elif mode=='deadlines':require(inventory=={0:('waiting',True),1:('waiting',True)},str(inventory))
    elif mode=='mixed':require(inventory=={0:('runnable',False),1:('waiting',True)},str(inventory))
    else:require(inventory=={0:('runnable',False),1:('runnable',False)},str(inventory))
    return current(session),inventory


def actions(mode):
    if mode=='choices':return [(0,'0'),*( (1,str(choice))for choice in range(3))]
    if mode=='mixed':return [*((0,str(choice))for choice in range(3)),(1,'timeout')]
    if mode=='pending':return [*((0,str(choice))for choice in range(3)),(1,'0')]
    if mode=='waiting':return [(1,'timeout')]
    if mode=='race':return [(0,'timeout'),(1,'0')]
    if mode=='deadlines':return [(0,'timeout'),(1,'timeout')]
    return [(0,'0'),(1,'0')]


def fingerprint(event):
    return (event['process'],event['thread'],event['choice'],event['syscall'],event['phase'],event['result_valid'],event['result'],
            json.dumps(event['after'],sort_keys=True))


def expected_event(mode,slot,choice):
    if choice=='timeout':return (35 if mode in ('waiting','deadlines','mixed')else 202,-2,0 if mode!='race'else -110)
    if (mode=='choices'and slot==1)or (mode in ('mixed','pending')and slot==0):return (96,int(choice),0)
    if mode=='race':return (202,-1,1)
    if mode=='pending'and slot==1:return (202,-1,0)
    return (1,-1,1)


def expected_terminals(mode):
    clocks={'actors':[0],'choices':[0,1000,2000],'waiting':[2],'deadlines':[2],
            'mixed':[2,1000,2000,1002,2002],'pending':[0,1000,2000]}
    if mode=='race':return {(text,value,(1,micros))for text in ('AB','BA')for value,micros in [(0,0),(-1,1)]}
    return {(text,0,(1,micros))for text in (('BA',)if mode=='waiting'else ('AB','BA'))for micros in clocks[mode]}


def run(work,tracer,binary,plugin,mode,backend,search):
    directory=work/f'{mode}-{backend}-{search}'
    session=Session(tracer,[str(binary),MODES[mode]],[str(plugin)],directory,backend,120,
                    choose_points=[{'syscall':96,'choose':3}]if mode in ('choices','mixed','pending')else [],
                    runtime_profile={'initial_time_us':1000000})
    row={'mode':mode,'backend':backend,'search':search,'artifacts':str(directory),'passed':False};owned=set()
    try:
        source,inventory=prepare(session,mode);row.update(source=source,actors=inventory)
        children=Path(f'/proc/{session.proc.pid}/task/{session.proc.pid}/children').read_text().split()
        require(len(children)==1,'expected exactly one owned process leader')
        for pid in children:owned.update(int(path.name)for path in Path(f'/proc/{pid}/task').iterdir())
        require(len(owned)==2,'fixture must have exactly two owned TIDs')
        row['owned_tids']=sorted(owned)
        checked(session,'thread 0')
        # Search is FIRST, before manual references write any canonical TSS edge.
        reply=checked(session,'dfs 1 --quiet'if search=='dfs'else 'bfs --quiet')
        (directory/'native-search.log').write_text(reply)
        require('Complete explore all'in reply,'native search did not finish')
        executed=events(reply);row['native_callback_events']=executed
        reference=[]
        for slot,choice in actions(mode):
            require('Loaded state'in checked(session,'load '+source),'source restore failed')
            checked(session,'sw 0');checked(session,'thread '+str(slot))
            answer=checked(session,'si '+choice);actual=events(answer)
            require(len(actual)==1,'manual reference missing actual callback')
            event=actual[0];target=current(session)
            number,preset,result=expected_event(mode,slot,choice)
            require((event['process'],event['thread'],event['syscall'],event['choice'],event['phase'],event['result_valid'],event['result'])==
                    (0,slot,number,preset,2,1,result),'manual reference executed wrong actor/syscall/choice/outcome: '+str(event))
            if choice=='timeout':
                require(event['choice']==-2 and event['thread']==slot,'timeout actor/choice mismatch')
                require(event['before']['main_marker']==event['after']['main_marker']==0 and
                        event['before']['worker_marker']==event['after']['worker_marker']==0,'timeout advanced user code')
            replays=[]
            for _ in range(3):
                require('Loaded state'in checked(session,'load '+source),'replay source missing')
                checked(session,'thread '+str(slot));again=checked(session,'si '+choice)
                repeated=events(again);require(len(repeated)==1 and fingerprint(repeated[0])==fingerprint(event),'callback replay drift')
                replays.append(current(session));require(replays[-1]==target,'same-source actor replay identity drift')
            reference.append({'thread':slot,'choice':choice,'successor':target,'event':event,'replays':replays})
        require(len(reference)==len(actions(mode)) and len({item['successor']for item in reference})==len(reference) and
                len({fingerprint(item['event'])for item in reference})==len(reference),'manual reference cardinality/targets/fingerprints unexpectedly alias')
        require(len({item['event']['before']['metadata_signature']for item in reference})==1,'manual references used different saved sources')
        row['reference']=reference
        root_signature=reference[0]['event']['before']['metadata_signature']
        actual=[event for event in executed if event['before']['metadata_signature']==root_signature]
        row['actual_root_callbacks']=actual
        require(Counter(fingerprint(event)for event in actual)==Counter(fingerprint(item['event'])for item in reference),
                'native current-response callback set lacks actor/action/choice successors: '+str([(e['thread'],e['choice'])for e in actual]))
        if search=='dfs':require(len(actual)==len(executed)==len(reference),'DFS1 did not execute exactly root actions')
        if mode=='race':require(actual[0]['choice']==-2,'finite main timeout must be explored even before the waker')
        if mode in ('mixed','pending'):
            require([event['thread']for event in actual]==[0,0,0,1],'per-actor alternatives/order lost')
            require(actual[-1]['choice']==(-2 if mode=='mixed'else -1),'stale choices leaked into timeout/pending completion')
        if search=='bfs':
            terminal=[event for event in executed if event['phase']==4 and event['after']['lifecycle']=='exited']
            require(terminal and all(event['after']['exit_code']==0 and event['after']['main_marker']==event['after']['worker_marker']==1 for event in terminal),
                    'finite BFS lacks actual successful process-exit callbacks')
            projections={(event['after']['stdout'],event['after']['main_result'],tuple(event['after']['clock']))for event in terminal}
            row['terminal_projections']=sorted(projections);row['terminal_callback_count']=len(terminal)
            require(projections==expected_terminals(mode),'finite BFS terminal projection set differs: '+str(projections))
        row.update(passed=True,reference_written_after_native_response=True,config_sha256=digest(directory/'config.json'))
    except Exception as error:row['diagnostic']=str(error)
    finally:
        row['tracer_exit']=session.close(timeout=20)
        row['remaining_owned_tids']=[pid for pid in owned if Path(f'/proc/{pid}').exists()]
        if row['tracer_exit']!=0 or row['remaining_owned_tids']:row['passed']=False
    return row


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--tracer',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'build/native-search-actors')
    parser.add_argument('--modes',nargs='+',choices=MODES,default=list(MODES));parser.add_argument('--searches',nargs='+',choices=['dfs','bfs'],default=['dfs','bfs'])
    args=parser.parse_args();args.tracer=args.tracer.resolve();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
    os.environ.clear();os.environ.update({'PATH':'/usr/bin:/bin','LANG':'C','LC_ALL':'C','PYTHONHASHSEED':'0','CC':'/usr/bin/gcc','CXX':'/usr/bin/g++'})
    work=Path(tempfile.mkdtemp(prefix='run-',dir=args.output));binary=work/'actors';plugin=work/'observer.so'
    report={'passed':False,'tracer':str(args.tracer),'tracer_sha256':digest(args.tracer),'workdir':str(work),'rows':[],
            'scope':'Actual native CLI search callbacks collected BEFORE manual reference edges; fresh session per search, no Python successor search loop.'}
    try:
        for build in [['/usr/bin/gcc','-std=gnu11','-O0','-g','-gdwarf-4','-fno-pie','-no-pie','-Wall','-Wextra','-Werror',ROOT/'tests/native_search_actors.c','-o',binary],
                      ['/usr/bin/g++','-std=gnu++20','-shared','-fPIC','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),'-I'+str(ROOT/'third_party'),ROOT/'tests/native_search_plugin.cpp','-o',plugin]]:
            result=command(build,ROOT);require(result.returncode==0,result.stdout)
        sources=[Path(__file__).resolve(),ROOT/'tests/native_search_actors.c',ROOT/'tests/native_search_plugin.cpp',ROOT/'examples/cases/run.py']
        archive=work/'source-archive';archive.mkdir()
        frozen={}
        for path in sources:
            target=archive/(digest(path)+'-'+path.name);target.write_bytes(path.read_bytes());target.chmod(0o400)
            frozen[str(path)]={'path':str(target),'sha256':digest(target)}
        report.update(binary_sha256=digest(binary),plugin_sha256=digest(plugin),sources={str(path):digest(path)for path in sources},source_archives=frozen)
        for mode in args.modes:
            native=command([binary,MODES[mode]],work);require(native.returncode==0 and native.stdout in ('AB','BA'),'native oracle failed')
            for backend in ('full','pages'):
                for search in args.searches:
                    row=run(work,args.tracer,binary,plugin,mode,backend,search);report['rows'].append(row)
                    (work/'results.json').write_text(json.dumps(report,indent=2)+'\n')
                    print(mode,backend,search,'PASS'if row['passed']else row.get('diagnostic','FAILED'),flush=True)
        report['passed']=all(row['passed']for row in report['rows']);return 0 if report['passed']else 1
    finally:
        (work/'results.json').write_text(json.dumps(report,indent=2)+'\n');print('RESULT',work/'results.json',flush=True)


if __name__=='__main__':raise SystemExit(main())
