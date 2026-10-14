"""Export Nsight event counts, target allocations and aligned NVTX ranges."""
import argparse
import json
from pathlib import Path
import sqlite3
import subprocess


def process_id(value):
    try:
        return int(str(value), 16 if str(value).lower().startswith('0x') else 10)
    except (TypeError, ValueError):
        return None


def event_association(pid, global_tid, data):
    emitter = ((global_tid >> 24) & 16777215) == pid
    payload = any(process_id(data.get(key)) == pid for key in ('ProcessId', 'ProcessID', 'hProcessId', 'PID'))
    return 'emitter+payload' if emitter and payload else 'payload' if payload else 'emitter' if emitter else None


def pair_packets(starts, stops, keys):
    """Pair raw queue events by their identifiers; retain all unmatched evidence."""
    pending = {}
    for stop in stops:
        pending.setdefault(tuple(stop[k] for k in keys), []).append(stop)
    for values in pending.values():
        values.sort(key=lambda row: row['start'])
    pairs, unmatched = [], []
    for start in sorted(starts, key=lambda row: row['start']):
        values = pending.get(tuple(start[k] for k in keys), [])
        match = next((i for i, stop in enumerate(values) if stop['start'] >= start['start']), None)
        if match is None:
            unmatched.append(start)
        else:
            stop = values.pop(match)
            pairs.append({**start, 'stop_ns': stop['start'], 'duration_ms': (stop['start']-start['start'])/1e6,
                          'stop_global_tid': stop['globalTid'], 'stop_preempted': stop.get('preempted'),
                          'stop_timeouted': stop.get('timeouted'), 'stop_packet_type': stop.get('packetType')})
    return {'pairs': pairs, 'unmatched_starts': unmatched, 'unmatched_stops': [v for values in pending.values() for v in values]}


def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('root',type=Path); args=parser.parse_args()
    root=args.root.resolve()
    for case in ('nsys-retry','nsys-wddm-retry','nsys-wddm-full-retry'):
        directory=root/'deep'/case; report=directory/'timeline.nsys-rep'; database=directory/'timeline.sqlite'
        if not report.exists(): continue
        if not database.exists() or database.stat().st_size==0:
            nsys=r'C:\Program Files\NVIDIA Corporation\Nsight Systems 2026.5.1\target-windows-x64\nsys.exe'
            with (directory/'export-retry.log').open('w') as log:
                p=subprocess.run([nsys,'export','--type=sqlite','--force-overwrite=true','--output='+str(database),str(report)],stdout=log,stderr=subprocess.STDOUT,timeout=900)
            if p.returncode:
                print('Export failed '+case,flush=True); continue
        viewer=json.loads((directory/'result.json').read_text())
        with sqlite3.connect('file:'+str(database)+'?mode=ro',uri=True) as db:
            tables=[r[0] for r in db.execute("select name from sqlite_master where type='table'")]
            result={'case':case,'tables':tables,'target_pid':viewer.get('pid'),'capture_error':viewer.get('error')}
            if 'NVTX_EVENTS' in tables:
                rows=db.execute('select start,end,text,globalTid from NVTX_EVENTS where end is not null').fetchall()
                result['nvtx_ranges']=[{'start_ns':a,'end_ns':b,'name':name,'global_tid':tid} for a,b,name,tid in rows]
                phase=next((p for p in viewer.get('trace',[]) if p['kind']=='phase' and p['name']=='cpu_import'),None)
                match=next((r for r in rows if r[2]=='cpu_import'),None)
                if phase and match: result['nvtx_to_phase_offset_ms']=match[0]/1e6-phase['start_ms']
            if 'TARGET_INFO_GPU' in tables:
                result['gpu_inventory']=db.execute('select id,name,busLocation,luid from TARGET_INFO_GPU').fetchall()
            if 'ETW_EVENTS' in tables:
                result['etw_event_types']=db.execute('''select t.typeId,s.value,count(*) from ETW_EVENTS e join GENERIC_EVENT_TYPES t using(typeId) join StringIds s on s.id=t.nameId group by t.typeId,s.value order by count(*) desc''').fetchall()
                result['etw_events_total']=db.execute('select count(*) from ETW_EVENTS').fetchone()[0]
                # Keep target events only. Other process activity stays in the native session.
                pid=viewer.get('pid',0)
                rows=db.execute('''select e.timestamp,s.value,e.opcode,e.data,e.globalTid from ETW_EVENTS e join GENERIC_EVENT_TYPES t using(typeId) join StringIds s on s.id=t.nameId''')
                result['target_etw_events']=[]
                for timestamp,name,opcode,data,tid in rows:
                    payload=json.loads(data); association=event_association(pid,tid,payload)
                    if association:
                        result['target_etw_events'].append({'time_ns':timestamp,'name':name,'opcode':opcode,'data':payload,
                                                          'global_tid':tid,'association':association})
            if 'MEMORY_TRANSFER_EVENTS' in tables:
                result['memory_transfers_total']=db.execute('select count(*),sum(size) from MEMORY_TRANSFER_EVENTS').fetchone()
                pid=viewer.get('pid',0)
                columns=[r[1] for r in db.execute('pragma table_info(MEMORY_TRANSFER_EVENTS)')]
                result['target_memory_transfers']=[dict(zip(columns,row)) for row in db.execute('select * from MEMORY_TRANSFER_EVENTS where ((globalTid>>24)&16777215)=?',[pid])]
                result['transfer_types']=db.execute('select id,name,label from ENUM_ETW_MEMORY_TRANSFER_TYPE').fetchall()
            if 'WDDM_QUEUE_PACKET_START_EVENTS' in tables:
                def records(table):
                    names=[r[1] for r in db.execute('pragma table_info('+table+')')]
                    return [dict(zip(names,row)) for row in db.execute('select * from '+table)]
                starts=records('WDDM_QUEUE_PACKET_START_EVENTS')
                contexts={row['context'] for row in starts if ((row['globalTid']>>24)&16777215)==pid}
                result['target_queue_contexts']=sorted(contexts)
                for kind in ('QUEUE','DMA'):
                    starts=records('WDDM_'+kind+'_PACKET_START_EVENTS'); stops=records('WDDM_'+kind+'_PACKET_STOP_EVENTS')
                    starts=[s for s in starts if s['context'] in contexts]; stops=[s for s in stops if s['context'] in contexts]
                    # Synchronization starts use UINT_MAX for packetType; the stop
                    # supplies its real type. Context, pointer and sequence identify it.
                    keys=('gpu','context','queuePacket','submitSequence') if kind=='QUEUE' else ('gpu','context','ulQueueSubmitSequence')
                    result[kind.lower()+'_packets']=pair_packets(starts,stops,keys)
                result['engine_types']=db.execute('select id,name,label from ENUM_WDDM_ENGINE_TYPE').fetchall()
                result['context_inventory']=db.execute('select * from TARGET_INFO_WDDM_CONTEXTS').fetchall()
        (directory/'timeline-analysis.json').write_text(json.dumps(result,indent=2))
        print(case, 'NVTX',len(result.get('nvtx_ranges',[])),'ETW',result.get('etw_events_total'), 'target ETW',len(result.get('target_etw_events',[])),flush=True)


if __name__=='__main__': main()
