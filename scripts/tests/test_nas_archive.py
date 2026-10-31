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


def test_custom_destination_cli_never_overwrites_existing_data(archive, roots, capsys):
    (roots.source / "pytest-result.txt").write_bytes(b"irreplaceable test results")
    roots.destination.mkdir(parents=True)
    (roots.destination / "first.txt").write_bytes(b"unrelated older data")
    destination = roots.nas / "archive" / "task-20261028"
    options = ["move", str(roots.source), "--destination-relative", "archive/task-20261028",
               "--attempt-id", "janitor-attempt"]
    assert archive.main([*options, "--dry-run"]) == 0
    assert str(destination) in capsys.readouterr().out
    assert not destination.exists()
    assert archive.main(options) == 0
    assert not roots.source.exists()
    assert (destination / "first.txt").read_bytes() == b"first result\n"
    assert (destination / "pytest-result.txt").read_bytes() == b"irreplaceable test results"
    assert not (destination / archive.INCOMPLETE_MARKER).exists()
    assert (roots.destination / "first.txt").read_bytes() == b"unrelated older data"

    roots.source.mkdir()
    (roots.source / "new.txt").write_bytes(b"new task")
    marker = destination / archive.INCOMPLETE_MARKER
    for empty_marker in (False, True):
        if empty_marker:
            marker.write_text("")
        for dry_run in (False, True):
            with pytest.raises(archive.ArchiveError, match="already exists"):
                archive.move(roots.source, destination_relative="archive/task-20261028",
                             attempt_id="janitor-attempt", dry_run=dry_run)
    assert (roots.source / "new.txt").read_bytes() == b"new task"
    assert not (destination / "new.txt").exists()


@pytest.mark.parametrize("with_links", [False, True])
def test_custom_destination_resumes_only_the_same_unfinished_attempt(archive, roots, monkeypatch, with_links):
    destination = roots.nas / "archive" / "task-20261028"
    options = {"destination_relative": "archive/task-20261028", "attempt_id": "stable-token"}
    run = archive.subprocess.run
    (roots.source / "removed.txt").write_bytes(b"old result")
    (roots.source / "removed-dir").mkdir()
    (roots.source / "removed-dir" / "old.bin").write_bytes(b"old nested result")
    (roots.source / "link").symlink_to("removed.txt")

    def interrupt_copy(*args, **kwargs):
        result = run(*args, **kwargs)
        assert result.returncode == 0, result.stderr
        (destination / "first.txt").write_bytes(b"partial data\n")
        return subprocess.CompletedProcess(args[0], 12, "", "connection unexpectedly closed")

    monkeypatch.setattr(archive.subprocess, "run", interrupt_copy)
    with pytest.raises(archive.ArchiveError, match="rsync failed"):
        archive.move(roots.source, **options)
    assert (destination / archive.INCOMPLETE_MARKER).is_file()
    (roots.source / "removed.txt").unlink()
    (roots.source / "removed-dir" / "old.bin").unlink()
    (roots.source / "removed-dir").rmdir()
    (roots.source / "link").unlink()
    if with_links:
        (roots.source / "link").symlink_to("first.txt")
    (destination / "extra.txt").write_bytes(b"incomplete extra")
    (destination / "extra-dir").mkdir()
    (destination / "extra-dir" / "extra.txt").write_bytes(b"incomplete nested extra")
    outside = roots.nas / "unrelated"
    outside.mkdir()
    (outside / "precious.txt").write_bytes(b"keep unrelated data")
    (destination / "extra-link").symlink_to(outside, target_is_directory=True)
    (destination / ".symlinks.tsv").write_text("link\tremoved.txt\n")
    with pytest.raises(archive.ArchiveError, match="already exists"):
        archive.move(roots.source, destination_relative=options["destination_relative"],
                     attempt_id="different-token")
    other = roots.local / "other-task"
    other.mkdir()
    with pytest.raises(archive.ArchiveError, match="already exists"):
        archive.move(other, **options)
    monkeypatch.setattr(archive.subprocess, "run", run)
    assert archive.move(roots.source, **options) == roots.size
    assert not roots.source.exists()
    assert not (destination / archive.INCOMPLETE_MARKER).exists()
    assert (destination / "first.txt").read_bytes() == b"first result\n"
    assert sorted(path.name for path in destination.iterdir() if path.name != ".symlinks.tsv") == ["empty", "first.txt", "nested"]
    if with_links:
        assert (destination / ".symlinks.tsv").read_text() == "link\tfirst.txt\n"
    else:
        assert not (destination / ".symlinks.tsv").exists()
    assert (outside / "precious.txt").read_bytes() == b"keep unrelated data"


def test_custom_destination_rejects_unsafe_paths(archive, roots):
    for relative in ("", ".", "../outside", "archive/../../outside", str(roots.nas)):
        with pytest.raises(archive.ArchiveError, match="relative path"):
            archive.move(roots.source, destination_relative=relative, attempt_id="token")
    with pytest.raises(archive.ArchiveError, match="requires a nonempty attempt_id"):
        archive.move(roots.source, destination_relative="archive/task")
    (roots.nas / "alias").symlink_to(roots.local, target_is_directory=True)
    with pytest.raises(archive.ArchiveError, match="symbolic link"):
        archive.move(roots.source, destination_relative="alias/task", attempt_id="token")


def test_verified_archive_is_sealed_before_interrupted_local_cleanup(archive, roots, monkeypatch):
    destination = roots.nas / "archive" / "task-20261028"
    options = {"destination_relative": "archive/task-20261028", "attempt_id": "stable-token"}
    remove = archive.shutil.rmtree

    def remove_then_fail(path):
        assert not (destination / archive.INCOMPLETE_MARKER).exists()
        remove(path)
        raise OSError("interrupted after source removal")

    monkeypatch.setattr(archive.shutil, "rmtree", remove_then_fail)
    with pytest.raises(archive.ArchiveError, match="verified on NAS; local cleanup failed"):
        archive.move(roots.source, **options)
    assert not roots.source.exists()
    assert not (destination / archive.INCOMPLETE_MARKER).exists()
    original = {str(path.relative_to(destination)): path.read_bytes()
                for path in destination.rglob("*") if path.is_file()}

    roots.source.mkdir()
    (roots.source / "first.txt").write_bytes(b"new task at the same source path")
    with pytest.raises(archive.ArchiveError, match="already exists"):
        archive.move(roots.source, **options)
    assert (roots.source / "first.txt").read_bytes() == b"new task at the same source path"
    assert {str(path.relative_to(destination)): path.read_bytes()
            for path in destination.rglob("*") if path.is_file()} == original
