#!/usr/bin/env python3
"""Guards the pt-br/English text split documented in CONTRIBUTING.md #1:
printLine/failWithCode/warnWithCode messages are user-facing and must stay
in ASCII-only Portuguese (no accents, matches LCD/serial font limits);
create_module/shell->add descriptions are TinyShell's own help surface and
stay in English. Catches strings left untranslated (or translated into the
wrong slot) by whoever adds the next command.

Runs standalone (`python scripts/check_user_text.py`) or as a PlatformIO
extra_scripts pre: hook for env:native (see platformio.ini), so it fires on
every `platformio test -e native` without needing ESP32 hardware. SCons execs
extra_scripts rather than importing them, so the check below runs
unconditionally at module scope instead of behind an `if __name__ ==
"__main__"` guard -- that guard would never fire under SCons and the check
would silently never run.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ["lib", "src"]

PORTUGUESE_CALLS = ("printLine", "failWithCode", "warnWithCode")
ENGLISH_CALLS = ("create_module", "->add")

# Words that only show up in English prose -- their pt-br spelling in this
# codebase's own vocabulary differs enough (erro vs error, aviso vs warning,
# diretorio vs directory, ...) that a hit here means the string was never
# translated, not a coincidental cognate.
ENGLISH_MARKERS = {
    "the", "this", "that", "with", "from", "please", "have", "has", "had",
    "will", "would", "should", "could", "and", "not", "you", "your",
    "when", "what", "which", "such", "cant", "wont", "isnt", "arent",
    "wasnt", "dont", "doesnt", "success", "successful", "successfully",
    "failed", "failure", "failing", "error", "warning", "invalid",
    "unable", "missing", "required", "requires", "must", "ready",
    "initialize", "initialized", "initializing", "removed", "removing",
    "written", "writing", "reading", "directory", "command", "commands",
    "module", "modules", "current", "enter", "exit", "running",
    "execute", "executing", "executed",
    "network", "device", "devices", "address", "messages", "request",
    "response", "connection", "connected", "disconnect", "disconnected",
    "files", "found", "value", "values",
}
# Bare BR-PT tech loanwords ("timeout", "remove" as a verb conjugation of
# "remover", ...) and English words used as CLI metavariable placeholders
# (<module>, <command>, ...) deliberately stay out of ENGLISH_MARKERS.

# Words that only show up in ASCII pt-br prose -- a hit inside a
# create_module/add() description (which stays English) means the string
# was translated into the wrong slot.
PORTUGUESE_MARKERS = {
    "nao", "comando", "erro", "aviso", "falha", "indisponivel",
    "disponivel", "diretorio", "arquivo", "mensagem", "dispositivo",
    "conexao", "obrigatorio", "necessario", "encontrado", "executar",
    "remover", "ligado", "desligado", "cartao", "senha", "elevar",
    "elevacao", "permissao", "perifericos", "inicializado", "reiniciando",
    "atualizado", "desligar", "consulta",
}

PLACEHOLDER_RE = re.compile(r"<[^<>]*>|\[[^\[\]]*\]")
# Snake_case kept as one token (command_log, sd_wipe, ...) so a technical
# identifier isn't mistaken for the standalone English word inside it.
WORD_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*")
STRING_LITERAL_RE = re.compile(r'"((?:\\.|[^"\\])*)"')


class Violation:
    def __init__(self, path, line, call, literal, reason):
        self.path = path
        self.line = line
        self.call = call
        self.literal = literal
        self.reason = reason

    def __str__(self):
        return f"{self.path}:{self.line}: [{self.call}] {self.reason}: \"{self.literal}\""


def iter_source_files():
    for scan_dir in SCAN_DIRS:
        base = REPO_ROOT / scan_dir
        if base.is_dir():
            yield from sorted(base.rglob("*.cpp"))


def find_calls(text, names):
    """Yields (call_name, args_text, start_index) for every `name(` in text,
    with args_text spanning the balanced parens (so multi-line/nested calls
    like failWithCode(Code::X, "a" + b) are captured whole)."""
    for match in re.finditer(r"(?<![A-Za-z0-9_])(" + "|".join(re.escape(n) for n in names) + r")\s*\(", text):
        name = match.group(1)
        depth = 1
        i = match.end()
        in_string = False
        escape = False
        start = i
        while i < len(text) and depth > 0:
            ch = text[i]
            if in_string:
                if escape:
                    escape = False
                elif ch == "\\":
                    escape = True
                elif ch == '"':
                    in_string = False
            elif ch == '"':
                in_string = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            i += 1
        yield name, text[start:i - 1], match.start()


def line_of(text, index):
    return text.count("\n", 0, index) + 1


def literal_words(literal):
    # <module>, <path>, [args], ... are CLI metavariable placeholders, not
    # prose -- their (often English) names don't drive the language check.
    stripped = PLACEHOLDER_RE.sub(" ", literal)
    for word_match in WORD_RE.finditer(stripped):
        word = word_match.group(0)
        if "_" in word:
            continue  # snake_case identifier/table/field reference, not a prose word
        if word.isupper() and len(word) > 1:
            continue  # acronym (SD, LCD, RTC, BTP, MAC, ...)
        yield word.lower()


def check_file(path, text):
    violations = []
    rel = path.relative_to(REPO_ROOT).as_posix()

    for call, args, call_index in find_calls(text, PORTUGUESE_CALLS):
        call_line = line_of(text, call_index)
        for literal in STRING_LITERAL_RE.findall(args):
            if any(ord(ch) > 127 for ch in literal):
                violations.append(Violation(
                    rel, call_line, call, literal,
                    "accented/non-ASCII character (project keeps pt-br text ASCII-only)",
                ))
            hits = {w for w in literal_words(literal) if w in ENGLISH_MARKERS}
            if hits:
                violations.append(Violation(
                    rel, call_line, call, literal,
                    f"looks untranslated (English word(s): {', '.join(sorted(hits))})",
                ))

    for call, args, call_index in find_calls(text, ENGLISH_CALLS):
        call_line = line_of(text, call_index)
        for literal in STRING_LITERAL_RE.findall(args):
            hits = {w for w in literal_words(literal) if w in PORTUGUESE_MARKERS}
            if hits:
                violations.append(Violation(
                    rel, call_line, call, literal,
                    f"looks translated into the wrong slot (pt-br word(s): {', '.join(sorted(hits))}; "
                    "create_module/add() help text stays in English)",
                ))

    return violations


def main():
    all_violations = []
    for path in iter_source_files():
        text = path.read_text(encoding="utf-8")
        all_violations.extend(check_file(path, text))

    if not all_violations:
        print("check_user_text: OK (no untranslated/mis-slotted user-facing strings found)")
        return 0

    print("check_user_text: found strings that break the pt-br/English text convention "
          "(CONTRIBUTING.md #1):\n")
    for violation in all_violations:
        print(f"  {violation}")
    print(f"\n{len(all_violations)} violation(s).")
    return 1


sys.exit(main())
