"""Command line for the offline character tools."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys

from .face import face_track


def run_script(relative_path, arguments, *, blender_args=None):
    script = Path(__file__).resolve().parent / relative_path
    if not script.is_file():
        raise SystemExit(f"character-tool: missing packaged script {relative_path}")
    if blender_args is None:
        command = [sys.executable, script]
    else:
        command = [os.environ.get("BLENDER", "blender"), "-b", *blender_args,
                   "--python-exit-code", "1", "-P", script, "--"]
    try:
        os.execvp(command[0], [*command, *arguments])
    except OSError as error:
        print(f"character-tool: cannot execute {command[0]}: {error.strerror}", file=sys.stderr)
        return 127 if isinstance(error, FileNotFoundError) else 126


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    # These scripts parse their own arguments, including --help.
    if argv[:1] == ["head"]:
        return run_script("head/build_head.py", argv[1:])
    if argv[:2] == ["anim", "retarget"]:
        return run_script("anim/retarget_soma.py", argv[2:])

    parser = argparse.ArgumentParser(prog="character-tool", description=__doc__)
    groups = parser.add_subparsers(dest="group", required=True)
    groups.add_parser("head", help="build a head from an npc.json recipe")

    body = groups.add_parser("body", help="create, outfit, export, write or install a body")
    body = body.add_subparsers(dest="operation", required=True)
    create = body.add_parser("create", help="create the MPFB body and its landmarks with Blender")
    create.add_argument("run", help="output working directory")
    create.add_argument("mdh", help="HUMANS.MDH.json")
    outfit = body.add_parser("outfit", help="build outfit pieces on the body with Blender")
    outfit.add_argument("scene", help="body.blend from a RIG=1 run")
    outfit.add_argument("run", help="working directory for outfit.blend")
    outfit.add_argument("pieces", nargs="?", help="comma-separated piece names in build order")
    export = body.add_parser("export", help="export body and outfit parts from an MPFB scene with Blender")
    export.add_argument("scene", help="body.blend or outfit.blend")
    export.add_argument("run", help="working directory containing landmarks.json")
    export.add_argument("skeleton", help="HUMANS.MDH.json")
    write = body.add_parser("write", help="write exported parts and materials to a staged content root")
    write.add_argument("run", help="working directory containing exported parts")
    write.add_argument("mdh", help="HUMANS.MDH.json")
    write.add_argument("original_mdm", help="HUM_BODY_NAKED0.MDM.json")
    write.add_argument("skin_png", help="MPFB diffuse skin texture")
    write.add_argument("staged", help="output content root")
    write.add_argument("neck", nargs="?", help="neck colour as #rrggbb")
    install = body.add_parser("install", allow_abbrev=False, help="merge staged body records into a content root")
    install.add_argument("staged")
    install.add_argument("content")
    install.add_argument("--dry", action="store_true", help="print the proposed changes")
    install.add_argument("--models-only", action="store_true", help="install models using the target's materials")

    anim = groups.add_parser("anim", help="animation info, retarget and preview")
    anim = anim.add_subparsers(dest="operation", required=True)
    info = anim.add_parser("info", help="summarize a MAN record with NumPy")
    info.add_argument("man", help="MAN JSON record with its sibling buffer")
    anim.add_parser("retarget", help="retarget a SOMA motion to the Gothic skeleton")
    preview = anim.add_parser("preview", help="play MAN clips on the Gothic rig in Blender")
    preview.add_argument("config", help="gothic_rig JSON configuration")

    face = groups.add_parser("face", help="prepare offline facial animation (wav, json, cues, leak-table)")
    face = face.add_subparsers(dest="operation", required=True)
    wav = face.add_parser("wav", help="extract game speech as 16 kHz mono PCM with ffmpeg")
    wav.add_argument("gothic", help="Gothic installation containing Data and its speech VDFs")
    wav.add_argument("out", help="output directory")
    wav.add_argument("names", nargs="+", metavar="NAME", help="voice name with or without .WAV")
    track = face.add_parser("json", help="convert Audio2Face CSV to a 30 fps .face.json track")
    track.add_argument("csv", help="a2f-weights CSV")
    track.add_argument("out", help="output .face.json file")
    track.add_argument("--emotion", default="", help="track label, e.g. anger:0.8,disgust:0.3")
    cues = face.add_parser("cues", help="write gesture, blink and emotion cue CSVs with NumPy")
    cues.add_argument("out", type=Path, help="output directory")
    cues.add_argument("--seed", type=int, default=2026)
    leaks = face.add_parser("leak-table", help="write the expression leak table as JSON to stdout with NumPy")
    leaks.add_argument("weights", type=Path, nargs="?", help="directory of paired neutral and emotional CSVs")

    args = parser.parse_args(argv)
    try:
        if args.group == "body":
            if args.operation == "create":
                return run_script("body/mpfb_body.py", [args.run, args.mdh], blender_args=[])
            if args.operation == "outfit":
                arguments = [args.run]
                if args.pieces is not None:
                    arguments.append(args.pieces)
                return run_script("body/outfit_build.py", arguments, blender_args=[args.scene])
            if args.operation == "export":
                return run_script("body/mpfb_export.py", [args.run, args.skeleton], blender_args=[args.scene])
            if args.operation == "write":
                arguments = [args.run, args.mdh, args.original_mdm, args.skin_png, args.staged]
                if args.neck is not None:
                    arguments.append(args.neck)
                return run_script("body/mpfb_write.py", arguments)
            return run_script("body/body_install.py", argv[2:])
        if args.group == "anim":
            if args.operation == "preview":
                return run_script("anim/blender/gothic_rig.py", [args.config], blender_args=["--factory-startup"])
            return run_script("anim/gothic_anim.py", [args.man])
        if args.operation == "wav":
            return face_track.wav(args.gothic, args.out, args.names)
        if args.operation == "json":
            return face_track.track(args.csv, args.out, args.emotion)
        if args.operation == "cues":
            from .face.cues import prepare
            return prepare(args.out, args.seed)
        from .face.leak_table import leak_table
        return leak_table(args.weights)
    except ModuleNotFoundError as error:
        if error.name != "numpy":
            raise
        parser.exit(1, "character-tool: install character-tool[authoring] for NumPy\n")
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"character-tool: {error}\n")
    except subprocess.CalledProcessError as error:
        return error.returncode
