# /// script
# requires-python = ">=3.11"
# dependencies = ["psutil>=6", "Pillow>=11"]
# ///
"""Measure real hidden desktop imports, preparation, uploads, and rendering.

Run only after builds/tests finish. Fresh viewer processes rotate reader order.
Every input is warmed immediately before its import. No GPU readback is inserted
in the initial loading path: verification captures follow background preparation
and stationary point refinement. All generated inputs share a unique temp root.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import threading
import time
import urllib.request
import uuid

import psutil
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
VARIANTS = ["legacy", "prototype_copy", "prototype"]


def module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def workloads(root, real_root, small=False):
    base = module(ROOT / 'tests/obj_prototype_benchmark.py', 'cpu_fixture_generator')
    batch, mixed = base.fixtures(root)
    result = [dict(id='small_batch', label='200 small files', category='small-file batch', path=batch),
              dict(id='mixed', label='Weighted polygons + rational curve', category='mixed/freeform', path=mixed)]
    quads = root / 'quad_grid.obj'
    n = 12 if small else 500
    with quads.open('w', encoding='utf-8', newline='\n') as out:
        out.write('g quad_grid\n')
        for y in range(n + 1):
            for x in range(n + 1):
                out.write(f'v {x/n:.9f} {y/n:.9f} {0.1*math.sin(x/n*6)*math.sin(y/n*6):.9f}\n')
        for y in range(n):
            for x in range(n):
                a = y * (n + 1) + x + 1
                out.write(f'f {a} {a+1} {a+n+2} {a+n+1}\n')
    result.append(dict(id='quads', label=f'{n*n:,} quads, no normals', category='polygon triangulation', path=quads))
    lines = root / 'lines_points.obj'
    count = 512 if small else 200000
    with lines.open('w', encoding='utf-8', newline='\n') as out:
        for i in range(count):
            out.write(f'v {(i%500)/500:.6f} {(i//500)/500:.6f} {math.sin(i*.01)*.03:.6f}\n')
        out.write('g polylines\n')
        for start in range(1, count - 63, 64):
            out.write('l ' + ' '.join(str(i) for i in range(start, start+64)) + '\n')
        out.write('g explicit_points\n')
        for i in range(1, count, 25):
            out.write(f'p {i}\n')
    result.append(dict(id='lines', label=f'{count:,} positions, polylines + points', category='lines and explicit points', path=lines))
    surfaces = root / 'trimmed_surfaces.obj'
    patches = 2 if small else 64
    with surfaces.open('w', encoding='utf-8', newline='\n') as out:
        for i in range(patches):
            x, y = i % 8, i // 8
            out.write('vp .25 .25 1\nvp .75 .25 1\nvp .75 .75 1\nvp .25 .75 1\n'
                      'cstype rat bezier\ndeg 1\ncurv2 -4 -3 -2 -1 -4\nparm u 0 1 2 3 4\nend\n')
            out.write(f'v {x} {y} 0 1\nv {x+.9} {y} .1 .75\nv {x} {y+.9} .1 1\nv {x+.9} {y+.9} 0 .75\n')
            out.write(f'g patch_{i}\ncstype rat bezier\ndeg 1 1\nsurf 0 1 0 1 -4 -3 -2 -1\n'
                      'parm u 0 1\nparm v 0 1\nhole 4 0 -1\nend\n')
    result.append(dict(id='trimmed', label=f'{patches} trimmed rational surfaces', category='analytic freeform surfaces', path=surfaces))
    if not small:
        for identity, name, category, relative in (
            ('bus', 'BusGameMap', 'medium attribute-rich scene', 'uploads_files_2720101_BusGameMap.obj'),
            ('cloud', 'Semantic3D · 10 million points', 'positions and explicit point records', 'pointclouds/semantic3d_sg27_station8_10000000_xyz_points.obj'),
            ('bennu', 'Bennu high-resolution mesh', 'large position-only triangles', 'bennu_OLA_v21_PTM_very-high.obj'),
            ('powerplant', 'Powerplant', 'large mesh with UV/normal seams', 'powerplant.obj'),
        ):
            result.append(dict(id=identity, label=name, category=category, path=(real_root / relative).resolve(strict=True)))
    return result


def files(path):
    return sorted(path.glob('*.obj')) if path.is_dir() else [path]


def warm(path):
    for item in files(path):
        with item.open('rb') as source:
            while source.read(4 * 1024 * 1024):
                pass


def digest_input(path):
    value = hashlib.sha256()
    for item in files(path):
        with item.open('rb') as stream:
            value.update(hashlib.file_digest(stream, 'sha256').digest())
    return value.hexdigest()


def overlap(excluded_pid=None):
    guard = module(ROOT / 'experiments/mesh-overlays/run.py', 'load_workflow_guard')
    result = guard.overlapping_workloads(excluded_pid)
    for process in psutil.process_iter(['pid', 'name']):
        if (process.info['name'] or '').lower() in ('cmake.exe', 'woby_obj_prototype_benchmark.exe'):
            result.append(process.info)
    return result


def wait_idle(max_background_cpu=100):
    idle = 0
    while idle < 3:
        active = overlap()
        idle = 0 if active else idle + 1
        if active:
            print(json.dumps(dict(waiting_for=active)), flush=True)
        time.sleep(2)
    if max_background_cpu<100:
        gate=module(ROOT/'experiments/load-workflow/idle.py','workflow_idle')
        gate.wait_for_quiet(lambda:psutil.cpu_percent(interval=.5),max_background_cpu)


def distribution(values):
    values = sorted(values)
    return dict(median=statistics.median(values), minimum=min(values), maximum=max(values),
                p95=values[math.ceil((len(values)-1)*.95)], count=len(values)) if values else None


def one(executable, workload, variant, destination, round_index, guard_enabled=True, pacing='foreground'):
    destination.mkdir(parents=True, exist_ok=False)
    trace = destination / 'trace.json'
    instance = 'load-flow-' + uuid.uuid4().hex[:12]
    command = [str(executable), '--instance', instance, '--drawable-size', '1280x720', '--hidden-window',
               '--log-file', str(destination / 'viewer.log'), '--log-level', 'info']
    env = dict(os.environ, VK_LOADER_LAYERS_DISABLE='~implicit~', WOBY_WORKFLOW_VARIANT=variant,
               WOBY_WORKFLOW_TRACE=str(trace))
    env.pop('WOBY_WORKFLOW_FOREGROUND', None)
    if pacing == 'foreground':
        env['WOBY_WORKFLOW_FOREGROUND'] = '1'
    result = dict(workload=workload['id'], variant=variant, round=round_index, pacing=pacing, status='running',
                  directory=str(destination), memory_samples=[], status_samples=[], interference=[])
    stop = threading.Event()
    stage = ['startup']
    started = time.perf_counter()
    load_start = None
    startup = subprocess.STARTUPINFO() if os.name == 'nt' else None
    if startup:
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with tempfile.TemporaryDirectory(prefix='woby-load-session-') as temporary, (destination / 'console.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen(command, cwd=temporary, env=env, stdout=log, stderr=log, startupinfo=startup)
        probe = psutil.Process(process.pid)

        def monitor():
            last_guard = 0
            while not stop.wait(.02) and process.poll() is None:
                try:
                    now = time.perf_counter()
                    info = probe.memory_info()
                    result['memory_samples'].append(dict(ms=(now-started)*1000, stage=stage[0],
                        private_bytes=getattr(info, 'private', info.rss), working_set_bytes=info.rss,
                        peak_working_set_bytes=getattr(info, 'peak_wset', info.rss),
                        peak_commit_bytes=getattr(info, 'peak_pagefile', getattr(info, 'private', info.rss))))
                    if now-last_guard > .5:
                        last_guard = now
                        if guard_enabled:
                            active = overlap(process.pid)
                            if active:
                                result['interference'].append(dict(ms=(now-started)*1000, processes=active))
                        if getattr(info, 'private', info.rss) > 38*2**30 or psutil.virtual_memory().available < 6*2**30:
                            result['resource_guard'] = 'memory limit'
                            process.terminate()
                        if now-started > 600:
                            result['resource_guard'] = '600 second limit'
                            process.terminate()
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    return

        watcher = threading.Thread(target=monitor, daemon=True)
        watcher.start()
        record = {}
        request_id = 0

        def rpc(method, params=None, timeout=120):
            nonlocal request_id
            request_id += 1
            payload = dict(params or {})
            if method != 'instance.info':
                payload['timeoutSeconds'] = timeout
            request = urllib.request.Request(f"http://127.0.0.1:{record['port']}/rpc",
                data=json.dumps(dict(jsonrpc='2.0', id=request_id, method=method, params=payload)).encode(),
                headers={'Content-Type': 'application/json', 'Authorization': 'Bearer '+record['token']})
            with urllib.request.urlopen(request, timeout=timeout+5) as response:
                value = json.load(response)
            if 'error' in value:
                raise RuntimeError(f"{method}: {value['error']}")
            return value['result']

        try:
            registry = Path(os.environ['LOCALAPPDATA']) / 'woby/instances' / f'instance-{instance}.json'
            deadline = time.perf_counter() + 60
            while time.perf_counter() < deadline:
                if process.poll() is not None:
                    raise RuntimeError('Viewer exited during startup')
                try:
                    record = json.loads(registry.read_text(encoding='utf-8'))
                    if rpc('instance.info').get('ready'):
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.03)
            else:
                raise TimeoutError('Viewer startup')
            result['startup_ms'] = (time.perf_counter()-started)*1000
            rpc('pane.set', dict(visible=False, propertiesVisible=False))
            for helper in ('grid', 'origin', 'dimensions'):
                rpc(helper+'.set', dict(visible=False))
            result['environment_before'] = rpc('performance.get')
            warm(workload['path'])
            stage[0] = 'load'
            load_start = time.perf_counter()
            result['load_started_ms'] = (load_start-started)*1000
            method = 'folder.add' if workload['path'].is_dir() else 'model.add'
            loaded = rpc(method, dict(path=str(workload['path'])), 600)
            result['load_response_ms'] = (time.perf_counter()-load_start)*1000
            result['load_result'] = loaded
            if loaded.get('failedCount') or loaded.get('addedCount') != len(files(workload['path'])):
                raise RuntimeError(f'Import failed: {loaded}')
            result['stats'] = rpc('stats')
            stage[0] = 'background_preparation'
            deadline = time.perf_counter() + 180
            ready_seen = detail_seen = None
            while time.perf_counter() < deadline:
                status = rpc('status')
                performance = rpc('performance.get')
                elapsed = (time.perf_counter()-load_start)*1000
                result['status_samples'].append(dict(ms=elapsed, status=status, points=performance['points']))
                if status['annotationPreparationError']:
                    raise RuntimeError(status['annotationPreparationError'])
                if status['annotationReady'] and ready_seen is None:
                    ready_seen = elapsed
                points = performance['points']
                detail = not points['active'] or points['refinedCount'] == points['sourceCount']
                if detail and detail_seen is None:
                    detail_seen = elapsed
                # Triangle-free inputs do not schedule annotation snapshots.
                annotations = not result['stats']['triangleCount'] or status['annotationReady']
                if annotations and not status['annotationPreparing'] and detail:
                    # Allow two desktop frames for a just-published job to start.
                    if elapsed-result['load_response_ms'] >= 100:
                        break
                time.sleep(.03)
            else:
                raise TimeoutError('Background preparation/refinement')
            result.update(annotation_ready_observed_ms=ready_seen, full_detail_observed_ms=detail_seen,
                          preparation_settled_observed_ms=(time.perf_counter()-load_start)*1000,
                          final_status=status, environment_after=performance)
            stage[0] = 'capture'
            capture_started = time.perf_counter()
            rpc('screenshot.capture', dict(path=str(destination/'verified.png')), 180)
            result['verified_image_ms'] = (time.perf_counter()-load_start)*1000
            result['capture_ms'] = (time.perf_counter()-capture_started)*1000
            with Image.open(destination/'verified.png') as capture:
                pixels = capture.convert('RGB')
                result['image'] = dict(size=list(pixels.size), sha256=hashlib.sha256(pixels.tobytes()).hexdigest())
            stage[0] = 'steady'
            time.sleep(.25)
            rpc('performance.begin')
            time.sleep(1.0)
            result['steady'] = rpc('performance.end')
            rows = result['steady']['events']
            result['steady_summary'] = dict(
                gpu_ms=distribution([r['gpuFrameMilliseconds'] for r in rows if r.get('gpuFrameMilliseconds') is not None]),
                cpu_submit_ms=distribution([r['cpuSubmitMilliseconds'] for r in rows]),
                frame_interval_ms=distribution([b['completedMilliseconds']-a['completedMilliseconds'] for a,b in zip(rows, rows[1:])]))
            stage[0] = 'navigation'
            camera = rpc('camera.get')['camera']
            rpc('performance.begin')
            for view in range(48):
                angle = view * math.tau / 12
                rpc('camera.set', dict(yawDegrees=camera['yawDegrees']+6*math.sin(angle),
                                       pitchDegrees=camera['pitchDegrees']+2*math.cos(angle)))
            result['navigation'] = rpc('performance.end')
            rows = result['navigation']['events']
            result['navigation_summary'] = dict(
                gpu_ms=distribution([r['gpuFrameMilliseconds'] for r in rows if r.get('gpuFrameMilliseconds') is not None]),
                cpu_submit_ms=distribution([r['cpuSubmitMilliseconds'] for r in rows]),
                frame_interval_ms=distribution([b['completedMilliseconds']-a['completedMilliseconds'] for a,b in zip(rows, rows[1:])]))
            result['stats_final'] = rpc('stats')
            stage[0] = 'shutdown'
            rpc('quit', dict(onDirty='discard'))
            if process.wait(timeout=30) != 0:
                raise RuntimeError('Viewer shutdown failed')
            result['trace'] = json.loads(trace.read_text(encoding='utf-8'))
            if result['trace']['variant'] != variant:
                raise RuntimeError('Reader selection mismatch')
            result['status'] = 'interfered' if result['interference'] else 'completed'
        except BaseException as error:
            result['status'] = 'failed'
            result['error'] = repr(error)
        finally:
            stop.set()
            watcher.join(timeout=3)
            if process.poll() is None:
                process.kill()
                process.wait(timeout=15)
            result['elapsed_ms'] = (time.perf_counter()-started)*1000
            (destination/'run.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--models', type=Path, default=Path('D:/temp/obj_tests'))
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--variants', nargs='+', choices=VARIANTS, default=VARIANTS)
    parser.add_argument('--only', nargs='+')
    parser.add_argument('--contract', action='store_true')
    parser.add_argument('--pacing', choices=['foreground', 'background'], default='foreground')
    parser.add_argument('--no-guard', action='store_true', help='Functional checks only; results are not suitable for performance claims')
    parser.add_argument('--max-background-cpu',type=float,default=100,help='Require two quiet CPU samples before starting each viewer')
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('rounds must be positive')
    if not 0<=args.max_background_cpu<=100: parser.error('CPU limit must be between 0 and 100 percent')
    executable = args.executable.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='woby-load-workflow-inputs-') as temporary:
        corpus = workloads(Path(temporary).resolve(), args.models.resolve(), args.contract)
        if args.only:
            corpus = [workload for workload in corpus if workload['id'] in args.only]
        if not corpus:
            parser.error('No workloads selected')
        provenance = dict(revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
            platform=platform.platform(), logical_cpus=psutil.cpu_count(), physical_cpus=psutil.cpu_count(logical=False),
            ram_bytes=psutil.virtual_memory().total, rounds=args.rounds, variants=args.variants,
            guarded=not args.no_guard, max_background_cpu=args.max_background_cpu, hidden_window=True, pacing=args.pacing, viewport=[1280,720], warmup='before every import',
            source_sha256={str(path.relative_to(ROOT)):hashlib.sha256(path.read_bytes()).hexdigest()
                           for folder,pattern in [('src','*.cpp'),('src','*.h'),('experiments/load-workflow','*'),
                                                  ('third_party/rapidobj/include/rapidobj','*.hpp')]
                           for path in (ROOT/folder).glob(pattern) if path.is_file()}, workloads=[])
        for workload in corpus:
            provenance['workloads'].append({**{k:v for k,v in workload.items() if k!='path'}, 'path':str(workload['path']),
                'file_count':len(files(workload['path'])), 'input_bytes':sum(p.stat().st_size for p in files(workload['path'])),
                'input_sha256':digest_input(workload['path'])})
        (output/'manifest.json').write_text(json.dumps(provenance,indent=2),encoding='utf-8')
        for round_index in range(args.rounds):
            rotation = round_index % len(args.variants)
            order = args.variants[rotation:] + args.variants[:rotation]
            for workload in corpus:
                for variant in order:
                    for attempt in range(1,5):
                        if not args.no_guard:
                            wait_idle(args.max_background_cpu)
                        destination = output/f"{workload['id']}-{variant}-r{round_index+1}-a{attempt}"
                        result = one(executable,workload,variant,destination,round_index+1,not args.no_guard,args.pacing)
                        print(json.dumps({k:result.get(k) for k in ('workload','variant','round','status','load_response_ms','error')}),flush=True)
                        with (output/'runs.jsonl').open('a',encoding='utf-8') as log:
                            log.write(json.dumps(dict(workload=workload['id'],variant=variant,round=round_index+1,
                                                      status=result['status'],path=str(destination/'run.json')))+'\n')
                        if result['status'] == 'completed':
                            break
                        if result['status'] != 'interfered':
                            raise RuntimeError(result.get('error', 'Failed run'))
                    else:
                        raise RuntimeError('Four attempts overlapped another workload')


if __name__ == '__main__':
    main()
