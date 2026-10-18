import ctypes,hashlib,json,subprocess,sys,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
name,variant,cell,limit,mode=sys.argv[1:6]
dest=root/'measurements'/name
dest.mkdir(exist_ok=False)
exe=root/'bin/Release'/('gm_difference_'+variant+'.exe')
args=[str(exe),'D:/temp/GM-data/test.woby','D:/temp/GM-data/importers',cell,limit,mode,*sys.argv[6:]]
meta={'command':args,'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'start_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())}
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.GetProcessTimes.argtypes=[ctypes.c_void_p]+[ctypes.c_void_p]*4
samples=[]
with (dest/'stdout.jsonl').open('w') as out,(dest/'stderr.jsonl').open('w') as err:
 start=time.perf_counter()
 proc=subprocess.Popen(args,stdout=out,stderr=err,cwd=root)
 while proc.poll() is None:
  create=ctypes.c_uint64();exit_time=ctypes.c_uint64();kernel=ctypes.c_uint64();user=ctypes.c_uint64()
  if k.GetProcessTimes(int(proc._handle),ctypes.byref(create),ctypes.byref(exit_time),ctypes.byref(kernel),ctypes.byref(user)):
   samples.append({'wall_seconds':time.perf_counter()-start,'process_cpu_seconds':(kernel.value+user.value)/1e7})
  if time.perf_counter()-start>float(limit)+180:
   proc.kill();meta['hard_killed']=True;break
  time.sleep(.5)
 meta['exit_code']=proc.wait();meta['process_wall_seconds']=time.perf_counter()-start
meta['samples']=samples
(dest/'metadata.json').write_text(json.dumps(meta,indent=2))
lines=(dest/'stdout.jsonl').read_text().splitlines()
print(json.dumps({'run':name,'exit':meta['exit_code'],'last_output':json.loads(lines[-1]) if lines else None}),flush=True)
