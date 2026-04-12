#!/usr/bin/env python3
"""Dumpa dados do SQLite remoto (ESP32) via shell serial para um SQLite local.

Fluxo:
1) Envia comandos `database -exec_nolog <sql>` pela serial.
2) Pagina leituras para respeitar o limite de 80 linhas por consulta do firmware.
3) Grava um snapshot local em um arquivo .db no PC.

Requisito:
    pip install pyserial
"""

from __future__ import annotations

import argparse
import re
import sqlite3
import sys
import time
from pathlib import Path
from typing import Dict, List, Sequence, Set, Tuple

try:
    import serial
except ImportError as exc:
    print("[erro] pyserial nao instalado. Rode: pip install pyserial", file=sys.stderr)
    raise SystemExit(2) from exc

SAFE_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ROW_PREFIX_RE = re.compile(r"\[(\d+)\]\s+")
ANSI_RE = re.compile(r"\x1B\[[0-?]*[ -/]*[@-~]")

FALLBACK_TABLE_COLUMNS = {
    "peers": ["id", "mac", "name", "description", "created_at", "updated_at"],
    "command_log": ["id", "command", "source", "created_at"],
    "command_log_output": ["id", "log_id", "output", "created_at"],
    "espnow_incoming_log": ["id", "peer_id", "payload", "payload_type", "received_at"],
    "espnow_outgoing_log": ["id", "peer_id", "mac", "payload", "payload_type", "delivered", "sent_at"],
    "boot_events": ["id", "reason", "boot_at"],
    "kv_store": ["key", "value", "updated_at"],
}


class SerialShellClient:
    def __init__(
        self,
        port: str,
        baudrate: int,
        idle_timeout: float,
        command_timeout: float,
    ) -> None:
        self.port = port
        self.baudrate = baudrate
        self.idle_timeout = idle_timeout
        self.command_timeout = command_timeout
        self._ser: serial.Serial | None = None

    def open(self) -> None:
        self._ser = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            timeout=0.05,
            write_timeout=1.0,
        )
        # Drena lixo de boot/monitor antes de iniciar comandos.
        time.sleep(0.25)
        self._ser.reset_input_buffer()
        self._ser.reset_output_buffer()

    def close(self) -> None:
        if self._ser is not None:
            self._ser.close()
            self._ser = None

    def run_command(
        self,
        command: str,
        *,
        reset_input: bool = True,
        idle_timeout: float | None = None,
        command_timeout: float | None = None,
    ) -> Tuple[str, List[str]]:
        if self._ser is None:
            raise RuntimeError("serial nao aberta")

        if reset_input:
            self._ser.reset_input_buffer()
        payload = (command.strip() + "\n").encode("utf-8", errors="ignore")
        self._ser.write(payload)
        self._ser.flush()

        eff_idle_timeout = self.idle_timeout if idle_timeout is None else idle_timeout
        eff_command_timeout = self.command_timeout if command_timeout is None else command_timeout

        chunks: List[bytes] = []
        start = time.monotonic()
        last_data = start
        saw_data = False

        while True:
            now = time.monotonic()
            if (now - start) >= eff_command_timeout:
                break

            chunk = self._ser.read(512)
            if chunk:
                chunks.append(chunk)
                last_data = now
                saw_data = True
                continue

            if saw_data and (now - last_data) >= eff_idle_timeout:
                break

        raw = b"".join(chunks).decode("utf-8", errors="replace")
        lines = [clean_line(line) for line in raw.splitlines()]
        lines = [line for line in lines if line]
        return raw, lines

    def unlock_startup_prompts(self) -> str:
        """Tries to pass startup ENTER and optional clock prompt automatically."""
        transcript_parts: List[str] = []
        empty_reads = 0

        for _ in range(8):
            raw, _ = self.run_command(
                "",
                reset_input=False,
                idle_timeout=0.25,
                command_timeout=1.5,
            )

            if raw:
                transcript_parts.append(raw)
                empty_reads = 0
            else:
                empty_reads += 1

            transcript = "".join(transcript_parts)
            lower = transcript.lower()

            waiting_startup_enter = "[startup]" in lower and "pressione enter" in lower
            waiting_clock_enter = "[clock]" in lower and "[clock] >" in lower
            shell_ready_hint = "[shell]" in lower or "$" in transcript

            if shell_ready_hint and not waiting_startup_enter and not waiting_clock_enter:
                break

            if empty_reads >= 2 and not waiting_startup_enter and not waiting_clock_enter:
                break

        return "".join(transcript_parts)


