"""Validate matched workloads and summarize fresh-process workflow measurements."""
import argparse
import json
import math
from pathlib import Path
import statistics

PHASES = ['capacity_preflight', 'parse', 'legacy_freeform_reparse', 'freeform_resolve', 'mesh_validate',
          'source_adoption', 'coordinate_localization', 'triangulation', 'mesh_mapping', 'normals', 'mesh_bounds',
          'freeform_tessellation_append', 'ui_group_preparation', 'gpu_input_validation', 'point_membership',
          'point_hierarchy', 'annotation_blocks_fingerprints', 'annotation_spatial_index', 'annotation_snapshot_copy']
COUNTS = ['fileCount', 'vertexCount', 'triangleCount', 'lineSegmentCount', 'pointCount', 'groupCount']


def values(items):
    items = list(items)
    if not items or any(not math.isfinite(x) or x < 0 for x in items):
        raise ValueError('Missing, non-finite, or negative measurement')
    return dict(median=statistics.median(items), minimum=min(items), maximum=max(items), samples=len(items))


def summarize_run(run):
    if run['status'] != 'completed' or run.get('interference'):
        raise ValueError('Unsuccessful or interfered run cannot enter a comparison')
    if run['trace']['variant'] != run['variant']:
        raise ValueError('Variant mismatch')
    events = run['trace']['events']
    for event in events:
        if not all(math.isfinite(event[k]) and event[k] >= 0 for k in ('duration_ms','start_ms','end_ms')):
            raise ValueError('Invalid event timing')
        if event['end_ms'] < event['start_ms']:
            raise ValueError('Reversed event timing')

    def named(name):
        return [event for event in events if event['name'] == name]

    def unique(name):
        found = named(name)
        if len(found) != 1:
            raise ValueError(f'Expected one {name}, found {len(found)}')
        return found[0]

    start = unique('load_requested')['end_ms']
    commit = unique('scene_committed')['end_ms']
    submitted = unique('first_scene_frame_submitted')['end_ms']
    upload_start = unique('gpu_finalize_begin')['end_ms']
    if not start <= upload_start <= commit <= submitted:
        raise ValueError('Invalid workflow milestone order')
    annotation_end = max((event['end_ms'] for event in named('annotation_published')), default=commit)
    phases = {name:sum(event['duration_ms'] for event in named(name)) for name in PHASES}
    mesh = named('mesh_ready')
    if len(mesh) != run['stats']['fileCount']:
        raise ValueError('Mesh count does not match the published scene')
    if sum(row['data']['vertices'] for row in mesh) != run['stats']['vertexCount']:
        raise ValueError('Vertex count does not match the published scene')
    geometry = {key:run['stats'][key] for key in COUNTS}
    capacities = {}
    for event in mesh:
        for key,value in event['data'].items():
            if key.endswith('_bytes') or key == 'source_positions':
                capacities[key] = capacities.get(key,0) + value
    prepared = {}
    for event in named('gpu_input_ready'):
        for key,value in event['data'].items():
            if key.endswith('_bytes') or key in ('cloud_points','cloud_proxies'):
                prepared[key] = prepared.get(key,0) + value
    annotations = {}
    for event in named('annotation_snapshot_copy'):
        for key,value in event['data'].items():
            if key.endswith('_bytes'):
                annotations[key] = annotations.get(key,0) + value
    copied = sum(event['data']['position_bytes'] for event in named('source_adoption') if event['data']['copied'])
    import_samples = [row for row in run['memory_samples'] if row['stage'] in ('load','background_preparation')]
    if not import_samples:
        raise ValueError('No memory samples during loading')
    timings = dict(first_submitted_ms=submitted-start, scene_commit_ms=commit-start,
        cpu_batch_ms=unique('cpu_batch_total')['duration_ms'], gpu_finalize_wall_ms=commit-upload_start,
        upload_cpu_ms=sum(row['data']['cpu_ms'] for row in named('gpu_upload')),
        fully_prepared_ms=max(submitted, annotation_end)-start,
        annotation_publish_after_commit_ms=max(0,annotation_end-commit),
        load_response_ms=run['load_response_ms'], full_detail_observed_ms=run['full_detail_observed_ms'],
        preparation_settled_observed_ms=run['preparation_settled_observed_ms'],
        verified_image_ms=run['verified_image_ms'], capture_ms=run['capture_ms'], startup_ms=run['startup_ms'])
    values(timings.values())
    memory = dict(import_peak_commit_bytes=max(row['peak_commit_bytes'] for row in import_samples),
        import_peak_working_set_bytes=max(row['peak_working_set_bytes'] for row in import_samples),
        prepared_private_bytes=import_samples[-1]['private_bytes'],
        lifecycle_peak_commit_bytes=max(row['peak_commit_bytes'] for row in run['memory_samples']),
        copied_position_bytes=copied,
        source_adoption_private_bytes=max((row['memory'].get('private_bytes',0) for row in named('source_adoption')),default=0))
    return dict(workload=run['workload'], variant=run['variant'], round=run['round'], geometry=geometry,
        image=run['image'], timings=timings, phases=phases, memory=memory, mesh_capacities=capacities,
        preparation_capacities=prepared, annotation_capacities=annotations,
        steady=run['steady_summary'], navigation=run.get('navigation_summary'),
        pacing=run['environment_after']['pacing'], annotation_ready=run['final_status']['annotationReady'],
        source=run['directory'])


