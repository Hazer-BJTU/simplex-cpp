"""Build a reproducible, standalone source archive from tracked PTC files.

Run from a repository checkout; installation of the resulting archive needs no
Git checkout or worker build. The release tag identifies the source snapshot,
independently of the experimental Python package's own metadata version.
"""

import argparse
import gzip
import hashlib
import io
import re
import subprocess
import tarfile
from pathlib import Path


def package_source(output: Path, tag: str) -> Path:
    """Archive tracked package files and the repository license, never local state."""
    if not re.fullmatch(r"v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", tag):
        raise ValueError("expected a stable release tag")
    root = Path(__file__).resolve().parents[2]
    if tag != "v" + (root / "VERSION").read_text().strip():
        raise ValueError("release tag must match VERSION")
    paths = (
        subprocess.check_output(["git", "ls-files", "-z", "--", "ptc", "LICENSE"], cwd=root)
        .decode()
        .split("\0")
    )
    files = {}
    for relative in filter(None, paths):
        path = root / relative
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"expected a regular tracked file: {relative}")
        name = relative.removeprefix("ptc/")
        files[name] = path.read_bytes()
    required = {"LICENSE", "INSTALL.md", "README.md", "pyproject.toml", "uv.lock"}
    if not required <= files.keys():
        raise ValueError("source package is missing installation files")
    files["SOURCE_VERSION"] = (tag + "\n").encode()
    prefix = f"simplex-ptc-{tag}"
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"{prefix}-source.tar.gz"
    # Refuse to overwrite an existing candidate, including its checksum.
    checksum = output / (archive.name + ".sha256")
    if archive.exists() or checksum.exists():
        raise FileExistsError("source release output already exists")
    with archive.open("xb") as raw:
        with gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as tar:
                for name, data in sorted(files.items()):
                    info = tarfile.TarInfo(f"{prefix}/{name}")
                    info.size = len(data)
                    info.mode = 0o644
                    tar.addfile(info, io.BytesIO(data))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    checksum.write_text(f"{digest}  {archive.name}\n")
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("tag")
    args = parser.parse_args()
    print(package_source(args.output, args.tag))