def clean_line(line: str) -> str:
    cleaned = ANSI_RE.sub("", line)
    cleaned = cleaned.replace("\r", "")
    return cleaned.strip()


def quote_ident(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def parse_ordered_payload(payload: str, expected_columns: Sequence[str]) -> Dict[str, str] | None:
    safe_columns = [col for col in expected_columns if SAFE_NAME_RE.match(col)]
    if not safe_columns:
        return None

    pattern_parts: List[str] = []
    for i, col in enumerate(safe_columns):
        escaped = re.escape(col)
        if i == 0:
            pattern_parts.append(rf"{escaped}=(?P<{col}>.*?)")
        else:
            pattern_parts.append(rf"\s\|\s{escaped}=(?P<{col}>.*?)")

    pattern = "^" + "".join(pattern_parts) + "$"
    match = re.match(pattern, payload)
    if match is None:
        return None

    return {col: match.group(col) for col in safe_columns}


def parse_row_line(
    line: str,
    expected_keys: Set[str] | None = None,
    expected_columns: Sequence[str] | None = None,
) -> Dict[str, str] | None:
    match = ROW_PREFIX_RE.search(line)
    if not match:
        return None

    payload = line[match.end() :]

    if expected_columns is not None and len(expected_columns) > 0:
        ordered = parse_ordered_payload(payload, expected_columns)
        if ordered is not None:
            return ordered

    parts = payload.split(" | ")

    row: Dict[str, str] = {}
    current_key: str | None = None

    for part in parts:
        if "=" in part:
            key, value = part.split("=", 1)
            key = key.strip()
            if SAFE_NAME_RE.match(key) and (expected_keys is None or key in expected_keys):
                row[key] = value
                current_key = key
                continue

        if current_key is not None:
            row[current_key] = row[current_key] + " | " + part

    if not row:
        return None

    if expected_keys is not None and not set(row.keys()).issubset(expected_keys):
        return None

    return row


def parse_rows(
    lines: Sequence[str],
    expected_keys: Set[str] | None = None,
    expected_columns: Sequence[str] | None = None,
) -> List[Dict[str, str]]:
    rows: List[Dict[str, str]] = []
    for line in lines:
        row = parse_row_line(
            line,
            expected_keys=expected_keys,
            expected_columns=expected_columns,
        )
        if row is not None:
            rows.append(row)
    return rows


def exec_sql(
    client: SerialShellClient,
    sql: str,
    *,
    expected_keys: Set[str] | None = None,
    expected_columns: Sequence[str] | None = None,
) -> Tuple[str, List[str], List[Dict[str, str]]]:
    last_raw = ""
    last_lines: List[str] = []
    last_rows: List[Dict[str, str]] = []

    for attempt in range(2):
        command = f"database -exec_nolog {sql}"
        raw, lines = client.run_command(command)
        rows = parse_rows(
            lines,
            expected_keys=expected_keys,
            expected_columns=expected_columns,
        )

        last_raw = raw
        last_lines = lines
        last_rows = rows

        if rows:
            break

        if attempt == 0:
            time.sleep(0.15)

    return last_raw, last_lines, last_rows


def build_dump_select(columns: Sequence[str], table: str) -> str:
    return f"SELECT * FROM {table}"


def discover_tables(client: SerialShellClient) -> List[str]:
    _, lines, rows = exec_sql(
        client,
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;",
        expected_keys={"name"},
    )

    tables: List[str] = []
    for row in rows:
        name = row.get("name", "").strip()
        if SAFE_NAME_RE.match(name):
            tables.append(name)

    # Remover duplicatas preservando ordem.
    seen = set()
    unique_tables: List[str] = []
    for table in tables:
        if table not in seen:
            seen.add(table)
            unique_tables.append(table)

    if not unique_tables:
        details = "\n".join(lines)
        raise RuntimeError(
            "nao foi possivel descobrir tabelas. Resposta recebida:\n" + details
        )

    return unique_tables


def fetch_table_columns(client: SerialShellClient, table: str) -> List[str]:
    _, _, rows = exec_sql(
        client,
        f"PRAGMA table_info({table});",
        expected_keys={"name"},
        expected_columns=("cid", "name", "type", "notnull", "dflt_value", "pk"),
    )
    columns: List[str] = []
    for row in rows:
        name = row.get("name", "").strip()
        if SAFE_NAME_RE.match(name):
            columns.append(name)

    if not columns and table in FALLBACK_TABLE_COLUMNS:
        columns = list(FALLBACK_TABLE_COLUMNS[table])

    return columns


def fetch_table_count(client: SerialShellClient, table: str) -> int:
    _, lines, rows = exec_sql(
        client,
        f"SELECT COUNT(*) AS total FROM {table};",
        expected_keys={"total"},
        expected_columns=("total",),
    )
    for row in rows:
        raw_total = row.get("total")
        if raw_total is None:
            continue
        try:
            return int(raw_total)
        except ValueError:
            continue

    details = "\n".join(lines)
    raise RuntimeError(f"falha ao ler COUNT(*) da tabela {table}. Resposta:\n{details}")


def fetch_table_count_upto_id(client: SerialShellClient, table: str, max_id: int) -> int:
    _, lines, rows = exec_sql(
        client,
        f"SELECT COUNT(*) AS total FROM {table} WHERE id <= {max_id};",
        expected_keys={"total"},
        expected_columns=("total",),
    )
    for row in rows:
        raw_total = row.get("total")
        if raw_total is None:
            continue
        try:
            return int(raw_total)
        except ValueError:
            continue

    details = "\n".join(lines)
    raise RuntimeError(
        f"falha ao ler COUNT(*) ate id={max_id} da tabela {table}. Resposta:\n{details}"
    )


def fetch_table_max_id(client: SerialShellClient, table: str) -> int:
    _, lines, rows = exec_sql(
        client,
        f"SELECT COALESCE(MAX(id), 0) AS max_id FROM {table};",
        expected_keys={"max_id"},
        expected_columns=("max_id",),
    )
    for row in rows:
        raw_max = row.get("max_id")
        if raw_max is None:
            continue
        try:
            return int(raw_max)
        except ValueError:
            continue

    details = "\n".join(lines)
    raise RuntimeError(f"falha ao ler MAX(id) da tabela {table}. Resposta:\n{details}")


def ensure_meta_tables(conn: sqlite3.Connection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS __dump_meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS __raw_output (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            table_name TEXT NOT NULL,
            command TEXT NOT NULL,
            output TEXT NOT NULL,
            captured_at INTEGER NOT NULL
        );
        """
    )


def ensure_local_table(conn: sqlite3.Connection, table: str, columns: Sequence[str]) -> None:
    if not SAFE_NAME_RE.match(table):
        raise ValueError(f"nome de tabela invalido: {table}")

    safe_columns = [col for col in columns if SAFE_NAME_RE.match(col)]

    if not safe_columns:
        conn.execute(
            f"""
            CREATE TABLE IF NOT EXISTS {quote_ident(table)} (
                __capture_ts INTEGER NOT NULL,
                __raw TEXT
            );
            """
        )
    else:
        col_defs = ", ".join(f"{quote_ident(col)} TEXT" for col in safe_columns)
        conn.execute(
            f"""
            CREATE TABLE IF NOT EXISTS {quote_ident(table)} (
                __capture_ts INTEGER NOT NULL,
                {col_defs}
            );
            """
        )

    existing_cols = {
        row[1]
        for row in conn.execute(f"PRAGMA table_info({quote_ident(table)});").fetchall()
    }

    for col in safe_columns:
        if col not in existing_cols:
            conn.execute(
                f"ALTER TABLE {quote_ident(table)} ADD COLUMN {quote_ident(col)} TEXT;"
            )


def clear_snapshot_table(conn: sqlite3.Connection, table: str) -> None:
    conn.execute(f"DELETE FROM {quote_ident(table)};")


def insert_rows(
    conn: sqlite3.Connection,
    table: str,
    rows: Sequence[Dict[str, str]],
    capture_ts: int,
) -> int:
    inserted = 0

    for row in rows:
        safe_row = {
            key: (None if value == "NULL" else value)
            for key, value in row.items()
            if SAFE_NAME_RE.match(key)
        }
        if not safe_row:
            continue

        cols = ["__capture_ts", *safe_row.keys()]
        placeholders = ", ".join("?" for _ in cols)
        sql = (
            f"INSERT INTO {quote_ident(table)} ("
            + ", ".join(quote_ident(c) for c in cols)
            + f") VALUES ({placeholders});"
        )

        values = [capture_ts, *safe_row.values()]
        conn.execute(sql, values)
        inserted += 1

    return inserted


def save_raw_output(
    conn: sqlite3.Connection,
    table_name: str,
    command: str,
    output: str,
    capture_ts: int,
) -> None:
    conn.execute(
        """
        INSERT INTO __raw_output (table_name, command, output, captured_at)
        VALUES (?, ?, ?, ?);
        """,
        (table_name, command, output, capture_ts),
    )


def dump_table(
    client: SerialShellClient,
    conn: sqlite3.Connection,
    table: str,
    page_size: int,
    save_raw: bool,
) -> Tuple[int, int]:
    columns = fetch_table_columns(client, table)
    ensure_local_table(conn, table, columns)
    clear_snapshot_table(conn, table)
    expected_keys = set(columns) if columns else None

    captured_total = 0
    has_id_column = "id" in columns
    select_base = build_dump_select(columns, table)

    if has_id_column:
        snapshot_max_id = fetch_table_max_id(client, table)
        if snapshot_max_id <= 0:
            return 0, 0

        total = fetch_table_count_upto_id(client, table, snapshot_max_id)
        last_id = 0

        while last_id < snapshot_max_id:
            sql = (
                f"{select_base} "
                f"WHERE id > {last_id} AND id <= {snapshot_max_id} "
                f"ORDER BY id ASC LIMIT {page_size};"
            )
            command = f"database -exec_nolog {sql}"

            raw, lines, rows = exec_sql(
                client,
                sql,
                expected_keys=expected_keys,
                expected_columns=columns if columns else None,
            )
            if save_raw:
                save_raw_output(conn, table, command, raw, int(time.time()))

            if any("resultado truncado" in line.lower() for line in lines):
                raise RuntimeError(
                    f"consulta truncada em {table} (id>{last_id}). Diminua --page-size."
                )

            if not rows:
                break

            inferred_cols = list(rows[0].keys())
            if inferred_cols:
                ensure_local_table(conn, table, inferred_cols)
                if expected_keys is None:
                    expected_keys = set(inferred_cols)

            inserted = insert_rows(conn, table, rows, int(time.time()))
            captured_total += inserted

            row_ids: List[int] = []
            for row in rows:
                raw_id = row.get("id")
                if raw_id is None:
                    continue
                try:
                    row_ids.append(int(raw_id))
                except ValueError:
                    continue

            if not row_ids:
                raise RuntimeError(f"linhas sem coluna id parseavel em {table}")

            last_id = max(row_ids)

        return total, captured_total

    total = fetch_table_count(client, table)
    if total == 0:
        return 0, 0

    offset = 0
    while offset < total:
        chunk_size = min(page_size, total - offset)
        sql = f"{select_base} ORDER BY rowid LIMIT {chunk_size} OFFSET {offset};"
        command = f"database -exec_nolog {sql}"

        raw, lines, rows = exec_sql(
            client,
            sql,
            expected_keys=expected_keys,
            expected_columns=columns if columns else None,
        )
        if save_raw:
            save_raw_output(conn, table, command, raw, int(time.time()))

        if any("resultado truncado" in line.lower() for line in lines):
            raise RuntimeError(
                f"consulta truncada em {table} (offset={offset}). Diminua --page-size."
            )

        if not rows:
            raise RuntimeError(
                f"nenhuma linha parseada em {table} (offset={offset}). "
                "Verifique ruido no serial ou reduza --page-size."
            )

        inferred_cols = list(rows[0].keys())
        if inferred_cols:
            ensure_local_table(conn, table, inferred_cols)
            if expected_keys is None:
                expected_keys = set(inferred_cols)

        inserted = insert_rows(conn, table, rows, int(time.time()))
        captured_total += inserted
        offset += chunk_size

    return total, captured_total


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extrai dados do SQLite da ESP (database -exec_nolog) para um SQLite local.",
    )
    parser.add_argument("--port", default="COM5", help="Porta serial (padrao: COM5)")
    parser.add_argument("--baud", type=int, default=921600, help="Baudrate serial")
    parser.add_argument(
        "--output",
        default="dongle_dump.db",
        help="Arquivo SQLite de destino no PC",
    )
    parser.add_argument(
        "--tables",
        nargs="*",
        default=None,
        help="Lista de tabelas (se vazio, descobre automaticamente)",
    )
    parser.add_argument(
        "--page-size",
        type=int,
        default=60,
        help="Linhas por pagina (maximo efetivo 80)",
    )
    parser.add_argument(
        "--idle-timeout",
        type=float,
        default=1.50,
        help="Tempo sem bytes para encerrar leitura de um comando (s)",
    )
    parser.add_argument(
        "--command-timeout",
        type=float,
        default=12.0,
        help="Timeout maximo por comando SQL (s)",
    )
    parser.add_argument(
        "--save-raw",
        action="store_true",
        help="Salva saida serial bruta na tabela __raw_output",
    )
    parser.add_argument(
        "--include-command-output",
        action="store_true",
        help="Inclui a tabela command_log_output no dump (pode ser pesado na serial)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    page_size = max(1, min(args.page_size, 80))
    if args.page_size > 80:
        print("[aviso] --page-size acima de 80; usando 80 por limite do firmware")

    out_path = Path(args.output).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    client = SerialShellClient(
        port=args.port,
        baudrate=args.baud,
        idle_timeout=args.idle_timeout,
        command_timeout=args.command_timeout,
    )

    try:
        print(f"[info] abrindo serial {args.port} @ {args.baud}...")
        client.open()
        print("[info] destravando prompts de startup (ENTER automatico)...")
        client.unlock_startup_prompts()

        print("[info] conectando sqlite local...")
        conn = sqlite3.connect(str(out_path))
        conn.execute("PRAGMA journal_mode=WAL;")
        ensure_meta_tables(conn)

        tables = args.tables
        if tables is None or len(tables) == 0:
            print("[info] descobrindo tabelas remotas...")
            tables = discover_tables(client)

        if not args.include_command_output:
            filtered = [table for table in tables if table != "command_log_output"]
            if len(filtered) != len(tables):
                print("[aviso] command_log_output ignorada por padrao (use --include-command-output para incluir)")
            tables = filtered

        safe_tables = [table for table in tables if SAFE_NAME_RE.match(table)]
        if not safe_tables:
            raise RuntimeError("nenhuma tabela valida para dump")

        print("[info] tabelas alvo: " + ", ".join(safe_tables))

        conn.execute("DELETE FROM __dump_meta;")
        conn.execute(
            "INSERT INTO __dump_meta (key, value) VALUES (?, ?);",
            ("captured_at", str(int(time.time()))),
        )
        conn.execute(
            "INSERT INTO __dump_meta (key, value) VALUES (?, ?);",
            ("serial_port", args.port),
        )
        conn.execute(
            "INSERT INTO __dump_meta (key, value) VALUES (?, ?);",
            ("baud", str(args.baud)),
        )

        for table in safe_tables:
            print(f"[info] dump tabela: {table}")
            total, copied = dump_table(
                client=client,
                conn=conn,
                table=table,
                page_size=page_size,
                save_raw=args.save_raw,
            )
            print(f"[ok] {table}: remoto={total} local={copied}")
            conn.commit()

        conn.commit()
        conn.close()

        print(f"[ok] dump finalizado em: {out_path}")
        print("[ok] abra esse arquivo no DB Browser for SQLite")
        return 0

    except serial.SerialException as exc:
        print(f"[erro] falha serial: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:  # noqa: BLE001
        print(f"[erro] {exc}", file=sys.stderr)
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
