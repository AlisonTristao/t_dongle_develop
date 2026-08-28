#!/usr/bin/env python3
"""Guards the three byte-identical copies of include/bally_channels.h.

That file is the single table answering "whose message is this, and which key
opens it". It exists once per repository -- bally_OS, bally_dongle, TraceView
-- because it is product convention rather than protocol: BTP has no key-id
field on the wire at all (docs/encryption.md, CIPHER_ID picks the *cipher*,
never the *key*), so "this source_id uses the link key" is true only because
the three ends agree on it.

Three copies with no enforcement drift, and drifting is silent: the first
device added to the fleet after a divergence simply stops working, with
nothing to point at the cause. So each repository hashes its own copy against
a constant committed alongside it. Edit the file in one repository and the
other two fail to build until someone copies it across and updates the hash in
all three -- which is the whole mechanism. TraceView does this from
tests/test_ballychannels.cpp (it has Qt's QCryptographicHash); the two
firmwares do it here, because Unity under env:native has no hash primitive and
pulling one in just for this would be more code than the check.

Runs standalone (`python scripts/check_channel_contract.py`) or as a
PlatformIO extra_scripts pre: hook, the same way check_user_text.py already
does. SCons execs extra_scripts rather than importing them, so the check runs
unconditionally at module scope instead of behind an `if __name__ ==
"__main__"` guard -- that guard would never fire under SCons and the check
would silently never run.
"""
import hashlib
import sys
from pathlib import Path

try:
    _SCRIPT_PATH = Path(__file__).resolve()
except NameError:
    # SCons exec()s extra_scripts (see the module docstring) with a globals
    # dict that has no __file__, so the line above raises there. SCons runs
    # with the project directory as cwd, which is this script's grandparent.
    # Same workaround, and same reason, as check_user_text.py.
    _SCRIPT_PATH = (Path.cwd() / "scripts" / "check_channel_contract.py").resolve()

REPO_ROOT = _SCRIPT_PATH.parent.parent
CONTRACT = REPO_ROOT / "include" / "bally_channels.h"

# SHA-256 of bally_channels.h with line endings normalized to LF.
#
# Normalized because git on Windows checks the file out with CRLF while the
# repositories store LF, so hashing the bytes as they sit on disk would make
# this pass or fail depending on which machine ran it -- a false alarm that
# would teach people to ignore the check, which is worse than not having it.
#
# This constant is duplicated on purpose in all three repositories (here, in
# bally_OS/scripts/check_channel_contract.py, and in
# TraceView/tests/test_ballychannels.cpp's kExpectedSha256). Reading it from a
# shared location would defeat the check: the point is that three independent
# copies have to be updated together.
EXPECTED_SHA256 = "c141fbb0e14f1f0c6573d0406b736d4419d3c1879a17044f86ce83fc6a804560"


def main():
    if not CONTRACT.is_file():
        print(f"check_channel_contract: {CONTRACT} nao encontrado", file=sys.stderr)
        return 1

    normalized = CONTRACT.read_bytes().replace(b"\r\n", b"\n")
    actual = hashlib.sha256(normalized).hexdigest()

    if actual != EXPECTED_SHA256:
        print(
            "check_channel_contract: bally_channels.h nao confere com o hash "
            "commitado.\n"
            f"  esperado {EXPECTED_SHA256}\n"
            f"  obtido   {actual}\n"
            "Se a mudanca foi proposital, copie o arquivo verbatim para os "
            "outros dois repositorios e atualize o hash nos tres guardas "
            "(bally_dongle/scripts/check_channel_contract.py, "
            "bally_OS/scripts/check_channel_contract.py, "
            "TraceView/tests/test_ballychannels.cpp). Nao existe edicao "
            "parcial correta deste arquivo.",
            file=sys.stderr,
        )
        return 1

    print("check_channel_contract: OK")
    return 0


_status = main()

# Exit ONLY on failure, and never on success -- same reasoning as
# check_user_text.py: SCons exec()s an extra_script in its own process, so a
# sys.exit(0) here would end the BUILD, silently and totally, before a single
# object file is produced. Falling off the end lets SCons carry on, and still
# yields status 0 when run standalone.
if _status != 0:
    sys.exit(_status)
