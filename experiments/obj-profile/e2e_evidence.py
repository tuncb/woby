"""Reduce controls and profiler records to auditable numerical tables."""
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import statistics

from e2e_analyze import distribution, phases, summarize_run, first_marks, timechart_series


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def vtune_rows(path, first_column):
    text = path.read_text(encoding='utf-8')
    start = text.index(first_column + '\tCPU Time\t')
    return [row for row in csv.DictReader(text[start:].splitlines(), delimiter='\t')
            if row.get('CPU Time') and not row[first_column].startswith('vtune:')]


def controls(root):
    result = {}
    for name, phase in (('parser', 'parser_total'), ('reads', 'read_only'), ('copy', 'memory_copy')):
        records = read_json(root/'controls'/(name+'.json'))['records']
        groups = defaultdict(list)
        for record in records:
            groups[(record['mode'], record['workers_requested'])].append(record)
        rows = []
        for (mode, workers), records in sorted(groups.items()):
            events = [next(e for e in r['events'] if e['name']==phase) for r in records]
            row = {'mode': mode, 'workers': workers, 'wall_ms': distribution([e['wall_ms'] for e in events]),
                   'average_logical_cpus': distribution([e['average_logical_cpus'] for e in events]),
                   'peak_working_set_bytes': distribution([max(e['process_peak_working_set'] for e in r['events']) for r in records]),
                   'fingerprints': sorted({r.get('geometry',{}).get('geometry_fingerprint',r.get('geometry',{}).get('fingerprint')) for r in records
                                           if r.get('geometry',{}).get('geometry_fingerprint',r.get('geometry',{}).get('fingerprint')) is not None})}
            if name=='copy':
                row['payload_GB_per_second'] = distribution([r['stats']['payload_GB_per_second'] for r in records])
                row['minimum_read_plus_write_GB_per_second'] = distribution([r['stats']['minimum_read_plus_write_GB_per_second'] for r in records])
            rows.append(row)
        result[name]=rows
    lines=(root/'controls/upload-capacity.log').read_text().splitlines()
    first=next(i for i,line in enumerate(lines) if line.startswith('round,mode,'))
    raw=list(csv.DictReader(lines[first:])); result['upload_verified_samples']=sum(r['verified']=='true' for r in raw)
    result['upload_total_samples']=len(raw); result['upload']=[]
    for mode in ('burst','overlap'):
        for capacity in (4,8,16,32,64,128,256):
            selected=[r for r in raw if r['mode']==mode and int(r['capacity_mib'])==capacity and int(r['round'])>0]
            result['upload'].append({'mode':mode,'capacity_mib':capacity,
                                    **{field:distribution([float(r[field]) for r in selected])
                                       for field in ('allocation_ms','upload_ms','submissions','waits','peak_mib')}})
    diagnostic=read_json(root/'controls/diagnostic.json')['records'][0]
    result['mapping_diagnostic']=next(e['data'] for e in diagnostic['events'] if e['name']=='mapping_staged_diagnostic')
    result['geometry']=diagnostic['geometry']; result['parser_stats']=diagnostic['stats']
    result['instructions']=read_json(root/'controls/instruction-analysis.json')
    instruction_record=read_json(root/'controls/instructions.json')['records'][0]
    result['instruction_observer']={key:instruction_record[key] for key in
                                    ('sampler_cpu_ms','inventory_wall_ms','context_wall_ms','thread_inventories','suspend_failures')}
    calibration=read_json(root/'controls/calibration/uprof-flop-control-sse2-fp-analysis.json')
    result['fp_calibration']=calibration['stages'][0]
    result['fp_calibration']['expected_flops']=536870912
    result['fp_calibration']['expected_retired_loop_instructions']=402653184
    return result


def pacing(root):
    manifest=read_json(root/'pacing/manifest.json'); rows=[]
    for item in manifest['runs']:
        row=summarize_run(Path(item['output'])/'result.json')
        row.update(round=item['round'],configuration=item['configuration']); rows.append(row)
    return rows


