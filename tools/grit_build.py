#!/usr/bin/env python3
"""The one build script for Fast_Godot: editor, native export and HTML5 export.

    grit_build.py editor [--platform P] [--jobs N] [--toolchain DIR]
                         [--engine DIR] [--no-deps]
    grit_build.py native_export <engine> <generated> <target> <out> <platform>
                                <profile> <arch> [optimized|size]
    grit_build.py html5_export  <engine> <generated> <target> <out> <profile>
                                [optimized|size]
    grit_build.py <engine> <generated> <target> <out> <platform> <profile> <arch>
                  [optimized|size] [key_file] [wrapped_key_file] [lto] [optimize]

The positional form without a command is what the editor invokes for an export: it
is kept stable so the editor never has to know about this script's CLI. The same
script builds the editor locally, which is why it can check (and install) the
dependencies a build needs before starting SCons.

Editor builds need the engine source tree (this file lives inside it) and a
toolchain folder for Python, SCons and the platform compiler; pass it with
--toolchain or GRIT_TOOLCHAIN, or let it read bin/grit/build_env.json. When the
toolchain ships the engine source (<toolchain>/engine/...) that copy is used
unless --engine names another checkout. Exports are started by the editor, which
writes build_env.json and the build profile; they use the same toolchain copy.

Nothing here needs a shell: the toolchain bundles ship the interpreter, SCons and
the compilers, and the only downloads are the ones a platform build genuinely
cannot carry (the Direct3D 12 Agility SDK on Windows).
"""

import glob
import hashlib
import json
import os
import secrets
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


def host_platform():
    if os.name == "nt":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linuxbsd"


def which(program, env=None):
    return shutil.which(program, path=(env or os.environ).get("PATH"))


def toolchain_python(toolchain):
    if not toolchain:
        return sys.executable
    name = "python.exe" if os.name == "nt" else os.path.join("bin", "python3")
    candidate = os.path.join(toolchain, "python", name)
    return candidate if os.path.isfile(candidate) else sys.executable


def toolchain_scons(toolchain):
    if not toolchain:
        return ""
    candidate = os.path.join(toolchain, "scons", "4.11.1", "scons-4.11.1")
    return candidate if os.path.isdir(candidate) else ""


def find_engine(start=None):
    """Locate a Fast_Godot checkout from GRIT_ENGINE, the cwd or this script."""
    starts = [os.environ.get("GRIT_ENGINE"), start, os.getcwd()]
    for base in starts:
        if not base:
            continue
        path = os.path.abspath(base)
        while True:
            if os.path.isfile(os.path.join(path, "tools", "grit_build.py")):
                return path
            parent = os.path.dirname(path)
            if parent == path:
                break
            path = parent
    return ""


def toolchain_engine_source(toolchain):
    """Return the engine source tree a toolchain bundle carries, or "".

    A toolchain built by tools/toolchain/download_toolchain.{sh,cmd} or
    fetch_linux_toolchain.sh ships the tree it was built from at
    <toolchain>/engine/<revision>/ (the fetch cache adds another level for
    downloaded archives). Editor builds and exports use that copy instead of a
    separate checkout or a fresh download.
    """
    if not toolchain:
        return ""
    root = os.path.join(os.path.abspath(toolchain), "engine")
    if not os.path.isdir(root):
        return ""

    def is_tree(path):
        return os.path.isfile(os.path.join(path, "SConstruct")) and os.path.isfile(
            os.path.join(path, "tools", "grit_build.py")
        )

    if is_tree(root):
        return root
    found = []
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name)
        if not os.path.isdir(path):
            continue
        if is_tree(path):
            found.append(path)
            continue
        for sub in sorted(os.listdir(path)):
            subpath = os.path.join(path, sub)
            if os.path.isdir(subpath) and is_tree(subpath):
                found.append(subpath)
    return found[0] if found else ""


def install_d3d12(engine, python, mingw_prefix=""):
    """The editor's D3D12 driver needs the Agility SDK; install it if it is absent."""
    if os.name != "nt":
        return
    deps = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Godot", "build_deps")
    if os.path.isdir(os.path.join(deps, "agility_sdk")):
        return
    script = os.path.join(engine, "misc", "scripts", "install_d3d12_sdk_windows.py")
    if not os.path.isfile(script):
        fail("Grit: %s is missing, so the Direct3D 12 SDK cannot be installed." % script, 2)
    announce("Grit: installing the Direct3D 12 Agility SDK (one time)")
    env = dict(os.environ)
    if mingw_prefix:
        env["MINGW_PREFIX"] = mingw_prefix
    code = subprocess.call([python, script], cwd=engine, env=env)
    if code != 0:
        fail("Grit: the Direct3D 12 dependency installer failed (exit %d)" % code, 5)
    if not os.path.isdir(os.path.join(deps, "agility_sdk")):
        fail("Grit: the Direct3D 12 dependency installer reported success but %s is "
             "still missing." % os.path.join(deps, "agility_sdk"), 5)


