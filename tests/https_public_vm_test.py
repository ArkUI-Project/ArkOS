#!/usr/bin/env python3
"""Actual production browser and public roots, read-only request to example.com."""
import hashlib,json,time
from PIL import Image
from vm import VM,ROOT
v=VM('production-https-public',device='virtio-multitouch-pci,virtio-tablet-pci')
try:
 v.enroll_test_user();v.terminal();v.command('pkg run ark.browser');v.wait('[app] Browser ring3 ready');v.wait('[ui] open Browser');time.sleep(1);v.key('ctrl-l');v.type('https://example.com/');v.key('ret');v.wait('[permission] Consent requested by browser');v.key('ret');v.wait('[permission] Allowed browser');v.wait('[net] HTTP done status=200',60);v.screen('https-example');Image.open(v.out/'https-example.ppm').save(v.out/'https-example.png');assert 'User fault' not in v.log.read_text();result={'url':'https://example.com/','production_trust_anchors':True,'result':'PASS','kernel_sha256':hashlib.sha256((ROOT/'build/kernel.elf').read_bytes()).hexdigest()};(v.out/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(result,flush=True)
finally:v.close()
