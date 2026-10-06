# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "Pillow>=11"]
# ///
"""Fresh-process parser measurements; warm every input, rotate order, validate output."""
import argparse
import hashlib
import importlib.util
import json
import platform
from pathlib import Path
import subprocess
import tempfile
import time
import analysis
import psutil

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('workflow',ROOT/'experiments/load-workflow/run.py')
workflow=importlib.util.module_from_spec(spec)
spec.loader.exec_module(workflow)


def fixtures(root, real):
    corpus=workflow.workloads(root,real)
    corpus=[w for w in corpus if w['id'] not in ('mixed','trimmed')]
    corpus[0]=dict(id='tiny',path=sorted(corpus[0]['path'].glob('*.obj'))[0])
    for count in (4096,16384,65536,262144):
        path=root/f'positions_{count}.obj'
        with path.open('w',encoding='utf-8',newline='\n') as out:
            for i in range(count): out.write(f'v {i%512}.123456789 {i//512}.987654321 -1.23456789e-05\n')
        corpus.append(dict(id=f'positions_{count}',path=path))
    path=root/'negative_seams.obj'
    with path.open('w',encoding='utf-8',newline='\n') as out:
        for i in range(50000):
            x=i*2
            out.write(f'v {x} 0 0\nv {x+1} 0 0\nv {x} 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\n'
                      'f -3/-3/-1 -2/-2/-1 -1/-1/-1\n')
    corpus.append(dict(id='negative_seams',path=path))
    return corpus


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('executable',type=Path)
    p.add_argument('output',type=Path)
    p.add_argument('--before',type=Path)
    p.add_argument('--rounds',type=int,default=5)
    p.add_argument('--only',nargs='+')
    p.add_argument('--sweep',nargs='+',type=int,help='Additional prototype block sizes, in KiB')
    p.add_argument('--workers',type=int,default=0)
    p.add_argument('--max-background-cpu',type=float,default=100)
    args=p.parse_args()
    if args.rounds<1: p.error('rounds must be positive')
    if not 0<=args.max_background_cpu<=100: p.error('CPU limit must be between 0 and 100 percent')
    if not 0<=args.workers<=64: p.error('workers must be between 0 and 64')
    if any(not 0<k<=65536 for k in args.sweep or []): p.error('block sizes must be between 1 and 65536 KiB')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    exe=args.executable.resolve(strict=True)
    configs=[('legacy',exe,'legacy',0),('prototype',exe,'prototype',0)]
    if args.before: configs.insert(1,('before',args.before.resolve(strict=True),'prototype',0))
    for k in args.sweep or []: configs.append((f'chunk_{k}k',exe,'prototype',k*1024))
    report=dict(method='Fresh processes; input warmed before each run; rotated order; guarded; ordered parser fingerprints outside timing',
        rounds=args.rounds,worker_limit=args.workers,worker_limit_scope='both parsers',max_background_cpu=args.max_background_cpu,
        platform=platform.platform(),logical_cpus=psutil.cpu_count(),physical_cpus=psutil.cpu_count(logical=False),
        ram_bytes=psutil.virtual_memory().total,
        revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        source_sha256={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest()
                       for folder in ('experiments/parser-throughput','third_party/rapidobj/include/rapidobj')
                       for path in (ROOT/folder).glob('*') if path.is_file()},
        executables={str(c[1]):hashlib.sha256(c[1].read_bytes()).hexdigest() for c in configs},records=[],excluded=[])
    with tempfile.TemporaryDirectory(prefix='woby-parser-throughput-') as temporary:
        corpus=fixtures(Path(temporary).resolve(),Path('D:/temp/obj_tests'))
        if args.only: corpus=[w for w in corpus if w['id'] in args.only]
        if not corpus: p.error('No workloads selected')
        report['inputs']=[dict(id=w['id'],bytes=w['path'].stat().st_size,sha256=workflow.digest_input(w['path'])) for w in corpus]
        for round_index in range(args.rounds):
            order=configs[round_index%len(configs):]+configs[:round_index%len(configs)]
            for w in corpus:
                for label,program,mode,chunk in order:
                    for attempt in range(4):
                        while workflow.overlap(): time.sleep(1)
                        time.sleep(.5)
                        workflow.warm(w['path'])
                        quiet=[]
                        if args.max_background_cpu<100:
                            gate=workflow.module(ROOT/'experiments/load-workflow/idle.py','parser_idle')
                            quiet=gate.wait_for_quiet(lambda:psutil.cpu_percent(interval=.5),args.max_background_cpu)
                        result=subprocess.run([str(program),mode,str(w['path']),str(chunk),str(args.workers)],
                                              capture_output=True,text=True,timeout=120,check=True)
                        row=json.loads(result.stdout)
                        row.update(model=w['id'],variant=label,round=round_index+1,background_cpu_before=quiet)
                        interference=workflow.overlap()
                        if interference:
                            report['excluded'].append(dict(record=row,interference=interference))
                            continue
                        report['records'].append(row)
                        print(json.dumps(row),flush=True)
                        args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
                        break
                    else: raise RuntimeError('Four interfered attempts')
        report['summaries']=analysis.summarize(report['records'],[w['id'] for w in corpus],[c[0] for c in configs],args.rounds)
        report['regressions']=analysis.regressions(report['summaries'])
        args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')


if __name__=='__main__': main()
