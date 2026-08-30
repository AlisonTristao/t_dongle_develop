Import("env")

import subprocess
from pathlib import Path

# Arduino-ESP32 hardcodes esp_app_desc.version to a placeholder, so the
# firmware cannot report which commit it was built from without help.
# AppRuntime's buildDongleSourceInfo() reads DONGLE_GIT_REV (if defined) for
# the MANIFEST_DATA source_info "fw_version" entry -- this script is what
# defines it, from `git describe` of the project checkout at build time.

project_dir = Path(env.subst("$PROJECT_DIR"))


def git_rev() -> str:
    try:
        out = subprocess.check_output(
            ["git", "describe", "--always", "--dirty", "--tags"],
            cwd=str(project_dir),
            stderr=subprocess.DEVNULL,
        )
        return out.decode("ascii", "replace").strip() or "unknown"
    except Exception:
        return "unknown"


env.Append(CPPDEFINES=[("DONGLE_GIT_REV", env.StringifyMacro(git_rev()))])
