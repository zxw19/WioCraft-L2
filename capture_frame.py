"""Fetch a framebuffer via USB CDC without changing DTR or RTS."""
import argparse
import re
import time
from pathlib import Path
from usb_port import UsbPort
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("--out", default="logs/device-frame.png")
args = parser.parse_args()
with UsbPort("COM83", timeout=0.3) as port:
    port.write(b"I")
    time.sleep(0.2)
    print(port.read(4096).decode("utf-8", "replace"), flush=True)
    port.write(b"P")
    data = b""
    deadline = time.monotonic() + 30
    while b"[/frame]" not in data and time.monotonic() < deadline:
        data += port.read(8192)
    marker = b"[frame] 320 240 RGB565-RLE-HEX\r\n"
    if marker not in data or b"[/frame]" not in data:
        raise RuntimeError("Frame capture incomplete: " + repr(data[-500:]))
    payload = data.split(marker, 1)[1].split(b"[/frame]", 1)[0]
    payload = re.sub(rb"\s", b"", payload).decode("ascii")
    pixels = []
    for i in range(0, len(payload), 8):
        run = int(payload[i:i+4], 16)
        color = int(payload[i+4:i+8], 16)
        r, g, b = (color >> 11) & 31, (color >> 5) & 63, color & 31
        pixels.extend([(r*255//31, g*255//63, b*255//31)] * run)
    if len(pixels) != 320*240:
        raise RuntimeError(f"Wrong pixel count: {len(pixels)}")
    image = Image.new("RGB", (320, 240))
    image.putdata(pixels)
    output = Path(args.out)
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output)
    print(f"Saved {output.resolve()}")
