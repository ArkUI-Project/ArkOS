// Independent development oracle; never shipped as an ArkOS runtime service.
const fs=require('fs'),path=require('path');
const dir=path.join(__dirname,'../examples/wasm');
const expected=JSON.parse(fs.readFileSync(path.join(dir,'expected.json')));let count=0;
for(const [name,want] of Object.entries(expected)){
 const bytes=fs.readFileSync(path.join(dir,name+'.wasm'));
 const valid=WebAssembly.validate(bytes);
 if(['invalid_type','truncated'].includes(name)){if(valid)throw Error(name+' unexpectedly valid');count++;continue;}
 if(!valid)throw Error(name+' unexpectedly invalid');
 if(want.error){count++;continue;}
 let instance;const imports={ark:{log:()=>{},ticks:()=>0n},wasi_snapshot_preview1:{fd_write:(fd,p,n,out)=>{const mem=new DataView(instance.exports.memory.buffer);let total=0;for(let i=0;i<n;i++)total+=mem.getUint32(p+i*8+4,true);mem.setUint32(out,total,true);return 0;}}};
 instance=new WebAssembly.Instance(new WebAssembly.Module(bytes),imports);
 const actual=instance.exports.main();if(actual!==want.result)throw Error(name+': '+actual+' != '+want.result);count++;
}
console.log('PASS independent WebAssembly engine validation/results for '+count+' fixtures');
