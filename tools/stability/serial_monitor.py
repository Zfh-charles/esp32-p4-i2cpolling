"""Background COM7 logger for xiaozhi P4. Appends to serial_monitor.log."""
import serial
import time
import sys
from pathlib import Path

PORT = "COM7"
BAUD = 115200
OUT = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109\serial_monitor.log")
META = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109\serial_monitor.meta")

def main():
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("a", encoding="utf-8", errors="replace") as f:
        f.write(f"\n===== MONITOR START {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n")
        f.flush()
    META.write_text(f"running=1\nport={PORT}\nstarted={time.time()}\n", encoding="utf-8")
    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=0.5, write_timeout=2)
            with OUT.open("a", encoding="utf-8", errors="replace") as f:
                f.write(f"----- OPENED {time.strftime('%H:%M:%S')} -----\n")
                f.flush()
                while True:
                    data = ser.read(4096)
                    if data:
                        text = data.decode("utf-8", errors="replace")
                        f.write(text)
                        if "\n" in text:
                            f.flush()
                    META.write_text(
                        f"running=1\nport={PORT}\nlast={time.time()}\nbytes_ok=1\n",
                        encoding="utf-8",
                    )
        except Exception as e:
            with OUT.open("a", encoding="utf-8", errors="replace") as f:
                f.write(f"\n===== MONITOR ERR {time.strftime('%H:%M:%S')}: {e} =====\n")
                f.flush()
            META.write_text(f"running=1\nport={PORT}\nerr={e}\nlast={time.time()}\n", encoding="utf-8")
            time.sleep(2)

if __name__ == "__main__":
    main()
