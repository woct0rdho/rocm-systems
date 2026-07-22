from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
import csv
import json
from pathlib import Path
import sqlite3
from typing import Any


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def read_csv_if_present(path: Path) -> list[dict[str, str]]:
    return read_csv(path) if path.is_file() else []


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


@contextmanager
def open_rocpd(path: Path, *, named_rows: bool = False) -> Iterator[sqlite3.Connection]:
    if not path.is_file():
        raise FileNotFoundError(path)
    connection = sqlite3.connect(path)
    if named_rows:
        connection.row_factory = sqlite3.Row
    try:
        yield connection
    finally:
        connection.close()
