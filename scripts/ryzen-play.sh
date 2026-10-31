#!/usr/bin/env python3
"""Manage the personal Ryzen checkout from this Mac (stdlib only).

install/update also transfer content and settings. After one manual update,
autoupdate install enables a user systemd timer every 15 minutes;
autoupdate uninstall disables it. The timer
runs this installed script's __auto action: update/build while the game is closed,
roll back failed builds, and leave content/settings alone. autoupdate status shows
the timer, checkout commits and last result (~/gothic-ai/play-update.json).
"""
import argparse
from datetime import datetime, timezone
import fcntl
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path


def arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    for action in ("install", "update"):
        actions.add_parser(action)
    automatic = actions.add_parser("autoupdate")
    automatic.add_argument("command", choices=("install", "uninstall", "status"))
    return parser.parse_args(argv)


def run(command, **kwargs):
    print("+ " + shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def owned_process(command, payload=None, *, keep_stdin=False):
    # Keep stdin open: gq run cancels its own job on EOF, including an SSH loss.
    child = subprocess.Popen(command, stdin=subprocess.PIPE if payload is not None or keep_stdin else None)
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
    settings["game"]["dir"] = str(home / "gothic-ai/game-settings")
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
        text = path.read_text()
        # Change only the installation input; keep all personal settings and comments.
        section = re.search(r"(?m)^\[game\][ \t]*(?:#.*)?$", text)
        value = f'dir = {json.dumps(settings["game"]["dir"])}'
        if section is None:
            text += f"\n[game]\n{value}\n"
        else:
            end = re.search(r"(?m)^\[", text[section.end():])
            stop = section.end() + end.start() if end else len(text)
            game = text[section.end():stop]
            game, replaced = re.subn(r"(?m)^[ \t]*dir[ \t]*=.*$", value, game, count=1)
            if not replaced:
                game = f"\n{value}" + game
            text = text[:section.end()] + game + text[stop:]
        path.write_text(text)
        print(f"Updated game.dir in {path} (personal settings kept)", flush=True)
        return False
    print(f"Created {path} (no secrets copied)", flush=True)
    return True


def build_command(source):
    return [str(Path.home() / "gothic-ai/bin/gq"), "run", "--class", "quick",
            "--threads", "12", "--mem", "12G", "--max", "30m",
            "--name", "Gothic AI install/update", "--", *prepare_command(source)]


def prepare_command(source):
    return ["bash", "-euc", 'source scripts/ryzen-build.sh; exec "$@"', "ryzen-play",
            sys.executable, "-c", source, "__prepare"]


def sync_content():
    content = Path.home() / ".local/share/gothic-ai/content"
    with tempfile.TemporaryDirectory(prefix="gothic-play-content-") as empty:
        source = content if content.is_dir() else Path(empty)
        run(["rsync", "-a", "--delete", "--checksum", "--delete-excluded",
             "--include=/materials/***", "--include=/meshes/***", "--include=/enabled.json", "--exclude=*",
             "-e", "ssh -o BatchMode=yes -o ConnectTimeout=10",
             "--rsync-path=mkdir -p ~/.local/share/gothic-ai/play-content.incoming && rsync",
             f"{source}/", "ryzen:.local/share/gothic-ai/play-content.incoming/"])


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
    flags = subprocess.check_output([
        "bash", "-euc", 'source "$1"; printf "%s\\0" "${RYZEN_CMAKE_FLAGS[@]}"',
        "ryzen-build", str(repo / "scripts/ryzen-build.sh"),
    ]).decode().split("\0")[:-1]
    build = repo / "OpenGothic/build"
    run(["cmake", "-S", repo / "OpenGothic", "-B", build, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", *flags,
         "-DOPENGOTHIC_ENABLE_APP_CADENCE=ON", f"-DCMAKE_CXX_FLAGS=-I{sdk}/include",
         f"-DCMAKE_EXE_LINKER_FLAGS=-L{sdk}/lib"])
    run(["cmake", "--build", build, "--target", "Gothic2Notr", "--parallel", "12"])
    # The server's NPC knowledge/director needs this converter on a fresh machine.
    graph = repo / "OpenGothic/ai_bridge/storygraph"
    run(["cmake", "-S", graph, "-B", graph / "build", "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", *flags,
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


def write_desktop_entry(repo, result):
    applications = Path.home() / ".local/share/applications"
    applications.mkdir(parents=True, exist_ok=True)
    (applications / "gothic-ai.desktop").write_text(desktop_entry(repo, launcher_ready=result == 0))


def check_tools(home):
    queue = home / "gothic-ai/bin/gq"
    if not os.access(queue, os.X_OK):
        raise ValueError("gq is not installed: run scripts/gq install ryzen on the Mac")
    queue_help = subprocess.run([str(queue), "run", "--help"], check=False, capture_output=True, text=True)
    if queue_help.returncode or "--interactive" not in queue_help.stdout:
        raise ValueError("Installed gq lacks --interactive: run scripts/gq install ryzen on the Mac")
    if not (home / ".local/bin/uv").is_file():
        raise ValueError("Missing ~/.local/bin/uv on Ryzen")


def check_checkout(repo, expected_origin):
    branch = subprocess.check_output(["git", "-C", repo, "branch", "--show-current"], text=True).strip()
    origin = subprocess.check_output(["git", "-C", repo, "remote", "get-url", "origin"], text=True).strip()
    if branch != "main" or origin != expected_origin:
        raise ValueError("~/gothic-ai/play must be a main checkout of this origin")
    if subprocess.check_output(["git", "-C", repo, "status", "--porcelain"], text=True).strip():
        raise ValueError("~/gothic-ai/play has local changes; keep them before updating")


def atomic_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, prefix=f".{path.name}.", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(text)
            stream.close()
            temporary.chmod(0o644)
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def auto_update(expected_origin, *, admitted=False):
    home = Path.home()
    repo = home / "gothic-ai/play"
    state_path = repo.parent / "play-update.json"
    state = {"old_commit": "", "new_commit": "", "failed_commit": ""}

    def finish(result, code=0, **details):
        state.update(time=datetime.now(timezone.utc).isoformat(), result=result, **details)
        text = json.dumps(state, sort_keys=True) + "\n"
        print(text, end="", flush=True)
        atomic_write(state_path, text)
        return code

    def commit(ref):
        return subprocess.check_output(["git", "-C", repo, "rev-parse", ref], text=True).strip()

    try:
        if state_path.exists():
            state["failed_commit"] = json.loads(state_path.read_text())["failed_commit"]
        if not admitted:
            run(["git", "-C", repo, "fetch", "origin"])
        state.update(old_commit=commit("HEAD"), new_commit=commit("origin/main"))
        if not admitted:
            if state["old_commit"] == state["new_commit"]:
                return finish("up_to_date")
            if state["failed_commit"] == state["new_commit"]:
                return finish("build_failed_rolled_back", detail="Skipping previously failed origin/main")
            previous = state_path.read_text() if state_path.exists() else None
            os.chdir(repo)
            os.environ["GQ_OWNER"] = "ryzen-play-autoupdate"
            command = build_command("")
            command = command[:command.index("--") + 1] + [
                sys.executable, str(repo / "scripts/ryzen-play.sh"), "__auto_build", expected_origin]
            # Wait without locking or changing the checkout. systemd serializes runs.
            result = owned_process(command, keep_stdin=True)
            current = state_path.read_text() if state_path.exists() else None
            if current == previous:
                return finish("error", result or 2, error=f"gq exited {result} without an update result")
            return result
        with (repo.parent / "play.install.lock").open("a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return finish("busy")
            check_tools(home)
            check_checkout(repo, expected_origin)
            # A manual update may have finished while this job was queued.
            state.update(old_commit=commit("HEAD"), new_commit=commit("origin/main"))
            if state["old_commit"] == state["new_commit"]:
                return finish("up_to_date")
            if state["failed_commit"] == state["new_commit"]:
                return finish("build_failed_rolled_back", detail="Skipping previously failed origin/main")
            run(["git", "-C", repo, "pull", "--ff-only"])
            state["new_commit"] = commit("HEAD")
            os.chdir(repo)
            script = repo / "scripts/ryzen-play.sh"
            result = subprocess.run(prepare_command(script.read_text()), check=False).returncode
            if result in (0, 3):
                write_desktop_entry(repo, result)
                state["failed_commit"] = ""
                return finish("updated")
            state["failed_commit"] = state["new_commit"]
            state["build_exit"] = result
            run(["git", "-C", repo, "reset", "--hard", state["old_commit"]])
            rollback = subprocess.run(prepare_command(script.read_text()), check=False).returncode
            state["rollback_exit"] = rollback
            if rollback not in (0, 3):
                raise ValueError(f"Rollback build failed with exit {rollback}")
            write_desktop_entry(repo, rollback)
            return finish("build_failed_rolled_back", result)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        return finish("error", 2, error=str(error))


def manage_autoupdate(action, origin=None):
    home = Path.home()
    units = home / ".config/systemd/user"
    if action == "install":
        script = home / "gothic-ai/play/scripts/ryzen-play.sh"
        if not script.is_file() or '"__auto"' not in script.read_text():
            raise ValueError("Installed ryzen-play.sh lacks autoupdate: run scripts/ryzen-play.sh update first")
        # systemd uses double-quoted arguments and expands percent specifiers.
        argument = json.dumps(origin.replace("%", "%%"))
        atomic_write(units / "gothic-play-update.service", (
            "[Unit]\nDescription=Update the personal Gothic AI installation\n\n"
            "[Service]\nType=oneshot\nTimeoutStartSec=infinity\n"
            "WorkingDirectory=%h/gothic-ai/play\n"
            f"ExecStart=/usr/bin/python3 %h/gothic-ai/play/scripts/ryzen-play.sh __auto {argument}\n"
            "Environment=PATH=%h/.local/bin:/usr/local/bin:/usr/bin:/bin\n"))
        atomic_write(units / "gothic-play-update.timer", (
            "[Unit]\nDescription=Check for Gothic AI updates every 15 minutes\n\n"
            "[Timer]\nOnCalendar=*:0/15\nPersistent=true\n\n"
            "[Install]\nWantedBy=timers.target\n"))
        run(["systemctl", "--user", "daemon-reload"])
        run(["systemctl", "--user", "enable", "--now", "gothic-play-update.timer"])
    elif action == "uninstall":
        run(["systemctl", "--user", "disable", "--now", "gothic-play-update.timer"])
        # Removing the files lets an active oneshot finish its build/rollback.
        for name in ("gothic-play-update.service", "gothic-play-update.timer"):
            (units / name).unlink(missing_ok=True)
        run(["systemctl", "--user", "daemon-reload"])
    else:
        subprocess.run(["systemctl", "--user", "status", "gothic-play-update.timer", "--no-pager"], check=False)
        run(["systemctl", "--user", "list-timers", "gothic-play-update.timer", "--all", "--no-pager"])
        repo = home / "gothic-ai/play"
        for ref in ("HEAD", "origin/main"):
            value = subprocess.check_output(["git", "-C", repo, "rev-parse", ref], text=True).strip()
            print(f"{ref}: {value}", flush=True)
        state = home / "gothic-ai/play-update.json"
        print(state.read_text() if state.exists() else "No automatic update result yet.", flush=True)
    return 0


def remote_install(action, payload, source):
    home = Path.home()
    helper = {"__name__": "ryzen_game"}
    exec(compile(payload["game_helper"], "ryzen_game.py", "exec"), helper)
    helper["game_environment"](home=home)
    repo = home / "gothic-ai/play"
    repo.parent.mkdir(parents=True, exist_ok=True)
    with (repo.parent / "play.install.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Gothic AI is open, playing or updating; close it before install/update") from None
        check_tools(home)
        if not repo.exists():
            if action == "update":
                raise ValueError("No ~/gothic-ai/play: run scripts/ryzen-play.sh install first")
            run(["git", "clone", "--branch", "main", payload["origin"], repo])
        check_checkout(repo, payload["origin"])
        run(["git", "-C", repo, "pull", "--ff-only"])
        os.chdir(repo)
        os.environ["GQ_OWNER"] = "ryzen-play-install"
        result = owned_process(build_command(source))
        if result not in (0, 3):
            return result
        content = home / ".local/share/gothic-ai/play-content"
        incoming = content.with_name("play-content.incoming")
        old = content.with_name("play-content.old")
        if old.exists():
            shutil.rmtree(old)
        if content.exists():
            content.rename(old)
        incoming.rename(content)
        if old.exists():
            shutil.rmtree(old)
        create_settings(home / ".config/gothic-ai/settings.toml", payload["settings"], repo, home)
        write_desktop_entry(repo, result)
        print("Ready: open Gothic AI in GNOME Applications. Login on Ryzen: "
              "~/.local/bin/uv run --project ~/gothic-ai/play/launcher/core gothic-launcher login", flush=True)
    return result


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv == ["__prepare"]:
            return prepare()
        if argv[:1] == ["__auto"]:
            return auto_update(argv[1])
        if argv[:1] == ["__auto_build"]:
            return auto_update(argv[1], admitted=True)
        if argv[:1] == ["__autoupdate"]:
            payload = json.loads(sys.stdin.readline())
            return manage_autoupdate(argv[1], payload.get("origin"))
        if argv[:1] == ["__remote"]:
            payload = json.loads(sys.stdin.readline())
            return remote_install(argv[1], payload, payload["installer"])
        args = arguments(argv)
        root = Path(__file__).resolve().parent.parent
        source = Path(__file__).read_text()
        if args.action == "autoupdate":
            payload = {}
            if args.command == "install":
                payload["origin"] = subprocess.check_output(
                    ["git", "-C", root, "remote", "get-url", "origin"], text=True).strip()
            command = shlex.join(["python3", "-c", source, "__autoupdate", args.command])
            return owned_process(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                                  "ryzen", command], payload)
        origin = subprocess.check_output(["git", "-C", root, "remote", "get-url", "origin"], text=True).strip()
        with (Path.home() / ".config/gothic-ai/settings.toml").open("rb") as stream:
            settings = tomllib.load(stream)
        # Never read secrets.toml/chatgpt.json or put private settings in argv/logs.
        game_helper = Path(__file__).with_name("ryzen_game.py").read_text()
        payload = {"origin": origin, "settings": {key: settings.get(key, {}) for key in ("game", "llm", "voice", "tts")},
                   "installer": source, "game_helper": game_helper}
        run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "ryzen",
             shlex.join(["python3", "-c", game_helper, "--check"])])
        sync_content()
        command = shlex.join(["python3", "-c", source, "__remote", args.action])
        return owned_process(["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                              "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
                              "ryzen", command], payload)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"ryzen-play: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
