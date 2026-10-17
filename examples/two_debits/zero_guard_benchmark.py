"""Paired finite exploration with a large, zero private anonymous guard.

This synthetic workload measures capture/execute/check/restore/deduplication,
not Redis performance. Each result also requires three exact terminal replays.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import statistics
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'examples'))
from _support import session as cases


class ChoiceSession(cases.Session):
    def __init__(self, *args, **kwargs):
        kwargs['choices'] = True
        super().__init__(*args, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tracer', type=Path, default=ROOT / 'tracer', help='NP1 production tracer')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/zero-guard-benchmark')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--size-mb', type=int, default=192)
    args = parser.parse_args()
    if args.repeats < 1 or args.size_mb < 1:
        parser.error('positive repeats and size required')
    tracer = args.tracer.resolve()
    cases.require(tracer.is_file(), 'NP1 tracer required')
    args.output.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    source = work / 'guard-inputs.c'
    original = (ROOT / 'examples/two_debits/native_inputs.c').read_text()
    anchor = '    int bits[2] = {0, 0};'
    cases.require(original.count(anchor) == 1, 'input fixture changed; review benchmark insertion')
    source.write_text(original.replace('#include <stdio.h>',
        '#include <stdio.h>\n#include <sys/mman.h>\nvoid *benchmark_guard;').replace(anchor,
        f'    benchmark_guard=mmap(NULL,{args.size_mb}UL*1024*1024,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);\n'
        '    if(benchmark_guard==MAP_FAILED) return 3;\n' + anchor))
    binary, plugin = work / 'guard-inputs', work / 'input-checker.so'
    compiled = cases.command(['/usr/bin/gcc', '-O0', '-g', '-gdwarf-4', '-fno-pie', '-no-pie',
                              '-Wall', '-Wextra', '-Werror', source, '-o', binary], ROOT)
    cases.require(compiled.returncode == 0, compiled.stdout)
    cases.build_plugin(ROOT / 'examples/two_debits/input_checker.cpp', plugin, ['/usr/bin/g++'])
    for bits in ('00', '01', '10', '11'):
        result = cases.command([binary, bits], work)
        expected = f'CASE_OK {bits} {1 if bits == "00" else 0}\n'
        cases.require(result.returncode == 0 and result.stdout == expected, 'native oracle mismatch')
    rows = []
    report = {'scope': __doc__, 'guard_mib': args.size_mb, 'repeats': args.repeats,
              'runner_sha256': cases.digest(__file__), 'rows': rows, 'passed': False}
    old_session = cases.Session
    old_mode = os.environ.get('DETSIM_ZERO_GUARD_RESTORE')
    cases.Session = ChoiceSession
    try:
        for trial in range(args.repeats):
            for backend in ('full', 'pages'):
                for mode in (('write', 'discard') if trial % 2 == 0 else ('discard', 'write')):
                    directory = work / f'{trial}-{backend}-{mode}'
                    directory.mkdir()
                    wrapper, timefile = directory / 'tracer-time', directory / 'time.txt'
                    wrapper.write_text('#!/bin/sh\nexec /usr/bin/time -v -o ' + shlex.quote(str(timefile)) +
                                       ' ' + shlex.quote(str(tracer)) + ' "$@"\n')
                    wrapper.chmod(0o700)
                    os.environ['DETSIM_ZERO_GUARD_RESTORE'] = mode
                    started = time.monotonic()
                    result = cases.search(wrapper, [str(binary)], plugin, directory / 'work', backend, 600, depth=30)
                    row = {'trial': trial, 'backend': backend, 'mode': mode, 'result': result,
                           'whole_driver_seconds': time.monotonic() - started, 'path': str(directory)}
                    rows.append(row)
                    cases.require(result.get('status') == 'pass' and result.get('guest_exit') == 0 and
                                  result.get('tracer_exit') == 0 and result.get('replay') == 'same_process_hash_match_3x', str(result))
                    log = (directory / 'work/transcript.log').read_text()
                    row['search_seconds'] = float(re.findall(r'Time elapsed: ([0-9.]+)s', log)[-1])
                    row['restore_ms'] = int(re.findall(r'Total restore time:\s+(\d+) ms', log)[-1])
                    row['restore_stats'] = json.loads(re.findall(r'PAGE_RESTORE (\{[^\n]+\})', log)[-1])
                    row['peak_command_rss_kib'] = int(re.search(r'Maximum resident set size \(kbytes\): (\d+)', timefile.read_text())[1])
                    cases.require((row['restore_stats']['zero_guard_bytes'] > 0) == (mode == 'discard'), 'wrong restore path')
                    pids = {int(pid) for pid in re.findall(r'(?:#pid = |\(pid=)(\d+)', log)}
                    cases.require(pids and not any(Path(f'/proc/{pid}').exists() for pid in pids), 'owned process remains')
                    row.update({name + '_sha256': cases.digest(path) for name, path in
                                [('source', source), ('binary', binary), ('tracer', tracer), ('plugin', plugin),
                                 ('helper', ROOT / 'examples/_support/session.py'), ('config', directory / 'work/config.json')]})
                    cases.require(all(other['result'].get('visited') == result['visited'] for other in rows), 'search work differs')
                    (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
                    print(backend, mode, trial, row['search_seconds'], flush=True)
        report['summary'] = {backend: {mode: {field: statistics.median(row[field] for row in rows
            if row['backend'] == backend and row['mode'] == mode) for field in
            ('search_seconds', 'whole_driver_seconds', 'restore_ms', 'peak_command_rss_kib')}
            for mode in ('write', 'discard')} for backend in ('full', 'pages')}
        report['passed'] = True
    finally:
        cases.Session = old_session
        if old_mode is None:
            os.environ.pop('DETSIM_ZERO_GUARD_RESTORE', None)
        else:
            os.environ['DETSIM_ZERO_GUARD_RESTORE'] = old_mode
        (work / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        print('RESULT', work / 'results.json', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
