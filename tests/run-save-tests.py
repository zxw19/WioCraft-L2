from pathlib import Path
import subprocess
root=Path(__file__).resolve().parent.parent
clang=r'D:\android-sdk\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
subprocess.run([clang,'--target=wasm32','-std=c++17','-O2','-ffast-math','-ffreestanding','-fno-exceptions','-fno-rtti','-nostdlib','-I','tests/save-stubs','-I','tests/stubs','-I','src','tests/save-harness.cpp','-Wl,--no-entry','-Wl,--export-all','-Wl,--allow-undefined','-Wl,--initial-memory=1048576','-o','tests/save.wasm'],cwd=root,check=True)
subprocess.run(['node','tests/save-tests.cjs'],cwd=root,check=True)
