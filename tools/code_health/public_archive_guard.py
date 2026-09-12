"""Reject local-only paths in the Git index before public archival."""
from __future__ import annotations
import re
import subprocess
from pathlib import Path, PurePosixPath


def forbidden(name: str) -> bool:
    path = PurePosixPath(name.replace('\\', '/').lower())
    private_dirs = {'game', '.cursor', '.codex', '.agents', '__pycache__',
                    'managed_components', 'fw_archive', 'elf_snapshots', 'backups'}
    if any(part in private_dirs or part.startswith(('build_', 'manual_'))
           or part == 'build' for part in path.parts[:-1]):
        return True
    if path.name in {'agent.md', 'agents.md', '.cursorrules', 'sdkconfig', 'sdkconfig.old'}:
        return True
    if path.name.startswith('.env') and path.name != '.env.example':
        return True
    if path.name.endswith(('.bin', '.elf', '.log', '.pyc', '.bak', '.tmp', '.local.json',
                           '.code-workspace', '.eez-project-ui-state', '.partial')):
        return True
    if re.search(r'\.sqlite3?(?:$|[-.])', path.name):
        return True
    return path.parts[:3] == ('components', 'eezui', 'eezui')


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'],
                            check=True, capture_output=True)
    files = result.stdout.decode('utf-8').split('\0')
    failures = [name for name in files if name and forbidden(name)]
    print(f'PUBLIC_ARCHIVE {"FAIL" if failures else "PASS"} forbidden={len(failures)}')
    for name in failures:
        print(name)
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())
