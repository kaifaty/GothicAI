"""Verified NAS moves with temporary roots and a fake mount table/free-space check."""
from __future__ import annotations

import csv
import importlib.util
import os
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "nas_archive.py"


@pytest.fixture(scope="module")
def archive():
    spec = importlib.util.spec_from_file_location("nas_archive_test", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def roots(tmp_path, monkeypatch, archive):
    local = tmp_path / "local"
    nas = tmp_path / "nas"
    local.mkdir()
    nas.mkdir()
    mounts = tmp_path / "mounts"
    mounts.write_text(f"//server/share {nas} cifs rw 0 0\n")
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(local))
    monkeypatch.setenv("GOTHIC_NAS_ROOT", str(nas))
    monkeypatch.setenv("GOTHIC_NAS_MOUNTS", str(mounts))
    monkeypatch.setattr(archive.shutil, "disk_usage", lambda _path: SimpleNamespace(free=10**15))
    source = local / "task" / "results"
    source.mkdir(parents=True)
    (source / "first.txt").write_bytes(b"first result\n")
    (source / "nested").mkdir()
    (source / "nested" / "second.bin").write_bytes(bytes(range(256)))
    (source / "empty").mkdir()
    return SimpleNamespace(local=local, nas=nas, mounts=mounts, source=source,
                           destination=nas / "task" / "results", size=269)


def test_successful_move_verifies_and_removes_source(archive, roots):
    assert archive.move(roots.source) == roots.size
    assert not roots.source.exists()
    assert (roots.destination / "first.txt").read_bytes() == b"first result\n"
    assert (roots.destination / "nested" / "second.bin").read_bytes() == bytes(range(256))
    assert (roots.destination / "empty").is_dir()


def test_missing_cifs_mount_retains_source(archive, roots):
    roots.mounts.write_text(f"/dev/disk {roots.nas} ext4 rw 0 0\n")
    with pytest.raises(archive.ArchiveError, match="not mounted as cifs") as error:
        archive.move(roots.source)
    assert error.value.bytes == roots.size
    assert (roots.source / "first.txt").read_bytes() == b"first result\n"
    assert not roots.destination.exists()


def test_checksum_mismatch_retains_all_local_files(archive, roots, monkeypatch):
    run = archive.subprocess.run

    def corrupt_after_copy(*args, **kwargs):
        result = run(*args, **kwargs)
        assert result.returncode == 0, result.stderr
        file = roots.destination / "first.txt"
        before = file.stat()
        file.write_bytes(b"broken copy!\n")
        os.utime(file, ns=(before.st_atime_ns, before.st_mtime_ns))
        return result

    monkeypatch.setattr(archive.subprocess, "run", corrupt_after_copy)
    with pytest.raises(archive.ArchiveError, match="checksum mismatch: first.txt"):
        archive.move(roots.source)
    assert (roots.source / "first.txt").read_bytes() == b"first result\n"
    assert (roots.source / "nested" / "second.bin").read_bytes() == bytes(range(256))


def test_retry_after_interrupted_copy_repairs_existing_files(archive, roots, monkeypatch):
    run = archive.subprocess.run

    def interrupt_copy(*args, **kwargs):
        result = run(*args, **kwargs)
        assert result.returncode == 0, result.stderr
        file = roots.destination / "first.txt"
        before = file.stat()
        file.write_bytes(b"partial data\n")
        os.utime(file, ns=(before.st_atime_ns, before.st_mtime_ns))
        return subprocess.CompletedProcess(args[0], 12, "", "connection unexpectedly closed")

    monkeypatch.setattr(archive.subprocess, "run", interrupt_copy)
    with pytest.raises(archive.ArchiveError, match="rsync failed \\(12\\)"):
        archive.move(roots.source)
    assert (roots.source / "first.txt").read_bytes() == b"first result\n"
    monkeypatch.setattr(archive.subprocess, "run", run)
    assert archive.move(roots.source) == roots.size
    assert not roots.source.exists()
    assert (roots.destination / "first.txt").read_bytes() == b"first result\n"
    assert sorted(path.name for path in roots.destination.iterdir()) == ["empty", "first.txt", "nested"]


