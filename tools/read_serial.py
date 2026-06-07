import serial
import sys
import time

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 60

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

p = serial.Serial(port, 115200, timeout=0.5)
deadline = time.time() + duration
while time.time() < deadline:
    line = p.readline()
    if line:
        print(line.decode("utf-8", "replace").rstrip())
p.close()
