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
import hashlib
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


def project_file(project_dir, res_path):
    return os.path.join(project_dir, res_path[len("res://") :])


def check_archive(project_dir, fixture, class_name, failures):
    """The converted node is the class and the archive is a detached .gds."""
    script = fixture.get("script", "")
    scene = fixture.get("scene", "")
    if not script.startswith("res://") or not scene.startswith("res://"):
        return
    stem = script.rsplit(".", 1)[0]
    archive = project_file(project_dir, stem + ".gds")
    if not os.path.isfile(archive):
        failures.append("converted script was not archived next to the source: %s" % archive)
        return
    try:
        with open(project_file(project_dir, scene), "r", encoding="utf-8") as handle:
            scene_text = handle.read()
    except OSError as error:
        failures.append("could not read the converted scene: %s" % error)
        return
    if 'path="%s.gds"' % stem in scene_text:
        failures.append("the archive is still referenced by the scene; it must be detached")
    if 'path="%s"' % script in scene_text:
        failures.append("the live script resource is still in the scene after conversion")
    if 'type="%s"' % class_name not in scene_text:
        failures.append("the converted node is not of the class_name type %r" % class_name)

    # The manifest records the archive hash the library was built from, which is
    # what makes a later edit detectable.
    archive_res = "res://" + os.path.relpath(archive, project_dir).replace(os.sep, "/")
    entry = None
    for manifest_path in glob.glob(os.path.join(project_dir, "gdext", "*", ".grit_manifest.json")):
        try:
            with open(manifest_path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError):
            continue
        for candidate in data.get("classes", []):
            if candidate.get("archived") == archive_res:
                entry = candidate
    if entry is None:
        failures.append("no manifest entry references the archived script")
    else:
        with open(archive, "rb") as handle:
            expected_hash = hashlib.sha256(handle.read()).hexdigest()
        if entry.get("source_hash") != expected_hash:
            failures.append("the manifest's source_hash does not match the archive")


def script_class_name(directory, fixture):
    script = fixture.get("script", "")
    if not script.startswith("res://"):
        return ""
    try:
        with open(os.path.join(directory, script[len("res://") :]), "r", encoding="utf-8") as handle:
            for line in handle:
                stripped = line.strip()
                if stripped.startswith("class_name "):
                    return stripped.split()[1]
    except OSError:
        return ""
    return ""


def run_fixture(name, args, env_with_paths, session):
    """Converts and runs one fixture inside the shared session directory."""
    directory = os.path.join(FIXTURES, name)
    with open(os.path.join(directory, "fixture.json"), "r", encoding="utf-8") as handle:
        fixture = json.load(handle)
    class_name = script_class_name(directory, fixture)
    if not class_name:
        raise SystemExit("fixture %r has no class_name; conversion requires one" % name)
    expected = read_lines(os.path.join(directory, "expected.txt"))
    forbidden = read_lines(os.path.join(directory, "forbidden.txt"))

    work = os.path.join(session, name, "project")
    shutil.copytree(directory, work)
    # One HOME for the whole session keeps the editor's API dump at a stable
    # path, so godot-cpp is built once instead of once per fixture.
    home = os.path.join(session, "home")
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

    if not failures:
        check_archive(work, fixture, class_name, failures)

    game_output = ""
    if not failures:
        command = [args.editor, "--headless", "--path", work] + fixture.get("run_args", [])
        code, game_output = run(command, env, args.timeout)
        lines = game_output.splitlines()
        matched, missing = contains_in_order(lines, expected)
        if not matched:
            failures.append("expected line never appeared: %r" % missing)
        # The archived source must not run next to the generated class: each
        # expected line appears as often in the output as in expected.txt. Count
        # whole lines, since several expected lines are substrings of others
        # ("types start" inside "script types start").
        for wanted in expected:
            wanted_count = sum(1 for line in expected if line == wanted)
            seen = sum(1 for line in lines if line.strip() == wanted)
            if seen > wanted_count:
                failures.append(
                    "expected line appeared %d times, want %d (the archive may have run): %r"
                    % (seen, wanted_count, wanted)
                )
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

    session = tempfile.mkdtemp(prefix="grit-gdx-fixtures-")
    passed = 0
    for name in names:
        if run_fixture(name, args, paths, session):
            passed += 1
    if args.keep:
        print("session directory: %s" % session)
    else:
        shutil.rmtree(session, ignore_errors=True)
    print("%d/%d fixtures passed" % (passed, len(names)))
    return 0 if passed == len(names) else 1


if __name__ == "__main__":
    sys.exit(main())
