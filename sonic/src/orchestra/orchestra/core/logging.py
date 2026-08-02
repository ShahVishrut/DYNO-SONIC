from __future__ import annotations

import csv
from pathlib import Path
from typing import Any, Iterable, List, Sequence

from rich.table import Table

from .config import ExperimentRecord, RunStatus
from .util import ensure_parent_dir


def export_csv(
    records: Iterable[ExperimentRecord],
    path: Path,
    *,
    extra_fields: Sequence[str] | None = None,
) -> None:
    rows: List[dict] = []
    fieldnames: List[str] = []
    seen_fields = set()
    for record in records:
        merged = {f"param_{k}": v for k, v in record.params.items()}
        merged.update({f"metric_{k}": v for k, v in record.metrics.items()})
        merged["status"] = record.status.value
        merged["duration"] = record.duration
        merged["command"] = " ".join(record.command)
        if extra_fields:
            for field in extra_fields:
                merged[field] = getattr(record, field, "")

        for key in merged.keys():
            if key not in seen_fields:
                seen_fields.add(key)
                fieldnames.append(key)
        rows.append(merged)

    if not rows:
        return

    ensure_parent_dir(path)
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def build_summary_table(
    records: Sequence[ExperimentRecord], metric_names: Sequence[str]
) -> Table:
    table = Table(title="results", show_lines=False)
    table.add_column("#", style="dim")
    table.add_column("status")
    table.add_column("params", overflow="fold")
    for metric in metric_names:
        table.add_column(metric, justify="right")

    for idx, record in enumerate(records, start=1):
        status = record.status.value
        params_repr = " ".join(
            f"{k}={record.params[k]}"
            for k in sorted(record.params.keys())
            if k not in {"construction", "accesses"}
        )
        row = [str(idx), status, params_repr or "<base>"]
        for metric in metric_names:
            value = record.metrics.get(metric)
            row.append(format_metric(value) if value is not None else "-")

        style = None
        if record.status is RunStatus.SUCCESS:
            style = "green"
        elif record.status is RunStatus.TIMEOUT:
            style = "yellow"
        elif record.status is RunStatus.ERROR:
            style = "red"
        elif record.status is RunStatus.DRY_RUN:
            style = "cyan"

        table.add_row(*row, style=style)

    return table


def format_metric(value: Any) -> str:
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        text = f"{value:.6f}"
        text = text.rstrip("0").rstrip(".")
        return text if text else "0"
    return str(value)
