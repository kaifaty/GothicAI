"""Windows process trees: every process the launcher starts runs in a job object of its own.

A job holds the process and everything it starts (`uv run` starts the service as its child), so stopping it
ends the whole tree, as signalling a process group does on Unix. Jobs are made with KILL_ON_JOB_CLOSE and
the launcher holds their only handles: when it exits for any reason, even killed from the Task Manager,
Windows ends every tree it started. A process starts suspended and runs only once it is in its job, so not
even its first child can slip out.

Documented kernel32 calls only (job objects, Tool Help snapshots). Importable everywhere, so the layouts can
be checked on any machine; kernel32 itself is loaded on first use, on Windows.
"""

from __future__ import annotations

import ctypes
import functools
from typing import Any

CREATE_SUSPENDED = 0x00000004
CREATE_NEW_PROCESS_GROUP = 0x00000200  # Ctrl-C in the launcher's console reaches only the launcher
CREATION_FLAGS = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP

JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
JOB_OBJECT_BASIC_ACCOUNTING_INFORMATION = 1
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
PROCESS_TERMINATE = 0x0001
PROCESS_SET_QUOTA = 0x0100
TH32CS_SNAPTHREAD = 0x00000004
THREAD_SUSPEND_RESUME = 0x0002
RESUME_FAILED = 0xFFFFFFFF
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

# Windows types by their sizes (DWORD is 32 bits, LONG 32, LARGE_INTEGER 64, SIZE_T and ULONG_PTR a pointer),
# so the layouts are the same wherever this module is imported
DWORD = ctypes.c_uint32
LONG = ctypes.c_int32
BOOL = ctypes.c_int
HANDLE = ctypes.c_void_p


class BasicLimitInformation(ctypes.Structure):  # JOBOBJECT_BASIC_LIMIT_INFORMATION
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64),
        ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", DWORD),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", DWORD),
        ("SchedulingClass", DWORD),
    ]


class IoCounters(ctypes.Structure):  # IO_COUNTERS
    _fields_ = [
        (name, ctypes.c_uint64)
        for name in (
            "ReadOperationCount",
            "WriteOperationCount",
            "OtherOperationCount",
            "ReadTransferCount",
            "WriteTransferCount",
            "OtherTransferCount",
        )
    ]


class ExtendedLimitInformation(ctypes.Structure):  # JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    _fields_ = [
        ("BasicLimitInformation", BasicLimitInformation),
        ("IoInfo", IoCounters),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


class BasicAccountingInformation(ctypes.Structure):  # JOBOBJECT_BASIC_ACCOUNTING_INFORMATION
    _fields_ = [
        ("TotalUserTime", ctypes.c_int64),
        ("TotalKernelTime", ctypes.c_int64),
        ("ThisPeriodTotalUserTime", ctypes.c_int64),
        ("ThisPeriodTotalKernelTime", ctypes.c_int64),
        ("TotalPageFaultCount", DWORD),
        ("TotalProcesses", DWORD),
        ("ActiveProcesses", DWORD),
        ("TotalTerminatedProcesses", DWORD),
    ]


class ThreadEntry32(ctypes.Structure):  # THREADENTRY32
    _fields_ = [
        ("dwSize", DWORD),
        ("cntUsage", DWORD),
        ("th32ThreadID", DWORD),
        ("th32OwnerProcessID", DWORD),
        ("tpBasePri", LONG),
        ("tpDeltaPri", LONG),
        ("dwFlags", DWORD),
    ]


