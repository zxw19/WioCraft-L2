import serial, time, sys
s = serial.Serial("COM83", 115200, timeout=0.3)
# hard reset via EN
s.setDTR(False); s.setRTS(True); time.sleep(0.15); s.setRTS(False)
time.sleep(0.1)
end = time.time() + 10
buf = b""
while time.time() < end:
    d = s.read(1024)
    if d:
        buf += d
s.close()
print(buf.decode("utf-8", "replace"))
