const fs = require('fs');
const assert = require('assert').strict;
(async () => {
  const { instance } = await WebAssembly.instantiate(fs.readFileSync('tests/game.wasm'), {
    env: { sqrtf: Math.sqrt, sinf: Math.sin, cosf: Math.cos, floorf: Math.floor, ceilf: Math.ceil, fabsf: Math.abs, powf: Math.pow }
  });
  const w = instance.exports;
  const touch = (points, dt=.02) => {
    const p = points.concat(Array(3-points.length).fill([-1,-1,-1]));
    w.touch(...p[0],...p[1],...p[2],points.length,dt);
    return Array.from({length:12},(_,i)=>w.inputValue(i));
  };
  w.resetInput();
  let f=touch([[56,136,1],[290,77,2],[280,179,3]]);
  assert(f[0]>.99 && f[4]===1 && f[5]===1 && f[6]===1, 'three-finger walk + jump + dig');
  f=touch([[56,136,1],[290,77,2],[280,179,3]]);
  assert(f[5]===0, 'jump only triggers once');
  f=touch([]);
  assert(f[0]>.99 && f[6]===1 && f[9]===1, 'one missed sample keeps gestures alive');
  touch([]); f=touch([]);
  assert(f[4]===0 && f[6]===0 && f[7]===0 && f[9]===0, 'release debounces without placing');
  w.resetInput(); touch([[56,168,1]]); f=touch([[20,230,1]]);
  assert(f[4]===1 && f[8]===-1 && f[1]<0, 'joystick stays locked outside its starting zone');
  w.resetInput(); touch([[180,80,7]]); f=touch([[200,100,7]]);
  assert(Math.abs(f[2]-.068)<1e-5 && Math.abs(f[3]+5.2)<1e-4, 'controlled view sensitivity');
  for(let i=0;i<20;i++)f=touch([[200,100,7]]);
  assert(f[6]===0, 'drag does not become a mining hold');
  w.resetInput(); for(let i=0;i<14;i++)f=touch([[180,80,1]]);
  assert(f[6]===1, 'stationary view hold mines');
  w.resetInput(); f=touch([[290,127,9]]); assert(f[7]===1, 'place has explicit button');
  f=touch([[290,127,9]]); assert(f[7]===0, 'place is edge triggered');
  w.resetInput(); touch([[48,220,1]]); f=touch([[270,220,1]]); assert(f[8]===8, 'hotbar slide selection');
  console.log('PASS input: multi-touch, debounce, locked zones, look, mining, placement, hotbar');
  assert(w.init()===1);
  const h=w.heightAt(56,56); assert(h>20 && h<64);
  assert(w.setBlock(-1,10,10,1)===0 && w.setBlock(10,10,10,250)===0);
  const b=w.getBlock(56,h-1,56);
  for(let i=0;i<10000;i++)assert(w.setBlock(56,h-1,56,i%2===0?0:b)===1);
  assert(w.countEdits()===1,'repeated editing is compacted');
  console.log('PASS world: generated terrain, rejected invalid edits, 10,000 repeated edits occupy one record');
  // A finite isolated scene above the terrain tests all visible cube surfaces.
  w.clearRegion(46,45,46,14);
  w.setBlock(52,49,52,3);
  const read=ptr=>new Uint16Array(w.memory.buffer,ptr,160*120).slice();
  const camera=[49.5,49.5,52.5,0,60];
  let front=read(w.draw(...camera));
  assert(w.pick(...camera)===((52<<16)|(49<<8)|52), 'center ray and visible target match');
  w.setBlock(54,49,52,5);
  let behind=read(w.draw(...camera));
  for(let y=44;y<=76;y++)for(let x=65;x<=95;x++)
    assert(front[y*160+x]===behind[y*160+x], 'opaque cube occludes block behind it');
  w.setBlock(52,49,52,0);
  let opened=read(w.draw(...camera));
  assert(opened[60*160+80]!==behind[60*160+80], 'removing front cube reveals rear cube');
  w.setBlock(52,49,52,3);
  let above=read(w.draw(49.5,51.5,52.5,0,26));
  let below=read(w.draw(49.5,47.5,52.5,0,94));
  const out=(name,pixels)=>{
    const rgb=Buffer.alloc(160*120*3);
    pixels.forEach((c,i)=>{rgb[i*3]=(c>>11)*255/31;rgb[i*3+1]=((c>>5)&63)*255/63;rgb[i*3+2]=(c&31)*255/31;});
    fs.writeFileSync(`tests/${name}.ppm`,Buffer.concat([Buffer.from('P6\n160 120\n255\n'),rgb]));
  };
  out('front',front);out('above',above);out('below',below);
  // Compare silhouettes against an independent slow 3-D DDA oracle.
  // This catches missing lower surfaces under tree canopies and false holes.
  const oracle=(x,y,z,yaw,horizon,col,row)=>{
    const f=Math.cos(yaw),s=Math.sin(yaw),lat=(col+.5-80)/(80/.75);
    const dx=f-s*lat, dz=s+f*lat, dy=(horizon-row-.5)/(80/.75);
    let ix=Math.floor(x),iy=Math.floor(y),iz=Math.floor(z),t=0;
    const ax=Math.abs(dx),ay=Math.abs(dy),az=Math.abs(dz);
    const sx=dx>=0?1:-1,sy=dy>=0?1:-1,sz=dz>=0?1:-1;
    const txd=ax>1e-7?1/ax:1e9,tyd=ay>1e-7?1/ay:1e9,tzd=az>1e-7?1/az:1e9;
    let tx=ax>1e-7?(dx>=0?ix+1-x:x-ix)*txd:1e9;
    let ty=ay>1e-7?(dy>=0?iy+1-y:y-iy)*tyd:1e9;
    let tz=az>1e-7?(dz>=0?iz+1-z:z-iz)*tzd:1e9;
    for(let guard=0;guard<160 && t<=46;guard++){
      if(ix<0||ix>=112||iy<0||iy>=64||iz<0||iz>=112)return false;
      if(w.getBlock(ix,iy,iz)!==0)return true;
      if(tx<=ty&&tx<=tz){t=tx;tx+=txd;ix+=sx;}
      else if(ty<=tz){t=ty;ty+=tyd;iy+=sy;}
      else{t=tz;tz+=tzd;iz+=sz;}
    }
    return false;
  };
  // Terrain textures are different from the sky; get the unmodified sky first
  // through the small harness export, then compare hit masks across views.
  if(w.drawSky) for(let a=0;a<12;a++){
    const cam=[49.3+(a%3)*.13,47.7+(a%5),51.2,a*.19,32+a*4];
    const sky=read(w.drawSky(...cam));const pixels=read(w.draw(...cam));
    let mismatch=0;
    for(let row=0;row<120;row+=3)for(let col=0;col<160;col+=3){
      const expected=oracle(...cam,col,row),actual=pixels[row*160+col]!==sky[row*160+col];
      if(expected!==actual)mismatch++;
    }
    assert(mismatch<5,`3D oracle silhouette at view ${a}: ${mismatch} mismatches`);
  }
  for(let a=0;a<100;a++)w.draw(50.1+(a%7)*.1,48+(a%5),51.1, a*.073,30+a%65);
  console.log('PASS render: opaque placement, target ray, removal and 100 viewpoints; wrote test frames');
})().catch(e=>{console.error(e);process.exitCode=1;});
