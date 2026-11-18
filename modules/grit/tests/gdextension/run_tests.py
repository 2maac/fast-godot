#!/usr/bin/env python3
"""Build and run the GDExtension fixtures from their `.gde` sources.

Each fixture under fixtures/<name>/ is a small Godot project. Its source is a
`.gde` class source (`class_name X` + `extends Base`) and its scene already
declares the generated class as the root node's type (`type="X"`, no script
attached). The driver copies the fixture to a temporary directory, builds the
project extension with the staged editor
(`--grit-gdx-generate ... --grit-gdx-build`), verifies the generated state and
runs the project headless, comparing the printed output with expected.txt (lines
that must appear, in order) and forbidden.txt (lines that must not appear).

Reading the output alone is not enough: if the extension failed to build, the
scene node's type would not resolve and the run would fail for the wrong reason.
The library and the generated artifacts are checked before the run.

The `.gde` source is inert: it is never executed, instantiated or attached to a
node, and the build never rewrites the scene.

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
PROJECT_EXTENSION_DIR = "gdext"


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


def build_extension(editor, work, fixture, env, timeout):
    """Generates every `.gde` class in the copy and builds the project extension."""
    command = [
        editor,
        "--headless",
        "-e",
        "--path",
        work,
        "--grit-gdx-generate",
        fixture["script"],
        "--grit-gdx-build",
    ]
    code, output = run(command, env, timeout)
    return code, output


def find_library(work):
    for pattern in LIBRARY_SUFFIXES:
        matches = glob.glob(os.path.join(work, PROJECT_EXTENSION_DIR, "bin", "lib" + pattern))
        if matches:
            return matches[0]
    return ""


def project_file(project_dir, res_path):
    return os.path.join(project_dir, res_path[len("res://") :])


def script_class_name(project_dir, fixture):
    """The generated class: the value of the `class_name X` line of the `.gde` source."""
    script = fixture.get("script", "")
    if not script.startswith("res://"):
        return ""
    try:
        with open(project_file(project_dir, script), "r", encoding="utf-8") as handle:
            for line in handle:
                stripped = line.strip()
                if stripped.startswith("class_name "):
                    return stripped[len("class_name ") :].strip()
    except OSError:
        return ""
    return ""


def project_config_name(project_dir):
    """`config/name` from project.godot, i.e. what the build names the extension after."""
    path = os.path.join(project_dir, "project.godot")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError:
        return ""
    section = ""
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            continue
        if section != "application" or "=" not in stripped:
            continue
        key, value = stripped.split("=", 1)
        if key.strip() != "config/name":
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        return value
    return ""


def sanitize_project_name(name):
    """Mirrors GritGdxManifest::project_extension_name(): identifier chars survive."""
    if not name:
        return "grit_project"
    out = []
    for character in name:
        if ("a" <= character <= "z") or ("A" <= character <= "Z") or ("0" <= character <= "9") or character == "_":
            out.append(character)
        else:
            out.append("_")
    if out and "0" <= out[0] <= "9":
        out.insert(0, "_")
    return "".join(out)


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return handle.read()
    except OSError:
        return None


def load_manifest(project_dir):
    data, error = None, None
    path = os.path.join(project_dir, PROJECT_EXTENSION_DIR, ".grit_manifest.json")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except OSError as load_error:
        error = str(load_error)
    except ValueError as load_error:
        error = str(load_error)
    return data, error


def check_generated(project_dir, fixture, class_name, failures):
    """The source is inert: scene uses the class type and the build wrote the extension."""
    script = fixture.get("script", "")
    scene = fixture.get("scene", "")
    if not script.startswith("res://") or not scene.startswith("res://"):
        failures.append("fixture.json needs res:// script and scene paths")
        return

    source = project_file(project_dir, script)
    if not os.path.isfile(source):
        failures.append("the `.gde` source is missing: %s" % source)
        return
    with open(source, "rb") as handle:
        source_hash = hashlib.sha256(handle.read()).hexdigest()

    scene_text = read_text(project_file(project_dir, scene))
    if scene_text is None:
        failures.append("could not read the scene: %s" % scene)
    else:
        lines = scene_text.splitlines()
        if 'path="%s"' % script in scene_text:
            failures.append("the scene still references the `.gde` source (it must stay unattached)")
        root_index = next((i for i, line in enumerate(lines) if line.startswith("[node ")), None)
        if root_index is None:
            failures.append("the scene has no node; it must already use the generated class as its root type")
        else:
            if 'type="%s"' % class_name not in lines[root_index]:
                failures.append("the root node is not of the generated class type %r" % class_name)
            for line in lines[root_index + 1 :]:
                stripped = line.strip()
                if stripped.startswith("["):
                    break
                if stripped.startswith("script = ExtResource("):
                    failures.append("the root node still has a script attached")
                    break

    # One project extension at res://gdext/, named after the project.
    project_name = sanitize_project_name(project_config_name(project_dir))
    descriptor_name = project_name + ".gdextension"
    descriptor_path = os.path.join(project_dir, PROJECT_EXTENSION_DIR, descriptor_name)
    descriptor_text = read_text(descriptor_path)
    if descriptor_text is None:
        written = sorted(
            os.path.basename(path)
            for path in glob.glob(os.path.join(project_dir, PROJECT_EXTENSION_DIR, "*.gdextension"))
        )
        failures.append(
            "the project descriptor is missing: gdext/%s (found: %s)"
            % (descriptor_name, ", ".join(written) if written else "none")
        )

    manifest, manifest_error = load_manifest(project_dir)
    entry = None
    if manifest is None:
        failures.append("could not read gdext/.grit_manifest.json: %s" % manifest_error)
    else:
        for candidate in manifest.get("classes", []):
            if candidate.get("class") == class_name:
                entry = candidate
                break
        if entry is None:
            failures.append("no manifest entry for the generated class %r" % class_name)
        else:
            if entry.get("script") != script:
                failures.append(
                    "the manifest entry's script is %r, want %r" % (entry.get("script"), script)
                )
            if entry.get("source_hash") != source_hash:
                failures.append("the manifest entry's source_hash does not match the `.gde` source")

    if descriptor_text is not None and entry is None and class_name not in descriptor_text:
        failures.append("the generated class %r appears in neither the descriptor nor the manifest" % class_name)


def run_fixture(name, args, env_with_paths, session):
    """Builds and runs one fixture inside the shared session directory."""
    directory = os.path.join(FIXTURES, name)
    with open(os.path.join(directory, "fixture.json"), "r", encoding="utf-8") as handle:
        fixture = json.load(handle)
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
    class_name = script_class_name(work, fixture)
    if not class_name:
        failures.append("the `.gde` source has no `class_name` line")

    code, output = build_extension(args.editor, work, fixture, env, args.timeout)
    library = find_library(work)
    if code != 0 and not library:
        failures.append("extension build failed (exit %d)" % code)
        failures.append(output.strip()[-2000:])

    if not failures and not library:
        failures.append("no extension library was built; the scene's node type would not resolve")

    if not failures:
        check_generated(work, fixture, class_name, failures)

    game_output = ""
    if not failures:
        command = [args.editor, "--headless", "--path", work] + fixture.get("run_args", [])
        code, game_output = run(command, env, args.timeout)
        lines = game_output.splitlines()
        matched, missing = contains_in_order(lines, expected)
        if not matched:
            failures.append("expected line never appeared: %r" % missing)
        # The `.gde` source is never executed or instantiated, so each expected
        # line appears as often in the output as in expected.txt. Count whole
        # lines, since several expected lines are substrings of others
        # ("types start" inside "script types start").
        for wanted in expected:
            wanted_count = sum(1 for line in expected if line == wanted)
            seen = sum(1 for line in lines if line.strip() == wanted)
            if seen > wanted_count:
                failures.append(
                    "expected line appeared %d times, want %d (the `.gde` source must not run): %r"
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
        print("       --- build ---")
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
