#!/usr/bin/env python3
"""Verify island evidence, or run it with --run. Thresholds live here, never in the program."""

import argparse
import ast
import gzip
import json
import math
from pathlib import Path
import posixpath
import re
import subprocess

MIN_CROSSINGS = 3
CHUNK_METRES = 64
SHOTS = {"beach", "rocks", "hill"}
SYSTEM_ROOTS = ("/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/etc", "/proc", "/sys", "/dev", "/tmp/.X11-unix", "/selinux", "/var/cache/libx11")
# Path operand indices, counting all syscall arguments. execve's argv/env strings are not file operands.
PATH_ARGS = {
    "open": (0,), "creat": (0,), "openat": (1,), "openat2": (1,),
    "stat": (0,), "stat64": (0,), "lstat": (0,), "lstat64": (0,),
    "newfstatat": (1,), "fstatat64": (1,), "statx": (1,),
    "access": (0,), "faccessat": (1,), "faccessat2": (1,),
    "readlink": (0,), "readlinkat": (1,), "execve": (0,), "execveat": (1,),
    "chdir": (0,), "mkdir": (0,), "mkdirat": (1,), "rmdir": (0,),
    "unlink": (0,), "unlinkat": (1,), "rename": (0, 1), "renameat": (1, 3), "renameat2": (1, 3),
    "link": (0, 1), "linkat": (1, 3), "symlink": (0, 1), "symlinkat": (0, 2),
    "chmod": (0,), "fchmodat": (1,), "chown": (0,), "lchown": (0,), "fchownat": (1,),
    "truncate": (0,), "utime": (0,), "utimes": (0,), "utimensat": (1,),
    "statfs": (0,), "statfs64": (0,),
}
AT_CALLS = {name for name in PATH_ARGS if name.endswith('at') or name.endswith('at2') or name in ('openat2', 'statx', 'newfstatat', 'fstatat64')}


def inside(path, root):
    return path == root or path.startswith(root.rstrip('/') + '/')


def arguments(text):
    """Split syscall operands, respecting quoted strings and nested structs/arrays."""
    return re.split(r',\s*(?=(?:[^"\\]*(?:\\.[^"\\]*)*"[^"\\]*(?:\\.[^"\\]*)*")*[^"\\]*$)(?![^\[\]{}]*[\]}])', text)


def trace_paths(text, run_root):
    """Resolve even failed opens, dirfds and child cwd; reject truncated/unparseable traces."""
    cwd, pending, paths, errors = {}, {}, set(), []
    calls = 0
    exited = False
    for line in text.splitlines():
        match = re.match(r'(?:\[pid\s+)?(\d+)\]?\s+(.*)', line)
        if not match:
            errors.append('unrecognized trace line: ' + line[:160])
            continue
        pid, body = match.groups()
        cwd.setdefault(pid, run_root)
        if '<unfinished ...>' in body:
            pending[pid] = body.replace('<unfinished ...>', '')
            continue
        if body.startswith('<... '):
            if pid not in pending:
                errors.append('resumed syscall without start: ' + line[:160])
                continue
            body = pending.pop(pid) + re.sub(r'^<\.\.\. \w+ resumed>', '', body)
        if body.startswith('+++ exited with '):
            exited = True
            continue
        if body.startswith(('--- ', '+++ ', 'strace:')):
            continue
        call = re.match(r'(\w+)\((.*)\)\s+=\s+(.*)', body)
        if not call:
            errors.append('unparsed syscall: ' + line[:160])
            continue
        name, raw, result = call.groups()
        calls += 1
        if name in ('clone', 'clone3', 'fork', 'vfork'):
            child = re.match(r'(\d+)', result)
            if child:
                cwd[child[1]] = cwd[pid]
            continue
        if name not in PATH_ARGS:
            errors.append('unsupported file syscall: ' + name)
            continue
        args = arguments(raw)
        resolved = []
        for index in PATH_ARGS[name]:
            if index >= len(args):
                errors.append('missing file operand: ' + line[:160])
                continue
            token = args[index]
            # NULL is valid in utimensat; empty strings designate the annotated fd itself.
            if token == 'NULL':
                continue
            try:
                path = ast.literal_eval(token)
            except (ValueError, SyntaxError):
                errors.append('unparsed file operand: ' + line[:160])
                continue
            if not isinstance(path, str):
                errors.append('non-string path: ' + line[:160])
                continue
            base = cwd[pid]
            if name in ('symlink', 'symlinkat') and index == 0:
                # A relative symlink target is relative to the new link's directory, not the process cwd.
                destination = ast.literal_eval(args[PATH_ARGS[name][-1]])
                if name == 'symlinkat':
                    annotation = re.search(r'<(/[^>]*)>', args[1])
                    if annotation:
                        base = annotation[1]
                    elif args[1] != 'AT_FDCWD' and not destination.startswith('/'):
                        errors.append('unresolved symlink directory fd: ' + line[:160])
                        continue
                base = posixpath.dirname(posixpath.join(base, destination))
            if name in AT_CALLS and index > 0:
                directory = args[index-1]
                annotation = re.search(r'<(/[^>]*)>', directory)
                if annotation:
                    base = annotation[1]
                elif directory != 'AT_FDCWD' and not path.startswith('/'):
                    errors.append('unresolved directory fd: ' + line[:160])
                    continue
            full = posixpath.normpath(posixpath.join(base, path))
            paths.add(full)
            resolved.append(full)
        # -yy resolves returned file descriptors through symlinks. Check those targets as well.
        paths.update(posixpath.normpath(p.removesuffix(' (deleted)')) for p in re.findall(r'<(/[^>]*)>', raw + ' = ' + result))
        if name == 'chdir' and result == '0' and resolved:
            cwd[pid] = resolved[0]
    if pending:
        errors.append(f'{len(pending)} unfinished syscalls')
    if not calls or not exited:
        errors.append('trace is empty or has no process exit')
    return paths, errors


