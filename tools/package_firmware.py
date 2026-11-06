"""把两个已构建的 ELF 与来源 SHA-256 清单打包到 firmware/<版本>。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="v1.0.2")
    args = parser.parse_args()
    if not re.fullmatch(r"v?\d+\.\d+\.\d+(?:[-.][A-Za-z0-9]+)*", args.version):
        parser.error("版本示例：v1.0.2")
    root = Path(__file__).resolve().parents[1]
    pairs = {"blink.elf": "scale_py32f002a", "scale_basic.elf": "scale_basic"}
    source = {name: root / ".pio/build" / env / "firmware.elf" for name, env in pairs.items()}
    for path in source.values():
        if not path.is_file():
            parser.error("先构建两个目标：pio run -e scale_py32f002a -e scale_basic")
    version = args.version if args.version.startswith("v") else "v" + args.version
    dest = root / "firmware" / version
    dest.mkdir(parents=True, exist_ok=True)
    hashes = []
    for name, path in source.items():
        shutil.copyfile(path, dest / name)
        hashes.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + name)
    (dest / "SHA256SUMS").write_text("\n".join(hashes) + "\n", encoding="utf-8", newline="\n")
    files = [p for folder in ("src", "include", "boards", "ldscripts", "tools")
             for p in (root / folder).rglob("*") if p.is_file() and "__pycache__" not in p.parts]
    files.append(root / "platformio.ini")
    manifest = {"version": version, "protocol": 2, "source_sha256": {
        p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)
    }}
    (dest / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                                        encoding="utf-8", newline="\n")
    print("Packaged firmware:", dest)


if __name__ == "__main__":
    main()
