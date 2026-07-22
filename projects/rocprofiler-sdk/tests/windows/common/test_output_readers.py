from __future__ import annotations

import json
from pathlib import Path
import sqlite3

from output_readers import open_rocpd, read_csv, read_csv_if_present, read_json


def test_csv_and_json_readers(tmp_path: Path):
    csv_path = tmp_path / "records.csv"
    csv_path.write_text('"Name","Value"\n"kernel,one",7\n', encoding="utf-8")
    assert read_csv(csv_path) == [{"Name": "kernel,one", "Value": "7"}]
    assert read_csv_if_present(csv_path) == [{"Name": "kernel,one", "Value": "7"}]
    assert read_csv_if_present(tmp_path / "missing.csv") == []

    json_path = tmp_path / "records.json"
    expected = {"records": [{"dispatch_id": 7}]}
    json_path.write_text(json.dumps(expected), encoding="utf-8")
    assert read_json(json_path) == expected


def test_rocpd_reader_closes_named_row_connection(tmp_path: Path):
    database = tmp_path / "records.db"
    connection = sqlite3.connect(database)
    try:
        connection.execute("CREATE TABLE records (dispatch_id INTEGER)")
        connection.execute("INSERT INTO records VALUES (7)")
        connection.commit()
    finally:
        connection.close()

    with open_rocpd(database, named_rows=True) as connection:
        row = connection.execute("SELECT dispatch_id FROM records").fetchone()
        assert row["dispatch_id"] == 7

    database.unlink()
    assert not database.exists()
