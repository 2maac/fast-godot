#!/usr/bin/env python3
"""Convert and run the GDExtension conversion fixtures.

Each fixture under fixtures/<name>/ is a small Godot project. The driver copies
it to a temporary directory, converts its script with the staged editor
(--grit-gdx-add ... --grit-gdx-build), verifies that the extension library was
built, runs the project headless and compares the printed output with
expected.txt (lines that must appear, in order) and forbidden.txt (lines that
must not appear).

Reading the output alone is not enough: if the extension failed to build, the
project falls back to the GDScript VM and would "pass" anyway, so the library is
checked before the run.

Usage:
    modules/grit/tests/gdextension/run_tests.py \
        --editor out/editor-staging/Fast_Godot.app/Contents/MacOS/Godot \
        [--godot-cpp DIR] [--scons PATH] [--only NAME] [--keep] [--timeout S]
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures")
LIBRARY_SUFFIXES = ("*.dylib", "*.so", "*.dll")


def read_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8") as handle:
        return [line.rstrip("\n") for line in handle if line.strip()]


def run(command, env, timeout, cwd=None):
    """Runs a command and returns (exit code, combined output), killing it on timeout."""
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    try:
        output, _ = process.communicate(timeout=timeout)
        return process.returncode, output
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
        return -1, (output or "") + "\n[driver] killed after %s s\n" % timeout


def contains_in_order(output_lines, expected):
    """True when every expected line appears as a substring, in order."""
    position = 0
    for wanted in expected:
        while position < len(output_lines) and wanted not in output_lines[position]:
            position += 1
        if position >= len(output_lines):
            return False, wanted
        position += 1
    return True, None


def convert(editor, work, fixture, env, timeout):
    extension = fixture["extension"]
    command = [
        editor,
        "--headless",
        "-e",
        "--path",
        work,
        "--grit-gdx-add",
        fixture["script"],
        "--grit-gdx-extension",
        "res://gdext/" + extension,
        "--grit-gdx-scene",
        fixture["scene"],
        "--grit-gdx-build",
    ]
    code, output = run(command, env, timeout)
    return code, output


def find_library(work):
    for pattern in LIBRARY_SUFFIXES:
        matches = glob.glob(os.path.join(work, "gdext", "*", "bin", pattern))
        if matches:
            return matches[0]
    return ""


def run_fixture(name, args, env_with_paths):
    directory = os.path.join(FIXTURES, name)
    with open(os.path.join(directory, "fixture.json"), "r", encoding="utf-8") as handle:
        fixture = json.load(handle)
    expected = read_lines(os.path.join(directory, "expected.txt"))
    forbidden = read_lines(os.path.join(directory, "forbidden.txt"))

    temp = tempfile.mkdtemp(prefix="grit-gdx-%s-" % name)
    work = os.path.join(temp, "project")
    home = os.path.join(temp, "home")
    shutil.copytree(directory, work)
    os.makedirs(home, exist_ok=True)
    # The source project files are the fixture; expected/forbidden/fixture.json
    # are not part of it.
    for extra in ("expected.txt", "forbidden.txt", "fixture.json"):
        path = os.path.join(work, extra)
        if os.path.exists(path):
            os.remove(path)

    env = dict(os.environ)
    env.update(env_with_paths)
    # A writable HOME lets the editor dump the extension API next to its data
    # directory; a read-only one makes the scons build fail with a missing
    # custom_api_file.
    env["HOME"] = home

    failures = []
    code, output = convert(args.editor, work, fixture, env, args.timeout)
    library = find_library(work)
    if code != 0 and not library:
        failures.append("conversion failed (exit %d)" % code)
        failures.append(output.strip()[-2000:])

    if not failures and not library:
        failures.append("no extension library was built; the run would use the GDScript VM")

    game_output = ""
    if not failures:
        command = [args.editor, "--headless", "--path", work] + fixture.get("run_args", [])
        code, game_output = run(command, env, args.timeout)
        lines = game_output.splitlines()
        matched, missing = contains_in_order(lines, expected)
        if not matched:
            failures.append("expected line never appeared: %r" % missing)
        for line in forbidden:
            if any(line in output_line for output_line in lines):
                failures.append("forbidden line appeared: %r" % line)

    status = "PASS" if not failures else "FAIL"
    print("%-6s %s" % (status, name))
    if failures:
        for failure in failures:
            print("       %s" % failure.replace("\n", "\n       "))
        print("       --- conversion ---")
        print("\n".join("       " + line for line in output.splitlines()[-25:]))
        print("       --- game ---")
        print("\n".join("       " + line for line in game_output.splitlines()[-25:]))

    if args.keep:
        print("       work directory: %s" % work)
    else:
        shutil.rmtree(temp, ignore_errors=True)
    return not failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor", required=True, help="staged editor binary")
    parser.add_argument("--godot-cpp", default=os.environ.get("GRIT_GDX_GODOT_CPP", ""), help="godot-cpp checkout the extension builds against")
    parser.add_argument("--scons", default=os.environ.get("GRIT_GDX_SCONS", ""), help="scons launcher")
    parser.add_argument("--only", default="", help="run one fixture")
    parser.add_argument("--keep", action="store_true", help="keep the temporary project")
    parser.add_argument("--timeout", type=int, default=600, help="seconds per step")
    args = parser.parse_args()

    if not os.path.isfile(args.editor):
        parser.error("editor not found: %s" % args.editor)

    paths = {}
    if args.godot_cpp:
        paths["GRIT_GDX_GODOT_CPP"] = args.godot_cpp
    if args.scons:
        paths["GRIT_GDX_SCONS"] = args.scons

    names = sorted(
        entry for entry in os.listdir(FIXTURES) if os.path.isdir(os.path.join(FIXTURES, entry))
    )
    if args.only:
        names = [name for name in names if name == args.only]
        if not names:
            parser.error("unknown fixture: %s" % args.only)

    passed = 0
    for name in names:
        if run_fixture(name, args, paths):
            passed += 1
    print("%d/%d fixtures passed" % (passed, len(names)))
    return 0 if passed == len(names) else 1


if __name__ == "__main__":
    sys.exit(main())
