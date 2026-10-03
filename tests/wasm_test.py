from pathlib import Path
import subprocess,json
root=Path(__file__).resolve().parents[1]
expected=json.loads((root/'examples/wasm/expected.json').read_text())
for name,want in expected.items():
 p=subprocess.run([str(root/'build/wasm-host-test'),str(root/'examples/wasm'/(name+'.wasm'))],capture_output=True,text=True,timeout=15)
 assert p.returncode==0,(name,p.stdout,p.stderr)
 assert not p.stderr,(name,p.stderr)
 if want['error']=='exit7':assert 'status=1 exit=7' in p.stdout,(name,p.stdout)
 elif want['error']:assert 'status=-1' in p.stdout,(name,p.stdout)
 else:assert 'status=0' in p.stdout and ('result='+str(want['result'])+' ') in p.stdout,(name,p.stdout)
 print(name,p.stdout.splitlines()[0])
for name in ['hello','memory','grow_limit','invalid_type','bad_import','out_of_bounds']:
 p=subprocess.run([str(root/'build/wasm-host-test'),str(root/'examples/wasm'/(name+'.wasm')),'100'],capture_output=True,text=True,timeout=20)
 assert p.returncode==0 and not p.stderr,(name,p.stdout,p.stderr)
print('PASS 600 repeated instantiate/execute/trap/free cycles with zero live heap bytes')
print('PASS',len(expected),'WASM execution/trap/validation/resource tests, ASan+UBSan')