def telemetry(record):
    result={}
    for name,begin,end in (('full_recording',0,record['duration_ms']),
                           ('load',record['load_request_ms'],record['load_request_ms']+record.get('load_rpc_ms',0)),
                           ('settled_render',record.get('render_started_ms',0),record.get('render_started_ms',0)+record.get('render_duration_ms',0))):
        system=[s['values'] for s in record.get('system_samples',[]) if begin<=s['time_ms']<end]
        gpu=[s for s in record.get('gpu',{}).get('samples',[]) if begin<=s['time_ms']<end]
        numbers={key:distribution([s.get(key) for s in system if isinstance(s.get(key),(float,int))])
                 for key in sorted({k for s in system for k,v in s.items() if isinstance(v,(float,int))})}
        for key in ('gpu_process_dedicated_bytes','gpu_process_shared_bytes'):
            numbers[key]=distribution([sum(v['value'] for v in s[key]) for s in system if isinstance(s.get(key),list)])
        numbers['gpu_counter_errors']=Counter(str(s.get('gpu_engines',{}).get('error')) for s in system if isinstance(s.get('gpu_engines'),dict))
        numbers['nvml']={key:distribution([s.get(key) for s in gpu]) for key in
                         ('gpu_percent','memory_used','temperature_C','power_W','sm_clock_MHz','memory_clock_MHz',
                          'pcie_rx_KB_per_second','pcie_tx_KB_per_second','pcie_link_generation','pcie_link_width')}
        numbers['clock_event_reasons']=dict(Counter(str(s.get('clock_event_reasons')) for s in gpu))
        result[name]=numbers
    return result


def memory_inventory(record):
    result=[]
    for checkpoint in record['memory_maps']:
        allocations=checkpoint['allocations']; wc=[a for a in allocations if any(p&0x400 for p in a['protections'])]
        result.append({'name':checkpoint['name'],'scan_ms':checkpoint['duration_ms'],
                       'private_bytes':checkpoint['private_bytes'],'working_set_bytes':checkpoint['working_set_bytes'],
                       'write_combined_committed_bytes':sum(a['committed_bytes'] for a in wc),
                       'write_combined_valid_page_bytes':sum(a.get('valid_pages',0)*4096 for a in wc),
                       'write_combined_allocations':len(wc),'largest_allocations':allocations[:16],
                       'totals':checkpoint['totals'],'page_query':checkpoint['page_query']})
    return result


