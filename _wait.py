import serial, time, sys
print("waiting for COM83 (replug the board now)...", flush=True)
port = None
for i in range(120):
    try:
        s = serial.Serial("COM83", 115200, timeout=0.3)
        port = s; break
    except Exception:
        time.sleep(1)
if not port:
    print("TIMEOUT: port never appeared"); sys.exit(1)
print("port open, reading 15s (no reset)", flush=True)
end = time.time() + 15
buf = b""
while time.time() < end:
    try:
        d = port.read(1024)
    except Exception as e:
        print("READ ERR:", e, flush=True); break
    if d: buf += d
try: port.close()
except Exception: pass
print("---- output ----")
print(buf.decode("utf-8", "replace"))
