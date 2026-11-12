const fs=require('fs'),assert=require('assert').strict;
(async()=>{
 const env={sqrtf:Math.sqrt,sinf:Math.sin,cosf:Math.cos,floorf:Math.floor,ceilf:Math.ceil,fabsf:Math.abs,powf:Math.pow};
 const load=async name=>(await WebAssembly.instantiate(fs.readFileSync(name),{env})).instance.exports;
 const a=await load('tests/game.wasm'),b=await load('tests/reference.wasm');
 a.init();b.init();
 let differences=0,total=0;
 for(let i=0;i<100;i++){
   const x=52.5+(i%7)*.37,z=52.5+(i%11)*.32;
   const y=Math.max(a.heightAt(Math.floor(x),Math.floor(z))+1.62,34.62)+(i%4)*.5;
   const cam=[x,y,z,i*.174, -8+(i*7)%140];
   const pa=a.draw(...cam),pb=b.draw(...cam);
   const aa=new Uint16Array(a.memory.buffer,pa,19200),bb=new Uint16Array(b.memory.buffer,pb,19200);
   let count=0;
   for(let p=0;p<19200;p++)if(aa[p]!==bb[p])count++;
   // Texture quantization can differ at exact mathematical edges.
   assert(count<=30,`view ${i} differs in ${count} pixels`);
   total+=19200;differences+=count;
 }
 console.log(`PASS exact span vs 3D DDA: ${total} pixels, ${differences} edge differences`);
})().catch(e=>{console.error(e);process.exitCode=1;});
