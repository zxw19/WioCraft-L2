"""Compile the actual game.cpp collision/integration functions as WASM."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
game = (root / "src/game.cpp").read_text(encoding="utf-8")
physics = game.split("static bool collides(", 1)[1].split("// ---- input", 1)[0]
prefix = r'''
#include <math.h>
static const float HALF_W=.30f, HEIGHT=1.80f, GRAVITY=24.0f, JUMP_V=7.6f, WALK=4.2f;
#define WORLD_X 16
#define WORLD_Z 16
static float s_px,s_py,s_pz,s_vy,s_mvx,s_mvz;
static bool s_onGround;
static int holeDepth=1, lowCeiling=0;
namespace world {
bool solid(int x,int y,int z) {
  if(y<1)return true;
  if(lowCeiling && y==3)return true;
  bool pit=x==1 && z==1;
  return !pit && y<1+holeDepth;
}
}
'''
suffix = r'''
extern "C" {
void reset(int depth,int ceiling) {
  holeDepth=depth;lowCeiling=ceiling;
  s_px=1.5f;s_py=1.0f;s_pz=1.5f;s_vy=0;s_onGround=true;s_mvx=s_mvz=0;
}
void step(float dt,int jump,float move) {
  s_mvx=move;
  if(jump && s_onGround) {s_vy=JUMP_V;s_onGround=false;}
  applyVertical(dt);
}
float value(int i) {return i==0?s_px:i==1?s_py:i==2?s_vy:(float)s_onGround;}
}
'''
generated = root / "tests/physics-extracted.cpp"
generated.write_text(prefix + "static bool collides(" + physics + suffix, encoding="utf-8")
clang = r"D:\android-sdk\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe"
subprocess.run([clang,"--target=wasm32","-std=c++17","-O2","-ffreestanding","-fno-exceptions","-fno-rtti","-nostdlib","-I",str(root / "tests/stubs"),str(generated),"-Wl,--no-entry","-Wl,--export-all","-Wl,--allow-undefined","-o",str(root / "tests/physics.wasm")],check=True)
subprocess.run(["node",str(root / "tests/test-physics.cjs")],cwd=root,check=True)
