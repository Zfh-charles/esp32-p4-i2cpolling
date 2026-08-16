"""Single-instance supervisor for serial_monitor + stability_watch + soak."""
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(r"c:\bake\xiaozhi-p4-epdainaozhong0109")
PY = Path(r"C:\esp_alm\tool\python_env\idf5.4_py3.11_env\Scripts\python.exe")
LOCK = ROOT / "stability_daemon.lock"
CREATE_NO_WINDOW = 0x08000000

TARGETS = (
    "serial_monitor.py",
    "stability_watch.py",
    "stability_soak_status.py",
)


def pids_matching(substr: str) -> list[int]:
    ps = (
        "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
        f"Where-Object {{ $_.CommandLine -like '*{substr}*' }} | "
        "ForEach-Object { $_.ProcessId }"
    )
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command", ps],
            text=True,
            errors="replace",
            timeout=30,
        )
    except Exception:
        return []
    return [int(x) for x in out.split() if x.isdigit()]


def kill_matching(substr: str, keep: int | None = None) -> None:
    for pid in pids_matching(substr):
        if keep is not None and pid == keep:
            continue
        try:
            os.kill(pid, 9)
            print(f"killed {substr} pid={pid}", flush=True)
        except OSError:
            pass


def start_one(script: str) -> int:
    # kill extras first
    kill_matching(script)
    time.sleep(0.3)
    if pids_matching(script):
        return pids_matching(script)[0]
    p = subprocess.Popen(
        [str(PY), "-u", str(ROOT / script)],
        cwd=str(ROOT),
        creationflags=CREATE_NO_WINDOW,
    )
    print(f"started {script} pid={p.pid}", flush=True)
    return p.pid


def acquire_lock() -> bool:
    # Kill other daemons except us
    kill_matching("stability_daemon.py", keep=os.getpid())
    time.sleep(0.5)
    others = [p for p in pids_matching("stability_daemon.py") if p != os.getpid()]
    if others:
        print(f"other daemon still alive {others}, exit", flush=True)
        return False
    LOCK.write_text(f"{os.getpid()}\n{time.time()}\n", encoding="utf-8")
    return True


def main() -> int:
    if not acquire_lock():
        return 1
    print(f"daemon pid={os.getpid()} supervising", flush=True)
    for script in TARGETS:
        start_one(script)
        time.sleep(0.5)

    while True:
        time.sleep(25)
        # ensure lock still ours
        try:
            holder = int(LOCK.read_text(encoding="utf-8").splitlines()[0])
            if holder != os.getpid():
                print("lost lock, exit", flush=True)
                return 0
        except Exception:
            LOCK.write_text(f"{os.getpid()}\n{time.time()}\n", encoding="utf-8")

        for script in TARGETS:
            live = pids_matching(script)
            if not live:
                start_one(script)
            elif len(live) > 1:
                for extra in live[1:]:
                    try:
                        os.kill(extra, 9)
                        print(f"dedupe {script} {extra}", flush=True)
                    except OSError:
                        pass
        # dedupe self
        selves = pids_matching("stability_daemon.py")
        for extra in selves:
            if extra != os.getpid():
                try:
                    os.kill(extra, 9)
                    print(f"dedupe daemon {extra}", flush=True)
                except OSError:
                    pass


if __name__ == "__main__":
    sys.exit(main())