def install_angle(engine, python):
    """The editor's ANGLE (GLES3) driver needs the static ANGLE drops."""
    if os.name != "nt":
        return
    deps = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Godot", "build_deps")
    if os.path.isdir(os.path.join(deps, "angle-x86_64-gcc")) or os.path.isdir(os.path.join(deps, "angle")):
        return
    script = os.path.join(engine, "misc", "scripts", "install_angle.py")
    if not os.path.isfile(script):
        fail("Grit: %s is missing, so ANGLE cannot be installed." % script, 2)
    announce("Grit: installing the ANGLE rendering driver dependencies (one time)")
    code = subprocess.call([python, script], cwd=engine)
    if code != 0:
        fail("Grit: the ANGLE dependency installer failed (exit %d)" % code, 5)
    if not (os.path.isdir(os.path.join(deps, "angle-x86_64-gcc")) or os.path.isdir(os.path.join(deps, "angle"))):
        fail("Grit: the ANGLE installer reported success but %s is still missing."
             % os.path.join(deps, "angle-x86_64-gcc"), 5)


def check_export_dependencies(platform, env):
    """Fail early, with an actionable message, when a build input is missing."""
    toolchain = env.get("toolchain_root") or ""
    if platform == "web":
        emsdk = os.path.join(toolchain, "emsdk")
        if not os.path.isdir(emsdk):
            fail("Grit: HTML5 Fast needs the emsdk that ships inside the toolchain "
                 "folder, and %s has none. Point Editor Settings > Grit > ToolChain "
                 "Root Folder Path at a Fast_Godot_Toolchain_* bundle." % (toolchain or "the configured path"), 5)
    elif platform == "linuxbsd":
        compiler = os.path.join(toolchain, "gcc", "bin", "g++")
        if not os.path.isfile(compiler):
            compiler = os.path.join(toolchain, "compiler", "clang")
        if not os.path.isfile(compiler) and not which("g++"):
            fail("Grit: no C++ compiler. Use a toolchain bundle that carries gcc/ "
                 "(tools/toolchain/make_linux_gcc.sh builds one).", 5)
        libffi = os.path.join(toolchain, "libffi", "linux-x86_64", "include")
        if not os.path.isdir(libffi) and not os.path.isdir("/usr/include/libffi"):
            pass  # CFFI's SCsub falls back to the system libffi; the link will say so.
    elif platform == "windows":
        libffi = os.path.join(toolchain, "libffi", "3.8.0", "mingw64", "include")
        if not os.path.isdir(libffi):
            fail("Grit: the Windows toolchain has no libffi/3.8.0/mingw64 prefix, and "
                 "the CFFI module needs one. Use a Fast_Godot_Toolchain_win64 bundle.", 5)
    elif platform == "macos":
        if not which("xcrun"):
            fail("Grit: macOS builds need the Xcode Command Line Tools (xcrun).", 5)


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
            # Every .gen.* file in a checkout is a build product (modules_enabled.gen.h,
            # register_module_types.gen.cpp, shader tables...). Seeding one would import
            # another platform's configuration into this tree: a stale
            # modules_enabled.gen.h made register_module_types call modules the web
            # build had compiled out, which is the "undefined symbol" link failure.
            # (The old check was a literal `endswith(".gen.*")`, so it never matched.)
            if ".gen." in name or name.startswith(SKIP_GLOBS):
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


