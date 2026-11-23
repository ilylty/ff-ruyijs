"""Windows dist-only shells must resolve a usable Necko helper.

Regression for the issue where Context(shell=...) with a dist that has no
vendor/necko-derived produced status=0 empty navigations: the native helper
path was reported as usable without checking that it exists, and the Python
host did not provide a fallback when no RUYIJS_NECKO_PYTHON was set.
"""

from __future__ import annotations

import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))


def check() -> None:
    helper_source = (ROOT / "src" / "resource_network" / "necko_helper.cpp").read_text(
        encoding="utf-8"
    )
    assert 'std::getenv("RUYIJS_NECKO_PYTHON")' in helper_source, (
        "Windows NeckoPythonPath must honour RUYIJS_NECKO_PYTHON"
    )
    assert helper_source.count('std::getenv("RUYIJS_NECKO_PYTHON")') >= 2, (
        "Both POSIX and Windows branches must read RUYIJS_NECKO_PYTHON"
    )
    assert "GetFileAttributesW" in helper_source, (
        "Windows NeckoPythonPath must check that the helper file exists"
    )

    if sys.platform != "win32":
        print("skip dist-only helper runtime check (Windows only)")
        return

    from ruyijs._context import process_environment

    fake_shell = ROOT / "artifacts" / "distclean" / "js.exe"
    fake_shell.parent.mkdir(parents=True, exist_ok=True)
    fake_shell.touch(exist_ok=True)
    saved = os.environ.get("RUYIJS_NECKO_PYTHON")
    os.environ.pop("RUYIJS_NECKO_PYTHON", None)
    try:
        env = process_environment(fake_shell)
    finally:
        if saved is not None:
            os.environ["RUYIJS_NECKO_PYTHON"] = saved
    override = env.get("RUYIJS_NECKO_PYTHON")
    assert override, "dist-only shell must get a bundled helper python override"
    assert Path(override).is_file(), f"helper override is not a file: {override}"
    print(f"ok   dist-only shell resolves helper: {override}")


if __name__ == "__main__":
    check()
    print("win_necko_helper_path_check passed")
