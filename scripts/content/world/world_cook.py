"""Publication of an edit set's cooked files with its GLBs (Д3, task 7 of docs/landscape-plan.md).

The game reads a cell of the ring from its .wcb beside the GLB with the set's index chunks.wci, and the world cloth
from the bundle cloth.wcl beside the index (OpenGothic/common/worldchunkcookedsource.h). A set folder of edits
(<staging>/<set>/worlds/<WORLD>/) has its own index and bundle, and a .wcb beside each of its GLBs. The cooker is the
converter's (content-convert cook-world, OpenGothic/common/worldchunkcook.*); this module only runs it and moves its
files, in the order every writer of a cooked set keeps:

1. the set's index goes before the first GLB, .wcb or bundle of the set is written or removed;
2. the GLBs are written or removed (world_edit.py, world_region.py);
3. finish(): the converter cooks the base world with the set folder into a scratch folder (--out) and
   verify-cooked-world checks the result; the .wcb of every cell whose GLB the set folder has and the bundle are
   written (through a temporary name; a file that already holds the bytes is left), .wcb files without their GLB
   are removed, and the index is written last.

A publication stopped at any point leaves the set without an index, and the game reads its GLBs. The cook goes to
a scratch folder because the base is only read: the .wcb of a cell depends on its GLB and world.json alone, so the
base's cells are cooked with the base (or read from their GLBs), whatever the set.

Without a converter (--converter '' or none found) the GLBs are still published; the .wcb of the GLBs written or
removed and the bundle are removed with the index, and one line says how to cook the set later
(world_edit.py cook). The converter: --converter, else CONTENT_CONVERT, else the repository's build
OpenGothic/build/opengothic/content-convert when it is there.
"""
from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

import world_manifest
from edit_error import EditError

REPO = Path(__file__).resolve().parents[3]
BUILT = REPO / 'OpenGothic/build/opengothic/content-convert'
INDEX = 'chunks.wci'
CLOTH = 'cloth.wcl'


def converter(given: str | None) -> Path | None:
    """The content-convert to cook with, or None: given ('' for none), else CONTENT_CONVERT, else the build's."""
    if given is None:
        given = os.environ.get('CONTENT_CONVERT')
    if given is None:
        return BUILT if os.access(BUILT, os.X_OK) else None
    if not given:
        return None
    path = Path(given).expanduser()
    if not os.access(path, os.X_OK):
        raise EditError(f'content-convert {path} is not an executable file')
    return path


# every write and removal of the set's files goes through these two (the tests follow the order through them)

def _write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f'{path.name}.{os.getpid()}.tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def _remove(path: Path) -> None:
    path.unlink(missing_ok=True)


class Publication:
    """The files one run of world_edit.py publishes into one set folder of one world."""

    def __init__(self, base_world: Path, set_world: Path, convert: Path | None, hint: str):
        self.base_world, self.set_world, self.convert, self.hint = Path(base_world), Path(set_world), convert, hint
        self.index_gone = False
        self.touched: list[Path] = []     # GLBs written or removed

    def _drop_index(self) -> None:
        if not self.index_gone:
            _remove(self.set_world / INDEX)
            self.index_gone = True

    def write(self, src: Path, target: Path) -> None:
        self._drop_index()
        _write(target, Path(src).read_bytes())
        self.touched.append(target)

    def remove(self, target: Path) -> None:
        self._drop_index()
        _remove(target)
        self.touched.append(target)

    def _put(self, data: bytes, path: Path) -> bool:
        if path.is_file() and path.read_bytes() == data:
            return False
        self._drop_index()
        _write(path, data)
        return True

    def finish(self) -> int:
        """Cooks the set (see the module doc); 0, or 1 when the converter failed (the set is left without index)."""
        glbs = list(self.set_world.rglob('*.glb')) if self.set_world.is_dir() else []
        if not glbs:
            for path in self._cooked():
                self._drop_index()
                _remove(path)
            return 0
        if self.convert is None:
            if self.touched:
                self._drop_index()
                for path in [t.with_suffix('.wcb') for t in self.touched] + [self.set_world / CLOTH]:
                    _remove(path)
            if not (self.set_world / INDEX).exists():
                print(f'{self.set_world}: not cooked, the game reads its GLBs (no content-convert: pass --converter '
                      f'or set CONTENT_CONVERT); cook it with: {self.hint} --converter <content-convert>')
            return 0
        with tempfile.TemporaryDirectory(prefix='world-cook-') as scratch:
            out = Path(scratch)
            args = [str(self.base_world), '--content', str(self.set_world), '--out', str(out)]
            for command in ('cook-world', 'verify-cooked-world'):
                run = subprocess.run([str(self.convert), command, *args], capture_output=True, text=True, check=False)
                if run.returncode != 0:
                    last = ((run.stderr or run.stdout).strip().splitlines() or [''])[-1]
                    return self._failed(f'{command} exited {run.returncode}: {last}')
            if not (out / INDEX).is_file():
                return self._failed('the cook wrote no index (a GLB changed while it ran)')
            written = left = 0
            draws = set()
            for cell in world_manifest.cells(self.base_world):
                if not (self.set_world / cell.draw).is_file():
                    continue
                draw = Path(cell.draw).with_suffix('.wcb')
                draws.add(draw)
                if self._put((out / draw).read_bytes(), self.set_world / draw):
                    written += 1
                else:
                    left += 1
            for path in self.set_world.rglob('*.wcb'):
                if path.relative_to(self.set_world) not in draws:
                    self._drop_index()
                    _remove(path)
            cloth = False
            if (out / CLOTH).is_file():
                cloth = self._put((out / CLOTH).read_bytes(), self.set_world / CLOTH)
            elif (self.set_world / CLOTH).exists():
                self._drop_index()
                _remove(self.set_world / CLOTH)
            index = (out / INDEX).read_bytes()
            index_written = self.index_gone or not (self.set_world / INDEX).is_file() or \
                (self.set_world / INDEX).read_bytes() != index
            if index_written:
                _write(self.set_world / INDEX, index)
        print(f'{self.set_world}: cooked by {self.convert.name}, verify-cooked-world passed: .wcb {written} written, '
              f'{left} left as they were; cloth bundle {"written" if cloth else "left"}; index '
              f'{"written" if index_written else "left"}')
        return 0

    def _cooked(self) -> list[Path]:
        if not self.set_world.is_dir():
            return []
        return [p for p in [self.set_world / INDEX, self.set_world / CLOTH, *self.set_world.rglob('*.wcb')]
                if p.exists()]

    def _failed(self, reason: str) -> int:
        self._drop_index()
        print(f'{self.set_world}: not cooked, the game reads its GLBs: {reason}; cook it again with: {self.hint}')
        return 1
