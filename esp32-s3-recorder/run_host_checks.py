from pathlib import Path
import subprocess,json,hashlib,tempfile
from concurrent.futures import ThreadPoolExecutor
root=Path(__file__).resolve().parent
items=[('signal_core_test',[]),('recorder_ui_test',[]),('record_store_test',['-Wno-unused-private-field','-I'+str(root/'tests/store_stubs')]),('display_adapter_test',['-I'+str(root/'tests/display_stubs')])]
def run(item):
 name,flags=item
 with tempfile.TemporaryDirectory(prefix='mc2v05-host-') as tmp:
  cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-pedantic',*flags,str(root/'tests'/(name+'.cpp')),'-o',str(Path(tmp)/name)]
  p=subprocess.run(cmd,capture_output=True,text=True)
  if p.returncode: return {'test':name,'command':cmd,'success':False,'stderr':p.stderr}
  p=subprocess.run([str(Path(tmp)/name)],capture_output=True,text=True)
  return {'test':name,'command':cmd,'success':p.returncode==0,'stdout':p.stdout,'stderr':p.stderr}
with ThreadPoolExecutor(max_workers=4) as ex: results=list(ex.map(run,items))
p=subprocess.run(['python3',str(root/'tests/app_integration_test.py')],capture_output=True,text=True)
results.append({'test':'actual-main-integration','success':p.returncode==0,'stdout':p.stdout,'stderr':p.stderr})
receipt={'results':results,'all_pass':all(x['success'] for x in results),'host_mock_note':'The store worker runs synchronously only in its host mock, so unused worker handle warning is suppressed in that test only. Production native build uses real FreeRTOS task.','sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'firmware/MC2_Live').iterdir() if p.suffix in {'.ino','.h'}}}
(root/'evidence/host-checks.json').write_text(json.dumps(receipt,indent=2))
print(json.dumps({'all_pass':receipt['all_pass'],'tests':[{k:x[k] for k in ('test','success','stdout','stderr') if k in x} for x in results]},indent=2))
assert receipt['all_pass']