def verify(folder):
    report = json.loads((folder / 'report.json').read_text())
    context = {k: report[k] for k in ('run_root', 'binary_dir', 'uid', 'program_exit')}
    if 'total_seconds' in report:
        context['total_seconds'] = report['total_seconds']
    events = [json.loads(line) for line in (folder / 'events.jsonl').read_text().splitlines()]
    failures = []
    def require(ok, reason):
        if not ok:
            failures.append(reason)
    require(bool(events) and events[0]['event'] == 'start', 'missing start event')
    end = events[-1] if events else {}
    require(end.get('event') == 'end' and end.get('completed') is True and end.get('exit_code') == 0
            and context['program_exit'] == 0, 'program did not exit 0 at track end')
    require(30000 <= end.get('ms', 0) <= 60000, 'track duration must be 30–60 seconds')
    kinds = [e['event'] for e in events]
    require('census' in kinds and (folder / 'census.json').is_file(), 'missing content census')
    crossings, axes, jumping, jumps, lands = 0, set(), False, 0, 0
    previous_ms = -1
    for e in events:
        require(e['ms'] >= previous_ms, 'event time went backwards')
        previous_ms = e['ms']
        require(all(math.isfinite(e[k]) for k in ('x', 'y', 'z')), 'non-finite hero position')
        if e['event'] == 'jump':
            require(not jumping and not e['grounded'], 'invalid jump event')
            jumping, jumps = True, jumps + 1
        elif e['event'] == 'land':
            require(jumping and e['grounded'], 'landing without a jump or ground contact')
            jumping, lands = False, lands + 1
        require(e['grounded'] or jumping, f"hero off ground outside a jump at {e['ms']} ms")
        if e['event'] == 'chunk':
            a = tuple(map(int, e['from'].split(',')))
            b = tuple(map(int, e['to'].split(',')))
            require(len(a) == len(b) == 2 and a != b, 'invalid chunk crossing')
            require(b == (math.floor(e['x']/CHUNK_METRES), math.floor(e['z']/CHUNK_METRES)), 'chunk does not match hero position')
            axes.update(i for i in range(2) if a[i] != b[i])
            crossings += 1
    require(crossings >= MIN_CROSSINGS, f'only {crossings} chunk crossings; need {MIN_CROSSINGS}')
    require(len(axes) == 2, 'chunk crossings must cover both X and Z axes')
    transitions = [e for e in events if e['event'] == 'residency']
    for e in transitions:
        require(bool(re.fullmatch(r'-?\d+,-?\d+', e['id'])) and e['from'] != e['to']
                and {e['from'], e['to']} <= {'Outside', 'Far', 'Near'}, 'invalid residency transition')
    loads = sum(e['ms'] > 0 and e['from'] == 'Far' and e['to'] == 'Near' for e in transitions)
    unloads = sum(e['ms'] > 0 and e['from'] == 'Near' and e['to'] == 'Far' for e in transitions)
    require(loads > 0 and unloads > 0, f'near-belt streaming missing after startup: {loads} Far→Near, {unloads} Near→Far')
    require(jumps > 0 and lands == jumps and not jumping, f'jump/landing missing or incomplete: {jumps} jumps, {lands} landings')
    samples = [e['ms'] for e in events if e['event'] == 'position']
    require(bool(samples) and max(b-a for a, b in zip([0]+samples, samples+[end.get('ms', 0)])) <= 117,
            'position samples missing or more than 117 ms apart')
    shots = {e['name'] for e in events if e['event'] == 'shot' and Path(e['file']).name == e['name']+'.png'
             and (folder / (e['name']+'.png')).is_file()}
    require(SHOTS <= shots, 'missing shots: ' + ', '.join(sorted(SHOTS-shots)))
    require((folder / 'clip.mp4').is_file(), 'missing clip.mp4')
    texture_loads = sum(e['event'] == 'texture' and e['ms'] > 0 for e in events)
    require(texture_loads > 0, 'missing texture loads after startup')
    with gzip.open(folder / 'strace.log.gz', 'rt') as trace:
        paths, errors = trace_paths(trace.read(), context['run_root'])
    allowed = SYSTEM_ROOTS + (context['run_root'], context['binary_dir'], f"/run/user/{context['uid']}")
    forbidden = sorted(p for p in paths if p != '/' and not any(inside(p, root) for root in allowed))
    require(not forbidden, f'{len(forbidden)} paths outside allowed roots (including failed attempts)')
    failures.extend(errors)
    result = dict(context, passed=not failures, crossings=crossings, loads=loads, unloads=unloads, jumps=jumps,
                  landings=lands, shots=sorted(shots), traced_paths=len(paths), forbidden_paths=forbidden,
                  frame_median_ms=end.get('frame_median_ms'), frame_p95_ms=end.get('frame_p95_ms'),
                  frames=end.get('frames'), duration_ms=end.get('ms'), texture_loads=texture_loads, failures=sorted(set(failures)))
    (folder / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--run', action='store_true', help='run island.sh instead of checking existing evidence')
    args = parser.parse_args()
    if args.run:
        return subprocess.call([str(Path(__file__).with_name('island.sh')), str(args.folder)])
    try:
        report = verify(args.folder)
    except (OSError, ValueError, KeyError, TypeError, IndexError) as exc:
        print(f'island: invalid or missing evidence: {exc}')
        return 1
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