def test_symlinks_are_listed_in_output_and_tsv(archive, roots, capsys):
    (roots.source / "link").symlink_to("first.txt")
    (roots.source / "nested" / "missing").symlink_to("../absent")
    (roots.source / "directory-link").symlink_to("nested", target_is_directory=True)
    assert archive.move(roots.source) == roots.size
    with (roots.destination / ".symlinks.tsv").open(newline="") as source:
        assert dict(csv.reader(source, delimiter="\t")) == {
            "link": "first.txt", "nested/missing": "../absent", "directory-link": "nested"}
    output = capsys.readouterr().out
    assert "'link' -> 'first.txt'" in output
    assert "'nested/missing' -> '../absent'" in output
    assert "'directory-link' -> 'nested'" in output
    assert not (roots.destination / "link").exists()
    assert not (roots.destination / "directory-link").exists()


def test_pytest_files_and_directories_are_excluded(archive, roots):
    cache = roots.source / "nested" / "pytest-huge"
    cache.mkdir()
    with (cache / "sparse").open("wb") as output:
        output.seek(2**32)
        output.write(b"x")
    (roots.source / "pytest-file").write_bytes(b"skip this")
    assert archive.move(roots.source) == roots.size
    assert not (roots.destination / "nested" / "pytest-huge").exists()
    assert not (roots.destination / "pytest-file").exists()


def test_cli_dry_run_and_keep_local(archive, roots, capsys):
    assert archive.main(["move", str(roots.source), "--dry-run"]) == 0
    output = capsys.readouterr().out
    assert "would move" in output and f"{roots.size} bytes" in output
    assert str(roots.source) in output and str(roots.destination) in output
    assert not roots.destination.exists()
    assert archive.main(["move", str(roots.source), "--keep-local"]) == 0
    assert "copied and verified" in capsys.readouterr().out
    assert roots.source.is_dir()
    assert (roots.destination / "first.txt").read_bytes() == b"first result\n"


def test_root_outside_and_symbolic_source_are_refused(archive, roots):
    for source in (roots.local, roots.local.parent, roots.nas):
        with pytest.raises(archive.ArchiveError, match="below the local root"):
            archive.move(source)
    alias = roots.local / "alias"
    alias.symlink_to(roots.source, target_is_directory=True)
    with pytest.raises(archive.ArchiveError, match="symbolic link"):
        archive.move(alias)
    with pytest.raises(archive.ArchiveError, match="absolute path"):
        archive.validate_source("task/results", require_absolute=True)
    assert archive.validate_source(roots.local / "future", require_absolute=True) == roots.local / "future"


def test_space_failure_is_one_line_and_retains_source(archive, roots, monkeypatch, capsys):
    monkeypatch.setattr(archive.shutil, "disk_usage", lambda _path: SimpleNamespace(free=0))
    assert archive.main(["move", str(roots.source)]) == 1
    error = capsys.readouterr().err
    assert len(error.splitlines()) == 1
    assert "not enough NAS space" in error and f"{roots.size} bytes" in error
    assert (roots.source / "first.txt").read_bytes() == b"first result\n"
    assert not roots.destination.exists()


def test_mount_at_ancestor_is_accepted(archive, roots):
    roots.mounts.write_text(f"//server/share {roots.nas.parent} cifs rw 0 0\n")
    assert archive.move(roots.source) == roots.size


def test_cancelled_before_deletion_retains_source(archive, roots):
    cancelled = iter((False, True))
    with pytest.raises(archive.ArchiveError, match="archive cancelled"):
        archive.move(roots.source, cancelled=lambda: next(cancelled))
    assert (roots.source / "first.txt").read_bytes() == b"first result\n"
    assert (roots.destination / "first.txt").read_bytes() == b"first result\n"
