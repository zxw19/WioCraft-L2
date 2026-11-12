const fs=require('fs'),assert=require('assert').strict;
(async()=>{
 const {instance}=await WebAssembly.instantiate(fs.readFileSync('tests/physics.wasm'),{
   env:{sqrtf:Math.sqrt,ceilf:Math.ceil,floorf:Math.floor,fabsf:Math.abs}
 });
 const w=instance.exports;
 for(const dt of [.016,.033,.1]) {
   w.reset(1,0);let peak=1;
   for(let t=0;t<1;t+=dt){w.step(dt,t===0?1:0,0);peak=Math.max(peak,w.value(1));}
   assert(peak>2.12&&peak<2.22,`jump reaches 1.12m at dt ${dt}: ${peak-1}`);
   w.reset(1,0);
   for(let t=0;t<1.3;t+=dt)w.step(dt,t===0?1:0,1);
   assert(w.value(0)>3&&w.value(1)>1.99,`escapes one-block hole at dt ${dt}: x=${w.value(0)} y=${w.value(1)}`);
   w.reset(2,0);
   for(let t=0;t<1.3;t+=dt)w.step(dt,t===0?1:0,1);
   assert(w.value(0)<1.71,`two-block wall stays solid at dt ${dt}: x=${w.value(0)}`);
   w.reset(1,1);let headPeak=1;
   for(let t=0;t<1;t+=dt){w.step(dt,t===0?1:0,0);headPeak=Math.max(headPeak,w.value(1));}
   assert(headPeak<1.22,`ceiling collision at dt ${dt}: ${headPeak}`);
   console.log(`PASS physics dt=${dt}: jump=${(peak-1).toFixed(3)}m, one-block escape, two-block wall, ceiling collision`);
 }
})().catch(e=>{console.error(e);process.exitCode=1;});
