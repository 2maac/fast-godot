def can_build(env, platform):
    # macOS and Linux ship libffi with the system/OS SDK. Windows has none, so a
    # libffi prefix must be supplied through cffi_libffi_path (vcpkg works).
    return platform in ("macos", "linuxbsd", "windows")


def configure(env):
    pass


def get_opts(platform):
    from SCons.Variables import PathVariable

    return [
        PathVariable(
            "cffi_libffi_path",
            "Path to a libffi prefix (include/ and lib/libffi) - required on Windows",
            "",
            PathVariable.PathAccept,
        ),
    ]