def export_main(argv):
    if len(argv) < 8:
        fail(__doc__, 2)
    engine, generated, target, out_path, platform = argv[1], argv[2], argv[3], argv[4], argv[5]
    profile, arch = argv[6], argv[7] if len(argv) > 7 else "arm64"
    # HTML5 Fast preset option "Optimized": the web platform builds with -Os to keep
    # the download small; -O3 is measurably faster on Grit-compiled code and costs
    # ~7 MB of wasm. Native platforms are always -O3 for release builds.
    optimized = len(argv) > 8 and argv[8] == "optimized"
    # Optional 10th argument: a file holding the project's 256-bit AES script/PCK
    # encryption key as 64 hex characters (empty file = no key). Older helpers in
    # an older toolchain simply ignore the extra argument.
    script_key_file = argv[9] if len(argv) > 9 else ""
    encryption_key = ""
    if script_key_file:
        try:
            with open(script_key_file, "r", encoding="utf-8") as key_handle:
                encryption_key = key_handle.read().strip().lower()
        except OSError:
            encryption_key = ""
    if encryption_key and (len(encryption_key) != 64 or any(c not in "0123456789abcdef" for c in encryption_key)):
        fail("Grit: invalid script encryption key; expected 64 hexadecimal characters.", 7)
    # Optional 11th argument: the per-export pack key. In the keystream form it is
    # the pack data key itself, baked into the template's white-box tables; in the
    # older wrapped form it is that key wrapped with the user key, and only the
    # wrapped form reaches the template.
    wrapped_key_file = argv[10] if len(argv) > 10 else ""
    wrapped_key = ""
    if wrapped_key_file:
        try:
            with open(wrapped_key_file, "r", encoding="utf-8") as wrapped_handle:
                wrapped_key = wrapped_handle.read().strip().lower()
        except OSError:
            wrapped_key = ""
    if wrapped_key and (len(wrapped_key) != 64 or any(c not in "0123456789abcdef" for c in wrapped_key)):
        what = "PCK pack key" if keystream_mode else "wrapped PCK data key"
        fail("Grit: invalid %s; expected 64 hexadecimal characters." % what, 8)
    # Optional 12th argument: the preset's LTO mode ("auto", "none", "thin",
    # "full"). "auto" keeps the default policy; older helpers ignore the argument.
    # GRIT_LTO is the marker the editor looks for before offering the option.
    lto_mode = argv[11].strip().lower() if len(argv) > 11 else "auto"
    if lto_mode not in ("auto", "none", "thin", "full"):
        fail("Grit: invalid LTO mode %r; expected auto, none, thin or full." % lto_mode, 9)
    # Optional 13th argument: the preset's optimization level ("none", "speed",
    # "size"). Older helpers ignore it and keep the historical defaults (native
    # release is -O3, web is -Os unless the legacy "optimized" flag says -O3).
    # GRIT_OPTIMIZE is the marker the editor looks for before offering the option.
    optimize_mode = argv[12].strip().lower() if len(argv) > 12 else ""
    if optimize_mode and optimize_mode not in ("none", "speed", "size"):
        fail("Grit: invalid optimization level %r; expected none, speed or size." % optimize_mode, 10)
    # Optional 14th argument: "keystream" means the 11th argument's file holds the
    # pack key itself rather than a wrapped copy. The generator bakes it into the
    # white-box tables and exposes only AES block encryption, so the exported game
    # never materializes the key. GRIT_WB_KEYSTREAM is the marker the editor looks
    # for before it stops wrapping the pack key (older helpers ignore the argument
    # and keep the wrapped form).
    keystream_mode = len(argv) > 13 and argv[13].strip().lower() == "keystream"
    # Optional 15th argument: "vm" builds the template without this project's
    # generated code, so the module compiles its fallback (empty) registry and the
    # game runs on the GDScript VM. The editor passes it when the preset's Grit
    # option is off (or GRIT_DISABLE=1 is set); older helpers ignore it.
    vm_mode = len(argv) > 14 and argv[14].strip().lower() == "vm"

    engine = os.path.abspath(engine)
    work_dir = os.path.join(engine, "bin", "grit", target)
    os.makedirs(work_dir, exist_ok=True)
    # Deliberately not last_build.log: the editor tails that one while the build
    # runs, and on Windows its open handle denies the helper's write.
    log_path = os.path.join(work_dir, "build.log")

    # Everything below reports into the log the editor follows for progress.
    # Written before anything else and never tailed, so a failure before the log is
    # usable still leaves a trace of what ran.
    try:
        with open(os.path.join(work_dir, "helper_argv.txt"), "w", encoding="utf-8") as diag:
            diag.write("argv: %r\npython: %s\n" % (sys.argv, sys.executable))
    except OSError:
        pass

    # The editor tails this file while the build runs. On Windows a reader without
    # share-write makes open() fail with a sharing violation, so retry briefly
    # instead of dying with exit 1 and an empty log.
    log = None
    last_error = None
    for _ in range(100):
        try:
            log = open(log_path, "w", encoding="utf-8", buffering=1)
            break
        except OSError as err:
            last_error = err
            time.sleep(0.1)
    if log is None:
        try:
            with open(os.path.join(work_dir, "helper_error.txt"), "w", encoding="utf-8") as diag:
                diag.write("could not open %s: %r\n" % (log_path, last_error))
        except OSError:
            pass
        raise SystemExit("Grit: could not open the build log at %s (%r)" % (log_path, last_error))
    sys.stdout = log
    sys.stderr = log
    print("Grit: helper started (python %s)" % sys.version.split()[0], flush=True)

    if not vm_mode and not os.path.isdir(generated):
        fail("Grit: no generated code in %s" % generated, 3)
    if not profile or not os.path.isfile(profile):
        fail("Grit: build profile not found: %s" % profile, 4)

    env = read_env(engine)
    check_export_dependencies(platform, env)
    extra = env.get("scons_args", "").split()
    toolchain_root = env.get("toolchain_root") or ""
    bundled_gcc = platform == "linuxbsd" and os.path.isfile(
        os.path.join(toolchain_root, "gcc", "bin", "g++")
    )
    # Link-time optimization. "auto" keeps the historical policy: the bundled
    # Linux GCC does full LTO cheaply, the web wasm was byte-for-byte the same
    # with and without Emscripten's thin LTO (so it only cost build time), macOS
    # defaults to none upstream, and the older Zig bundles could not do it at
    # all. A preset can force a mode; thin needs an LLVM compiler and full needs
    # a compiler that has an LTO pipeline, so the combinations that cannot work
    # are refused here instead of failing inside the linker.
    # GRIT_LTO
    if lto_mode == "auto":
        lto_mode = "full" if bundled_gcc else "none"
    if lto_mode == "thin" and platform in ("linuxbsd", "windows"):
        fail(
            "Grit: thin LTO needs an LLVM compiler, but %s templates use GCC. "
            "Choose Full or None for this preset."
            % ("Linux" if platform == "linuxbsd" else "Windows"),
            9,
        )
    if lto_mode == "full" and platform == "linuxbsd" and not bundled_gcc:
        fail(
            "Grit: this Linux toolchain compiles with Zig, which cannot do LTO. "
            "Choose None, or re-download the build tools to get the bundled GCC 9.",
            9,
        )
    extra = [arg for arg in extra if not arg.startswith("lto=")]
    extra.append("lto=" + lto_mode)
    print("Grit: template LTO mode: %s" % lto_mode, flush=True)
    extra_paths = list(env.get("path_add") or [])
    if platform == "linuxbsd":
        # A Linux toolchain can carry its own compiler (focal GCC 9, or the older
        # Zig): the bundle then needs no distribution -dev packages. Godot vendors
        # the X11/ALSA headers and dlopens those libraries, so the compiler is all
        # that was missing.
        gcc_bin = os.path.join(toolchain_root, "gcc", "bin")
        if bundled_gcc:
            extra += [
                "CC=" + os.path.join(gcc_bin, "gcc"),
                "CXX=" + os.path.join(gcc_bin, "g++"),
            ]
            extra_paths.insert(0, gcc_bin)
            announce("Grit: using the bundled GCC 9 (LTO)")
        compiler_dir = os.path.join(toolchain_root, "compiler")
        clang = os.path.join(compiler_dir, "clang")
        clangxx = os.path.join(compiler_dir, "clang++")
        if not bundled_gcc and os.path.isfile(clang) and os.path.isfile(clangxx):
            # Zig links its own libc++ statically; -static-libstdc++ is not a
            # thing there, so the static-cpp flag has to go.
            # Zig links its own libc++ statically; -static-libstdc++ is not a
            # thing there, so the static-cpp flag has to go. LTO flags are also
            # pointless (Zig does not do LTO) and only cost memory, which matters
            # for glslang's generated tables.
            extra += ["CC=" + clang, "CXX=" + clangxx, "use_static_cpp=no"]
            extra_paths.insert(0, compiler_dir)  # ar, ranlib, pkg-config stub
            announce("Grit: using the bundled Linux compiler %s" % clang)
        else:
            announce("Grit: the toolchain has no compiler; using the system C++ toolchain")
        # CFFI needs libffi headers and a static archive; the toolchain carries
        # them so no libffi-dev is required on the machine.
        libffi = os.path.join(toolchain_root, "libffi", "linux-x86_64")
        if os.path.isdir(os.path.join(libffi, "include")):
            extra.append("cffi_libffi_path=" + libffi)
        else:
            announce("Grit: no bundled libffi; CFFI needs the system libffi headers")
    if platform == "web":
        # Web inherits nothing from the host build: vulkan, mingw, d3d12 and the
        # libffi path are all meaningless for wasm, and passing them in makes the
        # build configure drivers that do not exist on this platform.
        extra = [arg for arg in extra if not arg.startswith(("vulkan", "use_mingw", "cffi_libffi_path", "accesskit", "d3d12", "winrt", "angle"))]
        # HTML5 Fast never includes CFFI: libffi has no wasm target here.
        extra.append("module_cffi_enabled=no")
        # Godot's web template defaults to threads=yes, which needs
        # SharedArrayBuffer and therefore cross-origin isolation (COOP/COEP
        # headers). A Fast export has to run from any static host, so build the
        # template single-threaded; this also drops proxy_to_pthread.
        extra.append("threads=no")

    # Optimization level. The preset's mode wins; without one, fall back to the
    # historical behaviour (web -O3 only when the legacy "optimized" flag asked
    # for it, native whatever SCons infers for the target).
    if optimize_mode:
        extra = [arg for arg in extra if not arg.startswith("optimize=")]
        # A debug template exists for troubleshooting: the engine deliberately
        # builds it at -O2 (speed_trace) so crash backtraces stay readable, while
        # a release template wants -O3. Speed therefore means speed_trace for a
        # debug target; None and Size are explicit and pass through unchanged.
        # GRIT_OPTIMIZE
        build_optimize = "speed_trace" if optimize_mode == "speed" and target != "release" else optimize_mode
        extra.append("optimize=" + build_optimize)
    elif platform == "web" and optimized:
        extra.append("optimize=speed")

    with open(profile, "rb") as handle:
        key = hashlib.sha256(handle.read())
    # The engine revision is part of the key: a tree seeded by another revision
    # keeps *.gen.* files that are not refreshed, and then fails to link.
    key.update((env.get("engine_id") or "").encode("utf-8"))
    # A template built with an encryption key has to be separate from (and
    # rebuilt instead of reusing) a keyless one: the key is compiled into the
    # binary, so it is part of the build identity.
    key.update(("enc:" + encryption_key).encode("utf-8"))
    if vm_mode:
        # A VM-only tree shares nothing with a compiled one: separate directories so
        # switching the option never reuses the other mode's objects.
        key.update(b"vm-only")
    profile_hash = key.hexdigest()[:16]
    work_root = os.environ.get("GRIT_PROFILE_WORKDIR") or os.path.join(engine, "out", "grit-profiles")
    source = os.path.join(work_root, "%s-%s-%s-%s" % (profile_hash, platform, arch, target))

    if os.path.isdir(source):
        announce("Grit: refreshing isolated tree %s" % source)
    else:
        announce("Grit: seeding isolated tree %s (first build for this profile is a full one)" % source)
        os.makedirs(source, exist_ok=True)
    if vm_mode:
        announce("Grit: building a VM-only template (no generated code)")
    announce("Grit: rebuild with build profile in %s" % source)
    seed_tree(engine, source, generated)
    announce("Grit: rebuild with build profile in %s" % source)

    jobs = os.environ.get("GRIT_JOBS") or str(os.cpu_count() or 8)
    command = scons_command(env, extra) + [
        "platform=%s" % platform,
        "target=template_%s" % target,
        "arch=%s" % arch,
        "build_profile=%s" % profile,
    ]
    if not vm_mode:
        command.append("grit_generated=%s" % generated)
    command.append("-j%s" % jobs)

    # The build has to see the toolchain it was provisioned with, even though the
    # editor process it was started from does not.
    child_env = dict(os.environ)
    if extra_paths:
        child_env["PATH"] = os.pathsep.join(extra_paths + [child_env.get("PATH", "")])
    # core/SCsub reads this environment variable and compiles the key into the
    # template; the exported pack is encrypted with the same key.
    if encryption_key:
        child_env["SCRIPT_AES256_ENCRYPTION_KEY"] = encryption_key
        announce("Grit: template will embed the project's script encryption key")
    else:
        child_env.pop("SCRIPT_AES256_ENCRYPTION_KEY", None)
    if encryption_key and wrapped_key:
        # Randomizes the white-box table encodings, so every export has a
        # different table network for the same key.
        child_env["GRIT_WB_SEED"] = secrets.token_hex(16)
        if keystream_mode:
            child_env["GRIT_PACK_KEY"] = wrapped_key
            child_env.pop("GRIT_WRAPPED_KEY", None)
            announce("Grit: template will decode the pack through its white-box keystream")
        else:
            child_env["GRIT_WRAPPED_KEY"] = wrapped_key
            child_env.pop("GRIT_PACK_KEY", None)
            announce("Grit: template will unwrap the per-export PCK data key")
    else:
        child_env.pop("GRIT_WRAPPED_KEY", None)
        child_env.pop("GRIT_PACK_KEY", None)
        child_env.pop("GRIT_WB_SEED", None)
    if env.get("pythonpath"):
        child_env["PYTHONPATH"] = env["pythonpath"]
    mingw_prefix = os.path.join(env.get("toolchain_root") or "", "mingw64")
    if platform == "windows" and os.path.isdir(mingw_prefix):
        child_env["MINGW_PREFIX"] = mingw_prefix
    if platform == "web":
        # emcc, and the node it drives, come from the toolchain folder.
        emsdk = os.path.join(env.get("toolchain_root", ""), "emsdk")
        if os.path.isdir(emsdk):
            em = os.path.join(emsdk, "upstream", "emscripten")
            nodes = sorted(glob.glob(os.path.join(emsdk, "node", "*", "bin")))
            child_env["EMSDK"] = emsdk
            config = os.path.join(emsdk, ".emscripten")
            if os.path.isfile(config):
                child_env["EM_CONFIG"] = config
            # The emcc launcher runs Emscripten through Python. macOS and Linux
            # usually have an interpreter on PATH; a Windows machine supplied only
            # with a toolchain bundle does not, so point the launcher at the
            # toolchain's own interpreter instead of the "python" app alias.
            python = toolchain_python(toolchain_root)
            child_env["EMSDK_PYTHON"] = python
            child_env["PATH"] = os.pathsep.join(
                [em] + nodes + [os.path.dirname(python), child_env.get("PATH", "")]
            )
        else:
            announce("Grit: no emsdk in the toolchain folder; web build relies on PATH")

    announce("Grit: " + " ".join(command))
    # Redirecting sys.stdout does not touch fd 1, so the child's output has to be
    # sent to the log explicitly or every scons error would vanish.
    process = subprocess.Popen(command, cwd=source, env=child_env, stdout=log, stderr=log)
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

    if platform == "web":
        # The exporter renames "godot" to the export name and expects canonical
        # entries, so the build's arch-suffixed names are rewritten here and the
        # loader's internal references follow the new names.
        prefix = "godot.web.template_%s.wasm32" % target
        produced = {}
        for item in sorted(glob.glob(os.path.join(source, "bin", prefix + "*"))):
            suffix = os.path.basename(item)[len(prefix):]
            # A threads=no build is named <prefix>.nothreads.<ext>; the extra marker
            # must not hide the file from the canonical mapping below (that silently
            # packaged a template with only the HTML shells).
            if suffix.startswith(".nothreads."):
                suffix = suffix[len(".nothreads"):]
            produced[suffix] = item
        if not produced:
            fail("Grit: build finished but no %s* files were produced in %s/bin" % (prefix, source), 6)
        # extension -> canonical entry name
        # The shell does `new Engine(...)`, so the file it loads must be the wrapped
        # bundle (loader + engine API). Mapping the plain emscripten loader here was
        # what produced "ReferenceError: Engine is not defined".
        canonical = {".wasm": "godot.wasm", ".wrapped.js": "godot.js", ".js": "godot.loader.js", ".engine.js": "godot.engine.js"}
        missing = [suffix for suffix in (".wasm", ".wrapped.js") if suffix not in produced]
        if missing:
            fail("Grit: template build produced no %s" % ", ".join(prefix + suffix for suffix in missing), 6)
        shell_dir = os.path.join(engine, "misc", "dist", "html")
        libs_dir = os.path.join(engine, "platform", "web", "js", "libs")
        extras = [
            (os.path.join(shell_dir, "full-size.html"), "godot.html"),
            (os.path.join(shell_dir, "offline.html"), "godot.offline.html"),
            # The engine fetches these at runtime; without them the page logs
            # "Failed to load worklet module script" 404s and has no audio.
            (os.path.join(libs_dir, "audio.worklet.js"), "godot.audio.worklet.js"),
            (os.path.join(libs_dir, "audio.position.worklet.js"), "godot.audio.position.worklet.js"),
        ]
        out_zip = out_path if out_path.endswith(".zip") else out_path + ".zip"
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as bundle:
            for suffix, entry in canonical.items():
                if suffix not in produced:
                    continue
                with open(produced[suffix], "rb") as handle:
                    data = handle.read()
                # Point every textual reference at the canonical name (both the plain
                # and the threads=no spelling of the build file names).
                for build_suffix, canonical_suffix in canonical.items():
                    for marker in ("", ".nothreads"):
                        data = data.replace((prefix + marker + build_suffix).encode(), canonical_suffix.encode())
                bundle.writestr(entry, data)
            for path, entry in extras:
                if os.path.isfile(path):
                    bundle.write(path, entry)
                elif entry.endswith(".worklet.js"):
                    fail("Grit: build finished but %s is missing" % path, 6)
        announce("Grit: produced %s" % out_zip)
        return 0

    if platform == "macos":
        binary = os.path.join(source, "bin", "godot.macos.template_%s.%s" % (target, arch))
    elif platform == "windows":
        binary = os.path.join(source, "bin", "godot.windows.template_%s.%s.exe" % (target, arch))
    else:
        binary = os.path.join(source, "bin", "godot.linuxbsd.template_%s.%s" % (target, arch))
        # A clang/LLVM build appends ".llvm" to every target name.
        if not os.path.isfile(binary) and os.path.isfile(binary + ".llvm"):
            binary += ".llvm"
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