def nsys_summary(directory):
    data=read_json(directory/'timeline-analysis.json'); record=read_json(directory/'result.json')
    result={key:data.get(key) for key in ('case','target_pid','capture_error','etw_events_total',
                                         'memory_transfers_total','gpu_inventory','nvtx_to_phase_offset_ms','target_queue_contexts')}
    result['target_events']=len(data.get('target_etw_events',[]))
    result['target_event_names']=Counter(e['name'] for e in data.get('target_etw_events',[]))
    result['association_counts']=Counter(e.get('association') for e in data.get('target_etw_events',[]))
    result['nvtx_count']=len(data['nvtx_ranges']); result['phase_alignment_max_error_ms']=None
    offset=data.get('nvtx_to_phase_offset_ms',0)
    errors=[]; phase_rows=[]
    for phase in phases(record):
        if phase['wall_ms']<100 or 'worker' in phase['name']: continue
        matches=[r for r in data['nvtx_ranges'] if r['name']==phase['name']]
        if matches: errors.append(min(abs(r['start_ns']/1e6-offset-phase['start_ms']) for r in matches))
        begin=(phase['start_ms']+offset)*1e6; end=(phase['end_ms']+offset)*1e6
        transfers=[t for t in data.get('target_memory_transfers',[]) if begin<=t['start']<end]
        transfer_groups=defaultdict(lambda:{'events':0,'bytes':0})
        for transfer in transfers:
            key=str(transfer['memoryTransferType']); transfer_groups[key]['events']+=1; transfer_groups[key]['bytes']+=transfer['size']
        queue=[p for p in data.get('queue_packets',{}).get('pairs',[]) if begin<=p['start']<end]
        phase_rows.append({'name':phase['name'],'wall_ms':phase['wall_ms'],'transfers':dict(transfer_groups),
                           'queue_packets':len(queue),'queue_duration_ms':distribution([p['duration_ms'] for p in queue])})
    if errors: result['phase_alignment_max_error_ms']=max(errors)
    result['phases']=phase_rows
    for kind in ('queue','dma'):
        packets=data.get(kind+'_packets',{})
        result[kind]={'matched':len(packets.get('pairs',[])),
                      'unmatched_starts':len(packets.get('unmatched_starts',[])),
                      'unmatched_stops':len(packets.get('unmatched_stops',[])),
                      'duration_ms':distribution([p['duration_ms'] for p in packets.get('pairs',[])]),
                      'engine_types':Counter(str(p['engineType']) for p in packets.get('pairs',[])),
                      'preempted_stops':sum(bool(p.get('stop_preempted')) for p in packets.get('pairs',[])),
                      'timeout_stops':sum(bool(p.get('stop_timeouted')) for p in packets.get('pairs',[]))}
    types={str(key):label for key,name,label in data.get('transfer_types',[])}
    transfer_rows=[]
    for key in sorted({t['memoryTransferType'] for t in data.get('target_memory_transfers',[])}):
        rows=[t for t in data['target_memory_transfers'] if t['memoryTransferType']==key]
        transfer_rows.append({'type':key,'label':types.get(str(key),str(key)),'count':len(rows),'bytes':sum(t['size'] for t in rows)})
    result['transfer_rows']=transfer_rows; result['engine_type_names']=data.get('engine_types',[])
    result['owned_memory_events']=[e for e in data.get('target_etw_events',[]) if e.get('association') in ('payload','emitter+payload')
                                  and e['name'] in ('VidMmProcessCommitmentChange','VidMmProcessUsageChange','ProcessAllocationDetails')]
    result['driver_memory']=[]
    groups=defaultdict(list)
    for event in result['owned_memory_events']:
        if event['name']=='ProcessAllocationDetails': continue
        payload=event['data']; field='NewUsage' if event['name']=='VidMmProcessUsageChange' else 'Commitment'
        groups[(event['name'],payload['pDxgAdapter'],payload['MemorySegmentGroup'])].append(
            {'time_ms':event['time_ns']/1e6-offset-first_marks(record['trace'])['model_add_requested'],
             'bytes':int(payload[field])})
    for (event,adapter,segment),values in groups.items():
        values.sort(key=lambda v:v['time_ms'])
        result['driver_memory'].append({'event':event,'adapter':adapter,'segment':segment,
                                        'peak_bytes':max(v['bytes'] for v in values),'final_bytes':values[-1]['bytes'],'samples':values})
    return result


def all_evidence(root):
    timechart_record=read_json(root/'deep/timechart/result.json')
    series=timechart_series(root/'deep/timechart/timechart/timechart.csv',timechart_record)
    origin=(timechart_record['trace'][0]['steady_epoch_nanoseconds']-timechart_record['observer_epoch_qpc_ns'])/1e6
    phase_power=[]
    for phase in phases(timechart_record):
        if phase['wall_ms']<100 or 'worker' in phase['name']: continue
        samples=[s for s in series if phase['start_ms']+origin<=s['time_ms']<phase['end_ms']+origin]
        phase_power.append({'name':phase['name'],'samples':len(samples),
                            'mean_lp_frequency_MHz':distribution([s['mean_effective_frequency_MHz'] for s in samples]),
                            'package_power_W':distribution([s['package_power_W'] for s in samples])})
    root_native=root/'native'; native_records=[read_json(p) for p in root_native.glob('*/result.json')]
    return {'controls':controls(root),'pacing':pacing(root),
            'production_functions':read_json(root/'deep/production-branch/production-analysis.json'),
            'vtune_functions':vtune_rows(root/'deep/vtune/hotspots-function.csv','Function'),
            'vtune_threads':vtune_rows(root/'deep/vtune/hotspots-thread.csv','Thread'),
            'nsys':[nsys_summary(p.parent) for p in (root/'deep').glob('*retry/timeline-analysis.json')],
            'memory':memory_inventory(read_json(root/'deep/memory/result.json')),
            'native_telemetry':{str(r['command'][-1])+':'+str(r['pid']):telemetry(r) for r in native_records},
            'timechart_phase_power':phase_power}
