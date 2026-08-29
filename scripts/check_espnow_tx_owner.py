Import("env")

from pathlib import Path
import re


project_dir = Path(env.subst("$PROJECT_DIR"))
expected = (project_dir / "lib" / "EspNowManager" / "EspNowManager.cpp").resolve()
call_pattern = re.compile(r"\besp_now_send\s*\(")


def without_comments(text: str) -> str:
    out = []
    i = 0
    in_block = False
    while i < len(text):
        if in_block:
            end = text.find("*/", i)
            if end < 0:
                break
            i = end + 2
            in_block = False
            continue
        if text.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                break
            out.append("\n")
            i = end + 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


calls = []
for root_name in ("lib", "src"):
    root = project_dir / root_name
    for path in root.rglob("*"):
        if path.suffix.lower() not in (".c", ".cc", ".cpp", ".cxx"):
            continue
        clean = without_comments(path.read_text(encoding="utf-8", errors="replace"))
        for match in call_pattern.finditer(clean):
            line = clean.count("\n", 0, match.start()) + 1
            calls.append((path.resolve(), line))

if len(calls) != 1 or calls[0][0] != expected:
    rendered = ", ".join(f"{path}:{line}" for path, line in calls) or "nenhuma"
    raise RuntimeError(
        "ESP-NOW TX owner violated: expected exactly one esp_now_send() in "
        f"{expected}; found {rendered}"
    )

print("check_espnow_tx_owner: OK")