def parse_editor_args(argv):
    options = {
        "platform": host_platform(),
        "jobs": str(os.cpu_count() or 8),
        "toolchain": os.environ.get("GRIT_TOOLCHAIN", ""),
        "engine": os.environ.get("GRIT_ENGINE", ""),
        "deps": True,
        "extra": [],
    }
    rest = list(argv)
    while rest:
        arg = rest.pop(0)
        if arg in ("--platform", "-p") and rest:
            options["platform"] = rest.pop(0)
        elif arg in ("--jobs", "-j") and rest:
            options["jobs"] = rest.pop(0)
        elif arg in ("--toolchain", "-t") and rest:
            options["toolchain"] = rest.pop(0)
        elif arg in ("--engine", "-e") and rest:
            options["engine"] = rest.pop(0)
        elif arg == "--no-deps":
            options["deps"] = False
        elif arg in ("-h", "--help"):
            print(__doc__)
            raise SystemExit(0)
        elif arg.startswith("-"):
            fail("Grit: unknown editor option %s" % arg, 2)
        else:
            options["extra"].append(arg)
    return options


def build_editor(argv):
    """Build the editor for this host, installing what the build needs first."""
    options = parse_editor_args(argv)
    platform = options["platform"]
    toolchain = options["toolchain"]
    engine = options["engine"]
    # The toolchain is the source of truth: it ships the engine tree it was built
    # from, so use that unless --engine explicitly asks for another checkout.
    if not engine and toolchain:
        engine = toolchain_engine_source(toolchain)
        if engine:
            announce("Grit: using the engine source in the toolchain at %s" % engine)
    if not engine:
        engine = find_engine(os.path.dirname(os.path.abspath(__file__)))
    if not engine:
        fail("Grit: cannot find a Fast_Godot source tree. Pass --engine DIR or point "
             "--toolchain at a bundle that carries one (or set GRIT_ENGINE); the tree "
             "must contain tools/grit_build.py.", 2)
    engine = os.path.abspath(engine)
    if not os.path.isfile(os.path.join(engine, "tools", "grit_build.py")):
        fail("Grit: %s does not look like a Fast_Godot checkout (no "
             "tools/grit_build.py)." % engine, 2)
    if not toolchain:
        toolchain = read_env(engine).get("toolchain_root") or ""
    if toolchain:
        toolchain = os.path.abspath(toolchain)
    if toolchain and not os.path.isdir(toolchain):
        fail("Grit: toolchain folder not found: %s" % toolchain, 2)
    env = dict(os.environ)
    python = toolchain_python(toolchain)
    paths = []
    scons = toolchain_scons(toolchain)
    if scons:
        env["PYTHONPATH"] = scons
    # Production defaults (no debug symbols, static C++ runtime, LTO "auto" would
    # be overridden by the explicit lto=none below) for editor builds too.
    extra = ["production=yes", "lto=none"]

    if platform == "windows":
        mingw = os.path.join(toolchain, "mingw64", "bin")
        if os.path.isdir(mingw):
            paths.insert(0, mingw)
        elif not which("gcc", env):
            fail("Grit: no MinGW compiler; point --toolchain at a "
                 "Fast_Godot_Toolchain_win64 bundle.", 5)
        libffi = os.path.join(toolchain, "libffi", "3.8.0", "mingw64")
        # detect.py asks MINGW_PREFIX for the MinGW headers (D3D12); setting it here
        # keeps this build's signatures identical to the batch launcher's.
        env["MINGW_PREFIX"] = os.path.join(toolchain, "mingw64")
        winrt = os.path.join(toolchain, "winrt")
        if os.path.isdir(winrt):
            extra += ["winrt=yes", "winrt_path=" + winrt]
        else:
            extra += ["winrt=no"]
        extra += ["use_mingw=yes", "use_volk=yes", "vulkan=yes", "accesskit=no"]
        if os.path.isdir(libffi):
            extra.append("cffi_libffi_path=" + libffi)
        else:
            fail("Grit: the toolchain has no libffi/3.8.0/mingw64 prefix, which CFFI "
                 "needs on Windows.", 5)
        angle = os.path.join(toolchain, "angle")
        if os.path.isdir(angle):
            # The toolchain ships the static ANGLE libraries; no download needed.
            extra.append("angle_libs=" + angle)
        elif options["deps"]:
            install_angle(engine, python)
        mesa = os.path.join(toolchain, "mesa-x86_64-gcc")
        agility = os.path.join(toolchain, "agility_sdk")
        if os.path.isdir(mesa) and os.path.isdir(agility):
            # Bundled D3D12 drops: no download and no LOCALAPPDATA dependency.
            extra += ["mesa_libs=" + os.path.join(toolchain, "mesa"), "agility_sdk_path=" + agility]
        elif options["deps"]:
            install_d3d12(engine, python, os.path.join(toolchain, "mingw64") if toolchain else "")
    elif platform == "linuxbsd":
        gcc_bin = os.path.join(toolchain, "gcc", "bin")
        compiler = os.path.join(toolchain, "compiler")
        if os.path.isfile(os.path.join(gcc_bin, "g++")):
            extra += [
                "CC=" + os.path.join(gcc_bin, "gcc"),
                "CXX=" + os.path.join(gcc_bin, "g++"),
            ]
            paths.insert(0, gcc_bin)
        elif os.path.isfile(os.path.join(compiler, "clang")):
            extra += [
                "CC=" + os.path.join(compiler, "clang"),
                "CXX=" + os.path.join(compiler, "clang++"),
                "use_static_cpp=no",
            ]
            paths.insert(0, compiler)
        elif not which("g++", env):
            fail("Grit: no C++ compiler; use a toolchain built with "
                 "tools/toolchain/make_linux_gcc.sh.", 5)
        libffi = os.path.join(toolchain, "libffi", "linux-x86_64")
        if os.path.isdir(os.path.join(libffi, "include")):
            extra.append("cffi_libffi_path=" + libffi)
        extra.append("use_volk=yes")
    elif platform == "macos":
        if not which("xcrun", env):
            fail("Grit: macOS builds need the Xcode Command Line Tools (xcrun).", 5)
        moltenvk = os.path.join(toolchain, "moltenvk")
        if os.path.isdir(os.path.join(moltenvk, "macOS", "lib", "MoltenVK.xcframework")):
            extra += ["vulkan=yes", "vulkan_sdk_path=" + moltenvk]
        # Keep clang's module cache inside the tree: TMPDIR is not writable in some
        # sandboxes, and the shared cache also speeds up repeated builds.
        if not any(arg.startswith("ccflags=") for arg in options["extra"]):
            extra.append("ccflags=-fmodules-cache-path=" + os.path.join(engine, "out", "clangmods"))
    else:
        fail("Grit: unknown platform %s" % platform, 2)

    if paths:
        env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
    command = [python, "-m", "SCons", "platform=" + platform, "target=editor",
               "-j" + str(options["jobs"])] + extra + options["extra"]
    announce("Grit: " + " ".join(command))
    return subprocess.call(command, cwd=engine, env=env)


