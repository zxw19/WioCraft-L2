import serial, time, sys
try:
    s = serial.Serial("COM83", 115200, timeout=0.5)
except Exception as e:
    print("OPEN FAIL:", e); sys.exit(1)
end = time.time() + 14
buf = b""
while time.time() < end:
    d = s.read(512)
    if d:
        buf += d
s.close()
txt = buf.decode("utf-8", "replace")
print(txt[-3000:])
