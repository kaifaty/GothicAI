#!/usr/bin/env python3
"""Run the personal game in the GNOME terminal, holding only our own gq job."""
import argparse
import contextlib
import ctypes
import fcntl
import os
import signal
import subprocess
import sys
from pathlib import Path


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smoke", action="store_true", help="fake LLM, no model requests or voice")
    return parser.parse_args(argv)


def portaudio_available():
    try:
        ctypes.CDLL("libportaudio.so.2")
        return True
    except OSError:
        return False


def session_command(repo, smoke=False, microphone=True):
    launcher = [str(Path.home() / ".local/bin/uv"), "run", "--locked", "--project",
                str(repo / "launcher/core"), "gothic-launcher", "up"]
    launcher += ["--smoke", "--new"] if smoke else ["--continue"]
    if not microphone:
        launcher.append("--no-voice")
    return [str(Path.home() / "gothic-ai/bin/gq"), "run", "--class", "quick",
            "--interactive", "--threads", "8", "--mem", "8G", "--gpu-mem", "3G",
            "--display-exclusive", "--max", "6h", "--name", "Gothic AI play", "--", *launcher]


def main(argv=None):
    args = arguments(sys.argv[1:] if argv is None else argv)
    session_lock = None
    try:
        if not os.environ.get("DISPLAY"):
            raise ValueError("Start Gothic AI from the Ryzen GNOME desktop (DISPLAY is missing)")
        icd = "/usr/share/vulkan/icd.d/nvidia_icd.json"
        if not Path(icd).is_file():
            raise ValueError(f"Missing NVIDIA Vulkan ICD: {icd}")
        os.environ.update(VK_DRIVER_FILES=icd, VK_ICD_FILENAMES=icd, GQ_OWNER="ryzen-play")
        # No e2e audio/window settings leak into a live game.
        for key in ("ALSOFT_DRIVERS", "TEMPEST_WINDOW_SIZE", "TEMPEST_WINDOW_POS", "TEMPEST_BACKGROUND",
                    "OPENGOTHIC_MAX_FPS", "GOTHIC_AI_E2E"):
            os.environ.pop(key, None)
        microphone = portaudio_available()
        if not microphone:
            print("Voice input disabled: libportaudio2 is missing. Install it yourself on Ryzen:\n"
                  "  sudo apt install libportaudio2\nGame sound remains enabled.", flush=True)
        repo = Path(__file__).resolve().parent.parent
        session_lock = (Path.home() / "gothic-ai/play.install.lock").open("a")
        try:
            fcntl.flock(session_lock, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Gothic AI install/update is running; try again after it finishes") from None
        os.chdir(repo)
        # A private pipe stays open even if the terminal/SSH has no stdin. Signals
        # cancel gq, whose cgroup contains only this launcher and its children.
        child = subprocess.Popen(session_command(repo, args.smoke, microphone), stdin=subprocess.PIPE)
        interrupted = False

        def stop(signum, _frame):
            nonlocal interrupted
            interrupted = True
            if child.poll() is None:
                child.send_signal(signum)

        previous = {sig: signal.signal(sig, stop) for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM)}
        try:
            result = child.wait()
        finally:
            child.stdin.close()
            for sig, handler in previous.items():
                signal.signal(sig, handler)
        if result and not interrupted and sys.stdin.isatty():
            with contextlib.suppress(EOFError):
                input("Gothic AI stopped. Press Enter to close this terminal.")
        return result
    except (OSError, ValueError) as error:
        print(f"Gothic AI: {error}", file=sys.stderr)
        if sys.stdin.isatty():
            with contextlib.suppress(EOFError):
                input("Press Enter to close this terminal.")
        return 2
    finally:
        if session_lock is not None:
            session_lock.close()


if __name__ == "__main__":
    sys.exit(main())