def cli_export(kind, rest):
    """Explicit CLI forms; the editor uses the positional form without a command."""
    if kind == "html5_export":
        if len(rest) < 5:
            fail("usage: grit_build.py html5_export <engine> <generated> <target> "
                 "<out> <profile> [optimized|size]", 2)
        engine, generated, target, out, profile = rest[:5]
        optimization = rest[5] if len(rest) > 5 else "optimized"
        return export_main(["grit_build.py", engine, generated, target, out, "web",
                            profile, "wasm32", optimization])
    if len(rest) < 7:
        fail("usage: grit_build.py native_export <engine> <generated> <target> <out> "
             "<platform> <profile> <arch> [optimized|size]", 2)
    return export_main(["grit_build.py"] + rest[:8])


def main(argv):
    if len(argv) > 1 and argv[1] == "editor":
        return build_editor(argv[2:])
    if len(argv) > 1 and argv[1] in ("native_export", "html5_export"):
        return cli_export(argv[1], argv[2:])
    return export_main(argv)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except SystemExit:
        raise
    except Exception:
        import traceback
        try:
            with open(os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])) if len(sys.argv) > 1 else ".", "bin", "grit", "helper_error.txt"), "w", encoding="utf-8") as diag:
                diag.write("argv: %r\n\n" % (sys.argv,))
                traceback.print_exc(file=diag)
        except OSError:
            pass
        traceback.print_exc()
        sys.stdout.flush()
        print("Grit: internal error in the build helper", flush=True)
        sys.exit(1)
