import serial, time, sys
for attempt in range(30):
    try:
        s = serial.Serial("COM83", 115200, timeout=0.3)
        break
    except Exception:
        time.sleep(1)
else:
    print("PORT NEVER CAME BACK"); sys.exit(1)
end = time.time() + 12
buf = b""
while time.time() < end:
    try:
        d = s.read(1024)
    except Exception as e:
        print("READ ERR:", e); break
    if d: buf += d
try: s.close()
except Exception: pass
print(buf.decode("utf-8", "replace")[-2500:])