@functools.cache
def _kernel32() -> Any:
    """A private instance, so these prototypes never change ctypes.windll.kernel32 for anyone else."""
    k = ctypes.WinDLL("kernel32", use_last_error=True)  # type: ignore[attr-defined]
    prototypes: dict[str, tuple[Any, tuple[Any, ...]]] = {
        "CreateJobObjectW": (HANDLE, (ctypes.c_void_p, ctypes.c_wchar_p)),
        "SetInformationJobObject": (BOOL, (HANDLE, ctypes.c_int, ctypes.c_void_p, DWORD)),
        "QueryInformationJobObject": (
            BOOL,
            (HANDLE, ctypes.c_int, ctypes.c_void_p, DWORD, ctypes.POINTER(DWORD)),
        ),
        "AssignProcessToJobObject": (BOOL, (HANDLE, HANDLE)),
        "TerminateJobObject": (BOOL, (HANDLE, ctypes.c_uint)),
        "OpenProcess": (HANDLE, (DWORD, BOOL, DWORD)),
        "CreateToolhelp32Snapshot": (HANDLE, (DWORD, DWORD)),
        "Thread32First": (BOOL, (HANDLE, ctypes.POINTER(ThreadEntry32))),
        "Thread32Next": (BOOL, (HANDLE, ctypes.POINTER(ThreadEntry32))),
        "OpenThread": (HANDLE, (DWORD, BOOL, DWORD)),
        "ResumeThread": (DWORD, (HANDLE,)),
        "CloseHandle": (BOOL, (HANDLE,)),
    }
    for name, (restype, argtypes) in prototypes.items():
        fn = getattr(k, name)
        fn.restype, fn.argtypes = restype, argtypes
    return k


def _error() -> OSError:
    return ctypes.WinError(ctypes.get_last_error())  # type: ignore[attr-defined]


class Job:
    """One process tree. Closing the job, or the launcher's exit, ends whatever is still in it."""

    def __init__(self) -> None:
        k = _kernel32()
        handle = k.CreateJobObjectW(None, None)
        if not handle:
            raise _error()
        info = ExtendedLimitInformation()
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        size = ctypes.sizeof(info)
        limits = JOB_OBJECT_EXTENDED_LIMIT_INFORMATION
        if not k.SetInformationJobObject(handle, limits, ctypes.byref(info), size):
            err = _error()
            k.CloseHandle(handle)
            raise err
        self._handle: int | None = handle

    def add(self, pid: int) -> None:
        """Puts a process in the job: its children from now on are born in it too."""
        k = _kernel32()
        process = k.OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, False, pid)
        if not process:
            raise _error()
        try:
            if not k.AssignProcessToJobObject(self._handle, process):
                raise _error()
        finally:
            k.CloseHandle(process)

    def terminate(self, code: int = 1) -> None:
        """Ends every process in the job (the tree's counterpart of SIGKILL to a group)."""
        if self._handle is not None:
            _kernel32().TerminateJobObject(self._handle, code)  # fails only when nothing is left to end

    def active(self) -> int:
        """How many processes of the tree still run."""
        if self._handle is None:
            return 0
        info = BasicAccountingInformation()
        size = ctypes.sizeof(info)
        k = _kernel32()
        if not k.QueryInformationJobObject(
            self._handle, JOB_OBJECT_BASIC_ACCOUNTING_INFORMATION, ctypes.byref(info), size, None
        ):
            return 0
        return int(info.ActiveProcesses)

    def close(self) -> None:
        """Lets go of the job; KILL_ON_JOB_CLOSE ends whatever is still in it."""
        if self._handle is not None:
            _kernel32().CloseHandle(self._handle)
            self._handle = None


def resume(pid: int) -> None:
    """Lets a process started with CREATE_SUSPENDED run (it has only its first thread yet)."""
    k = _kernel32()
    snapshot = k.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if not snapshot or snapshot == INVALID_HANDLE_VALUE:
        raise _error()
    resumed = 0
    try:
        entry = ThreadEntry32()
        entry.dwSize = ctypes.sizeof(entry)
        more = k.Thread32First(snapshot, ctypes.byref(entry))
        while more:
            if entry.th32OwnerProcessID == pid:
                thread = k.OpenThread(THREAD_SUSPEND_RESUME, False, entry.th32ThreadID)
                if thread:
                    try:
                        resumed += k.ResumeThread(thread) != RESUME_FAILED
                    finally:
                        k.CloseHandle(thread)
            more = k.Thread32Next(snapshot, ctypes.byref(entry))
    finally:
        k.CloseHandle(snapshot)
    if not resumed:
        raise OSError(f"could not resume process {pid}")
