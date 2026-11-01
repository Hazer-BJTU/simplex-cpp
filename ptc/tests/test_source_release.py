"""Source packaging stays separate from runtime checkpoints and dependencies."""

import hashlib
import importlib.util
import tarfile
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "package_source.py"


@pytest.fixture
def packager(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location("package_source", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    root = tmp_path / "checkout"
    package = root / "ptc"
    package.mkdir(parents=True)
    (root / "VERSION").write_text("1.2.3\n")
    (root / "LICENSE").write_text("MIT fixture\n")
    paths = ["LICENSE"]
    for name in ["README.md", "INSTALL.md", "pyproject.toml", "uv.lock", "src/example.py"]:
        path = package / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
        paths.append(f"ptc/{name}")
    (package / "private-checkpoint").write_text("must not ship")
    monkeypatch.setattr(module, "__file__", str(package / "scripts" / "package_source.py"))
    monkeypatch.setattr(
        module.subprocess, "check_output", lambda *args, **kwargs: "\0".join(paths).encode()
    )
    return module, root


def test_source_archive_is_reproducible_and_has_only_tracked_files(packager, tmp_path):
    module, _ = packager
    archive = module.package_source(tmp_path / "first", "v1.2.3")
    other = module.package_source(tmp_path / "second", "v1.2.3")
    assert archive.read_bytes() == other.read_bytes()
    checksum = archive.with_name(archive.name + ".sha256").read_text()
    assert checksum == f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n"
    with tarfile.open(archive) as contents:
        names = contents.getnames()
        assert all(name.startswith("simplex-ptc-v1.2.3/") for name in names)
        assert not any("private-checkpoint" in name for name in names)
        assert contents.extractfile("simplex-ptc-v1.2.3/SOURCE_VERSION").read() == b"v1.2.3\n"
        assert contents.extractfile("simplex-ptc-v1.2.3/LICENSE").read() == b"MIT fixture\n"
    with pytest.raises(FileExistsError):
        module.package_source(tmp_path / "first", "v1.2.3")


def test_source_archive_rejects_wrong_tag_missing_files_and_symlinks(packager, tmp_path):
    module, root = packager
    for tag in ["../outside", "v1.2.4", "v1.2.3-extra"]:
        with pytest.raises(ValueError):
            module.package_source(tmp_path / "invalid", tag)
    path = root / "ptc/INSTALL.md"
    path.unlink()
    with pytest.raises(ValueError, match="regular tracked file"):
        module.package_source(tmp_path / "missing", "v1.2.3")
    path.symlink_to(root / "LICENSE")
    with pytest.raises(ValueError, match="regular tracked file"):
        module.package_source(tmp_path / "symlink", "v1.2.3")
