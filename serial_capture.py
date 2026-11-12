"""COM83 read-only capture. Never changes DTR/RTS or writes to the port."""
import argparse
import datetime
from pathlib import Path
import time
from usb_port import UsbPort

parser = argparse.ArgumentParser()
parser.add_argument("--seconds", type=int, default=3600)
parser.add_argument("--log", default="logs/serial-current.log")
args = parser.parse_args()
path = Path(args.log)
path.parent.mkdir(parents=True, exist_ok=True)
end = time.monotonic() + args.seconds
with path.open("a", encoding="utf-8", buffering=1) as log:
    def report(line):
        text = f"[{datetime.datetime.now().isoformat(timespec='milliseconds')}] {line}"
        print(text, flush=True)
        log.write(text + "\n")
    port = None
    report("capture started; COM83, read only")
    try:
        while time.monotonic() < end:
            if port is None:
                try:
                    port = UsbPort("COM83", timeout=0.3)
                    report("COM83 connected")
                except OSError:
                    time.sleep(1)
                    continue
            try:
                chunk = port.read(2048)
                if chunk:
                    report(chunk.decode("utf-8", "replace").rstrip())
            except OSError as exc:
                report(f"disconnected: {exc}")
                try:
                    port.close()
                except Exception:
                    pass
                port = None
    except KeyboardInterrupt:
        report("capture stopped")
    finally:
        if port is not None:
            port.close()
