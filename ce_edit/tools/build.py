"""Build a standalone ce_edit executable with PyInstaller.

Run from anywhere; the project root is derived from this file's location::

    python tools/build.py
    python tools/build.py --clean --check
    python tools/build.py --onedir

All console output is ASCII so logs stay readable on any Windows code page.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "ce_edit.spec"
SPEC_ONEDIR = ROOT / "ce_edit_onedir.spec"
REQUIRED_FILES = (
    ROOT / "run.py",
    ROOT / "ce_edit" / "__init__.py",
    ROOT / "ce_edit" / "data" / "completions.json",
)
ARTIFACT_NAME = "ce_edit.exe" if sys.platform == "win32" else "ce_edit"
MIN_PYTHON = (3, 11)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build ce_edit as a standalone executable with PyInstaller.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  python tools/build.py\n"
            "  python tools/build.py --clean --check\n"
            "  python tools/build.py --onedir\n"
            "  python tools/build.py --dry-run\n"
            "\nDefault output: dist/ce_edit.exe (Windows) or dist/ce_edit.\n"
            "With --onedir: dist/ce_edit/ce_edit.exe plus its runtime files.\n"
        ),
    )
    parser.add_argument(
        "--onedir",
        action="store_true",
        help="build a directory bundle instead of one bundled exe",
    )
    parser.add_argument(
        "--clean", action="store_true", help="remove build cache and the old artifact"
    )
    parser.add_argument(
        "--check", action="store_true", help="run ruff and pytest before building"
    )
    parser.add_argument(
        "--install-deps", action="store_true", help="install PyInstaller when missing"
    )
    parser.add_argument(
        "--dist", type=Path, default=Path("dist"), help="output directory (default: dist)"
    )
    parser.add_argument(
        "--work", type=Path, default=Path("build"), help="cache directory (default: build)"
    )
    parser.add_argument(
        "--quiet", action="store_true", help="reduce PyInstaller output"
    )
    parser.add_argument(
        "--dry-run", action="store_true", help="check and print the command without building"
    )
    return parser.parse_args(argv)


def version() -> str:
    source = (ROOT / "ce_edit" / "__init__.py").read_text(encoding="utf-8")
    match = re.search(r'__version__\s*=\s*["\']([^"\']+)', source)
    return match.group(1) if match else "unknown"


def resolve(path: Path) -> Path:
    return (path if path.is_absolute() else ROOT / path).resolve()


def fail(message: str) -> int:
    print(f"ERROR: {message}", file=sys.stderr)
    return 1


def spec_for(args: argparse.Namespace) -> Path:
    return SPEC_ONEDIR if args.onedir else SPEC


def artifact_for(dist: Path, args: argparse.Namespace) -> Path:
    if args.onedir:
        return dist / "ce_edit" / ARTIFACT_NAME
    return dist / ARTIFACT_NAME


def preflight(args: argparse.Namespace) -> str | None:
    if sys.version_info < MIN_PYTHON:
        required = f"{MIN_PYTHON[0]}.{MIN_PYTHON[1]}"
        return f"Python {required}+ is required, found {sys.version.split()[0]}"
    required = (*REQUIRED_FILES, spec_for(args))
    missing = [str(path.relative_to(ROOT)) for path in required if not path.is_file()]
    if missing:
        return "Missing required files: " + ", ".join(missing)
    return None


def run(command: list[str]) -> int:
    print(">", " ".join(command), flush=True)
    return subprocess.run(command, cwd=ROOT, check=False).returncode


def tool_available(module: str) -> bool:
    probe = subprocess.run(
        [sys.executable, "-m", module, "--version"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    return probe.returncode == 0


def run_checks() -> bool:
    for module, command in (
        ("ruff", [sys.executable, "-m", "ruff", "check", "."]),
        ("pytest", [sys.executable, "-m", "pytest", "-q"]),
    ):
        if not tool_available(module):
            print(f"Skipping {module} check (not installed)")
            continue
        if run(command) != 0:
            print(f"{module} check failed; build aborted", file=sys.stderr)
            return False
    return True


def ensure_pyinstaller(args: argparse.Namespace) -> bool:
    if importlib.util.find_spec("PyInstaller") is not None:
        return True
    if not args.install_deps:
        print("ERROR: PyInstaller is not installed. Run:", file=sys.stderr)
        print(f"  {sys.executable} -m pip install pyinstaller", file=sys.stderr)
        print("Or pass --install-deps to install it automatically.", file=sys.stderr)
        return False
    return run([sys.executable, "-m", "pip", "install", "pyinstaller>=6"]) == 0


def clean_outputs(dist: Path, work: Path, args: argparse.Namespace) -> None:
    targets = [work, artifact_for(dist, args)]
    if args.onedir:
        targets.append(dist / "ce_edit")
    seen: set[Path] = set()
    for path in targets:
        if path in seen:
            continue
        seen.add(path)
        if not path.exists():
            continue
        # Never delete anything outside the project, and never the root itself.
        if path == ROOT or ROOT not in path.parents:
            print(f"Skipping cleanup outside the project: {path}")
            continue
        try:
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
        except OSError as error:
            # A locked artifact or a guarded filesystem must not abort the
            # build: PyInstaller --noconfirm simply overwrites the old file.
            print(f"WARNING: could not remove {path}: {error}")
            continue
        print(f"Removed {path}")


def prepare_output(dist: Path, args: argparse.Namespace) -> None:
    """Remove the previous output so PyInstaller never has to delete it.

    Some guarded filesystems intercept Python-level deletes. Renaming the old
    one-file exe aside (or removing the onedir tree directly) lets PyInstaller
    always write to a fresh path.
    """
    if args.onedir:
        target = dist / "ce_edit"
        if target.is_dir():
            try:
                shutil.rmtree(target)
                print(f"Removed {target}")
            except OSError as error:
                print(f"WARNING: could not remove {target}: {error}")
        return
    artifact = artifact_for(dist, args)
    if not artifact.is_file():
        return
    backup = artifact.with_name(artifact.name + ".previous")
    try:
        os.replace(artifact, backup)
        print(f"Moved previous artifact to {backup}")
    except OSError as error:
        print(f"WARNING: could not move previous artifact: {error}")


def build_command(args: argparse.Namespace, dist: Path, work: Path) -> list[str]:
    command = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--distpath",
        str(dist),
        "--workpath",
        str(work),
    ]
    if args.clean:
        command.append("--clean")
    if args.quiet:
        command.extend(["--log-level", "WARN"])
    command.append(str(spec_for(args)))
    return command


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    mode = "onedir" if args.onedir else "onefile"
    print(
        f"ce_edit {version()} build ({mode}, Python {sys.version.split()[0]}, {sys.platform})"
    )
    problem = preflight(args)
    if problem:
        return fail(problem)

    dist = resolve(args.dist)
    work = resolve(args.work)
    command = build_command(args, dist, work)
    artifact = artifact_for(dist, args)

    if args.dry_run:
        if args.clean:
            print(f"Would remove: {work}, {artifact}")
        if args.check:
            print("Would run: ruff check ., pytest -q")
        if importlib.util.find_spec("PyInstaller") is None:
            print("WARNING: PyInstaller is not installed; install it before building.")
        print(">", " ".join(command))
        print("Dry run: no files were changed.")
        return 0

    if args.clean:
        clean_outputs(dist, work, args)
    if args.check and not run_checks():
        return 1
    if not ensure_pyinstaller(args):
        return 1
    prepare_output(dist, args)

    started = time.monotonic()
    if run(command) != 0:
        return fail("PyInstaller failed; see the log above")

    if not artifact.is_file() or artifact.stat().st_size == 0:
        return fail(f"No valid artifact produced: {artifact}")
    size_mb = artifact.stat().st_size / (1024 * 1024)
    elapsed = time.monotonic() - started
    root = artifact.parent if args.onedir else artifact
    print(f"\nBuild OK ({mode}): {root} ({size_mb:.1f} MB exe, {elapsed:.1f}s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
