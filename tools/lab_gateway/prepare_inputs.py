"""Install reviewed private build inputs into a fresh cloud checkout."""
import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
from common import digest


def prepare(source, project):
    source, project = source.resolve(), project.resolve()
    manifest = source / "build-inputs.sha256.json"
    inventory = json.loads(manifest.read_text())
    if not isinstance(inventory, dict) or not {"sdkconfig", "dependencies.lock"} <= inventory.keys():
        raise ValueError("inventory must include sdkconfig and dependencies.lock")
    plans = []
    for relative, sha in inventory.items():
        parts = PurePosixPath(relative).parts
        if (not parts or ".." in parts or "\\" in relative or ":" in relative
                or not (relative in ("sdkconfig", "dependencies.lock") or relative.startswith("components/"))):
            raise ValueError("unsupported private input path")
        origin, target = (source / relative).resolve(), (project / relative).resolve()
        if not origin.is_relative_to(source) or not target.is_relative_to(project):
            raise ValueError("input path escaped root")
        if digest(origin.read_bytes()) != sha:
            raise ValueError("private input hash mismatch")
        if target.exists() and digest(target.read_bytes()) != sha:
            raise ValueError("refusing to overwrite differing source: " + relative)
        plans.append((origin, target))
    for origin, target in plans:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(origin, target)
    shutil.copyfile(manifest, project / manifest.name)
    exclude = project / ".git/info/exclude"
    with exclude.open("a", encoding="utf-8") as stream:
        stream.write("\n/build-inputs.sha256.json\n")
    print("Private build inputs verified:", len(plans))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.project)