def compare(runs, manifest):
    summaries = []
    for workload in manifest['workloads']:
        selected = [run for run in runs if run['workload'] == workload['id']]
        if not selected:
            raise ValueError(f"Missing workload: {workload['id']}")
        # Hash decoded pixels, not PNG container bytes. Stats and exact visible
        # output must agree before timings are combined across readers.
        for field in ('geometry','image','mesh_capacities','preparation_capacities','annotation_capacities'):
            expected = selected[0][field]
            if any(run[field] != expected for run in selected):
                raise ValueError(f"Mismatched {field}: {workload['id']}")
        result = dict(workload=workload, geometry=selected[0]['geometry'], image=selected[0]['image'],
            mesh_capacities=selected[0]['mesh_capacities'], preparation_capacities=selected[0]['preparation_capacities'],
            annotation_capacities=selected[0]['annotation_capacities'], variants={})
        for variant in manifest['variants']:
            group = [run for run in selected if run['variant']==variant]
            if len(group)!=manifest['rounds'] or {r['round'] for r in group}!=set(range(1,manifest['rounds']+1)):
                raise ValueError(f"Missing or repeated rounds: {workload['id']} / {variant}")
            metrics = {field:{key:values(row[field][key] for row in group) for key in group[0][field]}
                       for field in ('timings','phases','memory')}
            metrics['render'] = {}
            for field in ('steady','navigation'):
                for key in ('gpu_ms','cpu_submit_ms','frame_interval_ms'):
                    available = [row[field][key]['median'] for row in group if row.get(field) and row[field].get(key)]
                    metrics['render'][field+'_'+key] = values(available) if available else None
            metrics['pacing'] = group[0]['pacing']
            metrics['annotation_ready'] = all(row['annotation_ready'] for row in group)
            result['variants'][variant] = metrics
        summaries.append(result)
    return summaries


def load(directory, allow_unguarded=False):
    manifest = json.loads((directory/'manifest.json').read_text(encoding='utf-8'))
    if not manifest['guarded'] and not allow_unguarded:
        raise ValueError('Unguarded functional runs are not performance evidence')
    index = [json.loads(line) for line in (directory/'runs.jsonl').read_text(encoding='utf-8').splitlines()]
    runs = [summarize_run(json.loads(Path(item['path']).read_text(encoding='utf-8')))
            for item in index if item['status']=='completed']
    return dict(manifest=manifest, runs=runs, comparisons=compare(runs,manifest),
                excluded=[row for row in index if row['status']!='completed'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    parser.add_argument('--allow-unguarded',action='store_true')
    args=parser.parse_args()
    result=load(args.directory.resolve(),args.allow_unguarded)
    (args.directory/'summary.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    for model in result['comparisons']:
        print(model['workload']['id'], {name:round(row['timings']['fully_prepared_ms']['median'],1)
                                      for name,row in model['variants'].items()})


if __name__=='__main__':
    main()
