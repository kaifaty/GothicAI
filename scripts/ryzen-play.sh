#!/usr/bin/env python3
"""Install/update the personal Ryzen checkout from this Mac (stdlib only)."""
import argparse
import fcntl
import json
import os
import shlex
import shutil
import signal
import subprocess
import sys
import tomllib
from pathlib import Path


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "update"))
    return parser.parse_args(argv)


def run(command, **kwargs):
    print("+ " + shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def owned_process(command, payload=None):
    # Keep stdin open: gq run cancels its own job on EOF, including an SSH loss.
    child = subprocess.Popen(command, stdin=subprocess.PIPE if payload is not None else None)
    previous = {}

    def stop(signum, _frame):
        if child.poll() is None:
            child.send_signal(signum)

    for sig in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        previous[sig] = signal.signal(sig, stop)
    try:
        if payload is not None:
            child.stdin.write((json.dumps(payload) + "\n").encode())
            child.stdin.flush()
        return child.wait()
    finally:
        if child.stdin is not None:
            child.stdin.close()
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def create_settings(path, source, repo, home):
    """Copy only non-secret settings; no Mac paths or existing Ryzen settings."""
    settings = {key: dict(source.get(key, {})) for key in ("game", "llm", "voice", "tts")}
    settings["game"]["dir"] = str(home / "gothic-ai/game")
    settings["voice"].pop("mic_device", None)
    settings["paths"] = {"repo": str(repo), "uv": str(home / ".local/bin/uv")}
    lines = []
    for section, values in settings.items():
        lines.append(f"[{section}]")
        for key, value in values.items():
            # Launcher settings have scalar fields. JSON strings/booleans are valid TOML.
            if not key.replace("_", "").isalnum() or not isinstance(value, (str, bool, int, float)):
                raise ValueError(f"unsupported setting: {section}.{key}")
            lines.append(f"{key} = {json.dumps(value, ensure_ascii=False, allow_nan=False)}")
        lines.append("")
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    try:
        with path.open("x") as stream:
            stream.write("\n".join(lines))
    except FileExistsError:
        print(f"Keeping existing {path}", flush=True)
        return False
    print(f"Created {path} (no secrets copied)", flush=True)
    return True


def build_command(source):
    return [str(Path.home() / "gothic-ai/bin/gq"), "run", "--class", "quick",
            "--threads", "12", "--mem", "12G", "--max", "30m",
            "--name", "Gothic AI install/update", "--", sys.executable, "-c", source, "__prepare"]


def prepare():
    repo = Path.cwd()
    sdk = Path.home() / "gothic-ai/body-baseline/toolchain/vulkan-1.4.341.0"
    os.environ["VULKAN_SDK"] = str(sdk)
    os.environ["PATH"] = f"{Path.home()}/.local/bin:{sdk}/bin:" + os.environ["PATH"]
    for tool in ("cmake", "ninja", "gcc", "g++", "glslangValidator", "uv"):
        if shutil.which(tool) is None:
            raise ValueError(f"Missing tool: {tool}")
    if not (sdk / "include/vulkan/vulkan.h").is_file() or not (sdk / "lib/libvulkan.so").is_file():
        raise ValueError(f"Incomplete user Vulkan SDK: {sdk}")
    build = repo / "OpenGothic/build"
    run(["cmake", "-S", repo / "OpenGothic", "-B", build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
         "-DOPENGOTHIC_ENABLE_APP_CADENCE=ON", f"-DCMAKE_CXX_FLAGS=-I{sdk}/include",
         f"-DCMAKE_EXE_LINKER_FLAGS=-L{sdk}/lib"])
    run(["cmake", "--build", build, "--target", "Gothic2Notr", "--parallel", "12"])
    # The server's NPC knowledge/director needs this converter on a fresh machine.
    graph = repo / "OpenGothic/ai_bridge/storygraph"
    run(["cmake", "-S", graph, "-B", graph / "build", "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
         "-DCMAKE_CXX_FLAGS=-include algorithm"])
    run(["cmake", "--build", graph / "build", "--target", "storygraph", "--parallel", "12"])
    for project in ("server", "asr-service", "tts-service", "launcher/core"):
        run([Path.home() / ".local/bin/uv", "sync", "--locked", "--project", repo / project])
    os.environ["CARGO_BUILD_JOBS"] = "12"
    try:
        run([repo / "scripts/build-launcher.sh", "--no-install"])
    except subprocess.CalledProcessError:
        print("Launcher build failed; Gothic AI will keep its terminal launcher", file=sys.stderr, flush=True)
        return 3
    return 0


def desktop_entry(repo, launcher_ready=True):
    # Desktop Entry quoting differs from shell quoting (notably percent and dollar).
    binary = repo / "launcher/app/src-tauri/target/release/gothic-launcher"
    window = launcher_ready and os.access(binary, os.X_OK)
    executable = str(binary if window else repo / "scripts/ryzen-play-session.sh")
    quoted = '"' + executable.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$").replace("`", "\\`").replace("%", "%%") + '"'
    command = quoted if window else f'ptyxis --new-window "--title=Gothic AI" -- {quoted}'
    return ("[Desktop Entry]\nType=Application\nName=Gothic AI\n"
            "Comment=Gothic II с ИИ\n"
            f"Exec={command}\n"
            f"Icon={repo}/launcher/app/src-tauri/icons/128x128.png\n"
            "Terminal=false\nCategories=Game;RolePlaying;\nStartupNotify=true\n")


def remote_install(action, payload, source):
    home = Path.home()
    repo = home / "gothic-ai/play"
    repo.parent.mkdir(parents=True, exist_ok=True)
    with (repo.parent / "play.install.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Gothic AI is open, playing or updating; close it before install/update") from None
        queue = home / "gothic-ai/bin/gq"
        if not os.access(queue, os.X_OK):
            raise ValueError("gq is not installed: run scripts/gq install ryzen on the Mac")
        queue_help = subprocess.run([str(queue), "run", "--help"], check=False, capture_output=True, text=True)
        if queue_help.returncode or "--interactive" not in queue_help.stdout:
            raise ValueError("Installed gq lacks --interactive: run scripts/gq install ryzen on the Mac")
        if not (home / ".local/bin/uv").is_file():
            raise ValueError("Missing ~/.local/bin/uv on Ryzen")
        if not (home / "gothic-ai/game").is_dir():
            raise ValueError("Missing ~/gothic-ai/game on Ryzen")
        if not repo.exists():
            if action == "update":
                raise ValueError("No ~/gothic-ai/play: run scripts/ryzen-play.sh install first")
            run(["git", "clone", "--branch", "main", payload["origin"], repo])
        branch = subprocess.check_output(["git", "-C", repo, "branch", "--show-current"], text=True).strip()
        origin = subprocess.check_output(["git", "-C", repo, "remote", "get-url", "origin"], text=True).strip()
        if branch != "main" or origin != payload["origin"]:
            raise ValueError("~/gothic-ai/play must be a main checkout of this origin")
        if subprocess.check_output(["git", "-C", repo, "status", "--porcelain"], text=True).strip():
            raise ValueError("~/gothic-ai/play has local changes; keep them before updating")
        run(["git", "-C", repo, "pull", "--ff-only"])
        os.chdir(repo)
        os.environ["GQ_OWNER"] = "ryzen-play-install"
        result = owned_process(build_command(source))
        if result not in (0, 3):
            return result
        create_settings(home / ".config/gothic-ai/settings.toml", payload["settings"], repo, home)
        applications = home / ".local/share/applications"
        applications.mkdir(parents=True, exist_ok=True)
        (applications / "gothic-ai.desktop").write_text(desktop_entry(repo, launcher_ready=result == 0))
        print("Ready: open Gothic AI in GNOME Applications. Login on Ryzen: "
              "~/.local/bin/uv run --project ~/gothic-ai/play/launcher/core gothic-launcher login", flush=True)
    return result


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv == ["__prepare"]:
            return prepare()
        if argv[:1] == ["__remote"]:
            payload = json.loads(sys.stdin.readline())
            return remote_install(argv[1], payload, payload["installer"])
        args = arguments(argv)
        root = Path(__file__).resolve().parent.parent
        origin = subprocess.check_output(["git", "-C", root, "remote", "get-url", "origin"], text=True).strip()
        with (Path.home() / ".config/gothic-ai/settings.toml").open("rb") as stream:
            settings = tomllib.load(stream)
        # Never read secrets.toml/chatgpt.json or put private settings in argv/logs.
        source = Path(__file__).read_text()
        payload = {"origin": origin, "settings": {key: settings.get(key, {}) for key in ("game", "llm", "voice", "tts")},
                   "installer": source}
        command = shlex.join(["python3", "-c", source, "__remote", args.action])
        return owned_process(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                              "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
                              "ryzen", command], payload)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ryzen-play: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
