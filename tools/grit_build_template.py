#!/usr/bin/env python3
"""Builds an export template that contains a project's generated Grit code.

    grit_build_template.py <engine_root> <generated_dir> <release|debug> <out_binary>
                           <platform> <profile_json> <arch>

The build profile is what enables Grit in the template: the module only builds when
grit_generated is set, so a plain template is stock Godot.

This is the portable replacement for grit_build_template.sh. It needs nothing but
the interpreter it runs under, which is why a self-contained toolchain folder has
no use for a bundled Git shell: the only thing that was needed from it is already
here.

The editor writes bin/grit/build_env.json next to the build; it carries the paths
the build has to use (SCons package, MinGW, the interpreter itself).
"""

import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile

# Copied from the checkout into the isolated tree. The isolated tree keeps its own
# bin/ and SCons state, which is what makes a second build incremental.
SKIP_DIRS = {".git", "bin", "out", ".godot"}
SKIP_SUFFIXES = (".app",)
SKIP_GLOBS = (".sconsign", "compile_commands.json")


def fail(message, code):
    print(message, flush=True)
    sys.exit(code)


def announce(message):
    print(message, flush=True)


def read_env(engine):
    """Paths the editor recorded for this build, if any."""
    path = os.path.join(engine, "bin", "grit", "build_env.json")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError:
        return {}
    except ValueError:
        return {}


def scons_command(env, scons_extra):
    python = env.get("python") or sys.executable
    return [python, "-m", "SCons"] + scons_extra


def seed_tree(engine, dest, generated):
    """Copy the checkout into the isolated tree, skipping this build's own output."""
    generated_dir = os.path.abspath(generated).replace("\\", "/").rstrip("/")
    copied = 0
    for root, dirs, files in os.walk(engine):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.endswith(SKIP_SUFFIXES)]
        rel = os.path.relpath(root, engine)
        target_dir = dest if rel == "." else os.path.join(dest, rel)
        os.makedirs(target_dir, exist_ok=True)
        for name in files:
            if name.endswith(".gen.*") or name.startswith(SKIP_GLOBS):
                continue
            source = os.path.join(root, name)
            if os.path.abspath(source).replace("\\", "/").startswith(generated_dir):
                continue
            try:
                shutil.copy2(source, os.path.join(target_dir, name))
                copied += 1
            except OSError:
                continue
    return copied


def package_macos(out_binary, out_path, engine):
    """macOS templates are a zip whose root is macos_template.app."""
    app = os.path.join(engine, "misc", "dist", "macos_template.app")
    stage = out_path + ".stage"
    shutil.rmtree(stage, ignore_errors=True)
    shutil.copytree(app, os.path.join(stage, "macos_template.app"))
    macos_dir = os.path.join(stage, "macos_template.app", "Contents", "MacOS")
    os.makedirs(macos_dir, exist_ok=True)
    for name in ("godot_macos_release.arm64", "godot_macos_debug.arm64"):
        shutil.copy2(out_binary, os.path.join(macos_dir, name))
        os.chmod(os.path.join(macos_dir, name), 0o755)
    with zipfile.ZipFile(out_path + ".zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for root, dirs, files in os.walk(os.path.join(stage, "macos_template.app")):
            for name in files:
                full = os.path.join(root, name)
                arc = os.path.join("macos_template.app", os.path.relpath(full, os.path.join(stage, "macos_template.app")))
                archive.write(full, arc)
    shutil.rmtree(stage, ignore_errors=True)


def main(argv):
    if len(argv) < 8:
        fail(__doc__, 2)
    engine, generated, target, out_path, platform = argv[1], argv[2], argv[3], argv[4], argv[5]
    profile, arch = argv[6], argv[7] if len(argv) > 7 else "arm64"

    engine = os.path.abspath(engine)
    work_dir = os.path.join(engine, "bin", "grit", target)
    os.makedirs(work_dir, exist_ok=True)
    log_path = os.path.join(work_dir, "last_build.log")

    # Everything below reports into the log the editor follows for progress.
    log = open(log_path, "w", encoding="utf-8", buffering=1)
    sys.stdout = log
    sys.stderr = log

    if not os.path.isdir(generated):
        fail("Grit: no generated code in %s" % generated, 3)
    if not profile or not os.path.isfile(profile):
        fail("Grit: build profile not found: %s" % profile, 4)

    env = read_env(engine)
    extra = env.get("scons_args", "").split()

    with open(profile, "rb") as handle:
        profile_hash = hashlib.sha256(handle.read()).hexdigest()[:16]
    work_root = os.environ.get("GRIT_PROFILE_WORKDIR") or os.path.join(engine, "out", "grit-profiles")
    source = os.path.join(work_root, "%s-%s-%s-%s" % (profile_hash, platform, arch, target))

    if os.path.isdir(source):
        announce("Grit: refreshing isolated tree %s" % source)
    else:
        announce("Grit: seeding isolated tree %s (first build for this profile is a full one)" % source)
        os.makedirs(source, exist_ok=True)
    announce("Grit: rebuild with build profile in %s" % source)
    seed_tree(engine, source, generated)
    announce("Grit: rebuild with build profile in %s" % source)

    jobs = os.environ.get("GRIT_JOBS") or str(os.cpu_count() or 8)
    command = scons_command(env, extra) + [
        "platform=%s" % platform,
        "target=template_%s" % target,
        "arch=%s" % arch,
        "build_profile=%s" % profile,
        "grit_generated=%s" % generated,
        "-j%s" % jobs,
    ]

    # The build has to see the toolchain it was provisioned with, even though the
    # editor process it was started from does not.
    child_env = dict(os.environ)
    path_add = env.get("path_add") or []
    if path_add:
        child_env["PATH"] = os.pathsep.join(path_add + [child_env.get("PATH", "")])
    if env.get("pythonpath"):
        child_env["PYTHONPATH"] = env["pythonpath"]

    announce("Grit: " + " ".join(command))
    process = subprocess.Popen(command, cwd=source, env=child_env)
    cancel_file = os.path.join(work_dir, "cancel_build")
    try:
        os.remove(cancel_file)
    except OSError:
        pass
    with open(os.path.join(work_dir, "scons.pid"), "w", encoding="ascii") as handle:
        handle.write(str(process.pid))

    while True:
        code = process.poll()
        if code is not None:
            break
        if os.path.exists(cancel_file):
            announce("Grit: cancel requested, stopping scons")
            process.kill()
            process.wait()
            for stale in (cancel_file, os.path.join(work_dir, "scons.pid")):
                try:
                    os.remove(stale)
                except OSError:
                    pass
            sys.exit(3)
        time.sleep(0.2)

    try:
        os.remove(os.path.join(work_dir, "scons.pid"))
    except OSError:
        pass
    if code != 0:
        fail("Grit: scons exited with %d" % code, 5)

    if platform == "macos":
        binary = os.path.join(source, "bin", "godot.macos.template_%s.%s" % (target, arch))
    elif platform == "windows":
        binary = os.path.join(source, "bin", "godot.windows.template_%s.%s.exe" % (target, arch))
    else:
        binary = os.path.join(source, "bin", "godot.linuxbsd.template_%s.%s" % (target, arch))
    if not os.path.isfile(binary):
        fail("Grit: build finished but %s is missing" % binary, 6)

    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)
    shutil.copy2(binary, out_path)
    if platform == "macos":
        package_macos(out_path, out_path, engine)
        os.remove(out_path)
        out_path = out_path + ".zip"
    announce("Grit: produced %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
