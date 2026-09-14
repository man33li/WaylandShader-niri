#!/usr/bin/env python3
"""Build this niri fork and stage separately named WaylandShader executables."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


RASHADER_REV = "a910bee8d2ead0acf2f83b3e5ad0b8f8f66b53db"
ROOT = Path(__file__).resolve().parent.parent
SUPPORT = ROOT / "waylandshader"
WORK = ROOT / "build"


def run(*args, cwd=None, env=None):
    print("+", shlex.join(str(arg) for arg in args), flush=True)
    subprocess.run([str(arg) for arg in args], cwd=cwd, env=env, check=True)


def checkout_rashader():
    path = WORK / "librashader-source"
    if not path.exists():
        run("git", "init", path)
        run("git", "remote", "add", "origin", "https://github.com/SnowflakePowered/librashader.git", cwd=path)
        run("git", "fetch", "--depth=1", "origin", RASHADER_REV, cwd=path)
        run("git", "checkout", "--detach", "FETCH_HEAD", cwd=path)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=path, text=True).strip()
    top = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], cwd=path, text=True).strip()
    if Path(top).resolve() != path.resolve():
        raise RuntimeError(f"{path} is not a standalone checkout; refusing to use it")
    if actual != RASHADER_REV:
        raise RuntimeError(f"{path} is at {actual}, expected {RASHADER_REV}; refusing to reset an existing checkout")
    patch = SUPPORT / "patches/librashader-gl-lifetime.patch"
    check = subprocess.run(["git", "apply", "--check", str(patch)], cwd=path, capture_output=True)
    if check.returncode == 0:
        run("git", "apply", patch, cwd=path)
    elif subprocess.run(["git", "apply", "--reverse", "--check", str(patch)],
                        cwd=path, capture_output=True).returncode != 0:
        raise RuntimeError(f"{patch.name} neither applies nor is already applied in {path}:\n"
                           + check.stderr.decode(errors="replace"))
    return path


def staging_paths(parser, prefix_arg, destdir_arg):
    prefix = (prefix_arg or WORK / "niri-install").expanduser().absolute()
    destdir = destdir_arg.expanduser().resolve() if destdir_arg else None
    if ".." in prefix.parts or prefix == Path("/"):
        parser.error("--prefix must not be / or contain parent-directory components")
    if destdir:
        if destdir == Path("/") or any(destdir.is_relative_to(Path(path)) for path in
                                      ("/usr", "/etc", "/bin", "/sbin", "/lib", "/lib64", "/boot")):
            parser.error("--destdir must be a staging directory, not a system directory")
        staging = destdir
        installed = destdir / prefix.relative_to("/")
    else:
        staging = WORK
        installed = prefix
        if installed.resolve() == WORK or not installed.resolve().is_relative_to(WORK):
            parser.error("This script only stages builds: a prefix outside build/ requires --destdir")
    if installed.resolve().is_relative_to(ROOT) and not installed.resolve().is_relative_to(WORK):
        parser.error("The staging destination must not overwrite checkout sources")
    for name in ("librashader-source", "niri-deps", "niri-support"):
        source = WORK / name
        if installed.resolve().is_relative_to(source) or source.is_relative_to(installed.resolve()):
            parser.error("The staging destination must not overlap dependency build directories")
    for relative in ("", "bin", "lib/waylandshader", "share/applications", "share/wayland-sessions",
                     "share/doc/niri-waylandshader", "share/licenses/niri-waylandshader"):
        if not (installed / relative).resolve().is_relative_to(staging):
            parser.error(f"Staging path {installed / relative} escapes {staging}")
    return prefix, destdir, installed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--prefix", type=Path, help="Install prefix (default: build/niri-install)")
    parser.add_argument("--destdir", type=Path, help="Stage a package prefix inside this directory")
    parser.add_argument("--support-only", action="store_true",
                        help="Build the private shader library, bridge and controls; do not build or install niri")
    parser.add_argument("--tests", action="store_true", help="Also build the real GPU regression (does not run it)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if WORK.resolve() != WORK:
        parser.error("build/ must not be redirected outside the checkout by a symlink")
    for relative in ("librashader-source", "librashader-source/target", "niri-deps/lib", "niri-support"):
        if not (WORK / relative).resolve().is_relative_to(WORK):
            parser.error(f"{WORK / relative} escapes the build workspace")
    if not (ROOT / "target").resolve().is_relative_to(ROOT):
        parser.error("target/ must remain inside the checkout")
    prefix, destdir, installed = staging_paths(parser, args.prefix, args.destdir)
    for command in ("git", "cargo", "cmake", "ninja", "pkg-config", "c++"):
        if not shutil.which(command):
            raise RuntimeError(f"Missing build tool: {command}")
    WORK.mkdir(parents=True, exist_ok=True)
    rashader = checkout_rashader()
    build_env = os.environ.copy()
    build_env["CMAKE_POLICY_VERSION_MINIMUM"] = "3.5"
    build_env["CARGO_TARGET_DIR"] = str(rashader / "target")
    run("cargo", "rustc", "--manifest-path", rashader / "Cargo.toml", "--release", "--locked",
        "-p", "librashader-capi", "--no-default-features", "--features", "runtime-opengl,stable",
        "--jobs", args.jobs, "--", "-C", "link-arg=-Wl,-soname,libwaylandshader-rashader.so.2", env=build_env)
    private_lib = WORK / "niri-deps/lib"
    private_lib.mkdir(parents=True, exist_ok=True)
    library = private_lib / "libwaylandshader-rashader.so.2"
    if library.is_symlink():
        raise RuntimeError(f"Refusing to overwrite symlink {library}")
    shutil.copy2(rashader / "target/release/liblibrashader_capi.so", library)
    linker_name = private_lib / "libwaylandshader-rashader.so"
    if linker_name.is_symlink():
        if linker_name.readlink() != Path(library.name):
            raise RuntimeError(f"Unexpected library link: {linker_name}")
    elif linker_name.exists():
        raise RuntimeError(f"Expected a linker symlink, not a file: {linker_name}")
    else:
        linker_name.symlink_to(library.name)

    support = WORK / "niri-support"
    run("cmake", "-S", SUPPORT, "-B", support, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
        f"-DLIBRASHADER_INCLUDE_DIR={rashader / 'include'}", f"-DLIBRASHADER_LIBRARY={library}",
        f"-DRASHADER_SOURCE_DIR={rashader}", f"-DNIRI_EXECUTABLE={ROOT / 'target/release/niri'}",
        f"-DWAYLANDSHADER_BUILD_TESTS={'ON' if args.tests else 'OFF'}", env=build_env)
    run("cmake", "--build", support, "--parallel", args.jobs, env=build_env)
    build_env["WAYLANDSHADER_BRIDGE_LIB_DIR"] = str(support / "lib")
    build_env["WAYLANDSHADER_RASHADER_LIB_DIR"] = str(private_lib)
    build_env["LD_LIBRARY_PATH"] = str(private_lib) + (
        ":" + build_env["LD_LIBRARY_PATH"] if build_env.get("LD_LIBRARY_PATH") else "")
    build_env["CARGO_TARGET_DIR"] = str(ROOT / "target")
    if args.tests:
        print("GPU regression built, NOT run. Run on a supported GPU:")
        print(shlex.join([str(support / "niri_bridge_test"), str(SUPPORT / "tests/fixtures")]))
        print("Exit 77 means unavailable/skipped, not a passing GPU regression.")
    if args.support_only:
        print("\nNative support built; no compositor build or installation performed.")
        print("Environment for ordinary cargo commands from this checkout:")
        for name in ("WAYLANDSHADER_BRIDGE_LIB_DIR", "WAYLANDSHADER_RASHADER_LIB_DIR",
                     "CARGO_TARGET_DIR", "LD_LIBRARY_PATH"):
            print(f"export {name}={shlex.quote(build_env[name])}")
        return
    run("cargo", "build", "--manifest-path", ROOT / "Cargo.toml", "--release", "--locked",
        "--bin", "niri", "--jobs", args.jobs, env=build_env)
    install_env = build_env.copy()
    install_env.pop("DESTDIR", None)
    if destdir:
        install_env["DESTDIR"] = str(destdir)
    run("cmake", "--install", support, env=install_env)
    print(f"\nStaged in {installed}\nNo compositor was installed into or restarted in the current session.")
    if not destdir and prefix == WORK / "niri-install":
        print("Isolated nested preview (from a Wayland terminal):")
        print(shlex.join(["python3", str(SUPPORT / "run-nested.py")]))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
