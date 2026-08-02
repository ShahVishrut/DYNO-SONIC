from __future__ import annotations

import os
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import typer
from rich.console import Console
from rich.table import Table
from rich.progress import (
    BarColumn,
    Progress,
    TextColumn,
    TimeElapsedColumn,
    TimeRemainingColumn,
)

from .core.config import ExperimentRecord, MetricSpec, RunStatus
from .core.logging import build_summary_table, export_csv, format_metric
from .core.matrix import MatrixParseError, ParameterMatrix, parse_matrix
from .core.runners import RunnerError
from .core.scoring import parse_metric_specs, top_records
from .core.util import ExpressionError, evaluate_expression
from .project.sonic import (
    SonicRunner,
    apply_constraints,
    auto_params,
    prepare_param_sets,
    run_grid,
)

CONTEXT_SETTINGS = dict(help_option_names=["-h", "--help"])

app = typer.Typer(
    help="experiment harness",
    no_args_is_help=True,
    context_settings=CONTEXT_SETTINGS,
    pretty_exceptions_short=True,
    pretty_exceptions_show_locals=False,
)
console = Console()

run_app = typer.Typer(help="run experiments")
app.add_typer(run_app, name="run")


@run_app.command("grid")
def run_grid_command(
    binary: str = typer.Option(..., "--binary", "-b", help="binary path"),
    construction: str = typer.Option(
        ...,
        "--construction",
        "-c",
        help="construction",
        case_sensitive=False,
        metavar="zingoram|zingoram-disjoint|zingoram-tiered|zingoram-disjoint-tiered",
    ),
    accesses: int = typer.Option(..., "--accesses", "-a", help="accesses"),
    matrix: str = typer.Option(..., "--matrix", "-M", help="parameter matrix"),
    metrics: str = typer.Option(
        "throughput_total_ops:max", "--metrics", help="metrics"
    ),
    timeout: Optional[int] = typer.Option(None, "--timeout", help="timeout in seconds"),
    csv_path: Optional[Path] = typer.Option(None, "--csv", help="csv path"),
    top: int = typer.Option(5, "--top", help="top results"),
    dry_run: bool = typer.Option(False, "--dry-run", "-n", help="print commands"),
    show_output: bool = typer.Option(
        False, "--show-output", help="show stdout and stderr"
    ),
    thread_affinity: Optional[str] = typer.Option(
        None,
        "--thread-affinity",
        help="thread affinity",
        case_sensitive=False,
    ),
    thread_smt: Optional[str] = typer.Option(
        None,
        "--thread-smt",
        help="thread smt",
        case_sensitive=False,
    ),
    mode: str = typer.Option(
        "benchmark",
        "--mode",
        help="run mode",
        case_sensitive=False,
    ),
    oneshot: bool = typer.Option(
        False,
        "--oneshot",
        help="set mode to experiment",
    ),
) -> None:
    construction = _normalize_construction(construction)
    resolved_mode = _resolve_mode(mode, oneshot)
    resolved_affinity = _resolve_thread_affinity(thread_affinity)
    resolved_smt = _resolve_thread_smt(thread_smt)
    metric_specs = _parse_metric_option(metrics)
    matrix_def = _parse_matrix_option(matrix)
    base_params = {
        "construction": construction,
        "accesses": accesses,
        "mode": resolved_mode,
    }
    if resolved_affinity is not None:
        base_params["thread_affinity"] = resolved_affinity
    if resolved_smt is not None:
        base_params["thread_smt"] = resolved_smt
    prepared = _prepare_param_sets(construction, base_params, matrix_def)
    runner = _create_runner(binary, dry_run)

    console.print(f"[cyan]Running {len(prepared)} experiments ({construction})...[/]")
    records = _execute_grid(runner, prepared, timeout)
    _render_grid_results(records, metric_specs, top, csv_path, show_output)


@run_app.command("single")
def run_single_command(
    binary: str = typer.Option(..., "--binary", "-b", help="binary path"),
    construction: str = typer.Option(
        ..., "--construction", "-c", help="construction", case_sensitive=False
    ),
    accesses: int = typer.Option(..., "--accesses", "-a", help="accesses"),
    params: List[str] = typer.Option([], "--param", "-p", help="param override"),
    timeout: Optional[int] = typer.Option(None, "--timeout", help="timeout in seconds"),
    dry_run: bool = typer.Option(False, "--dry-run", "-n", help="print command"),
    show_output: bool = typer.Option(
        False, "--show-output", help="show stdout and stderr"
    ),
    thread_affinity: Optional[str] = typer.Option(
        None,
        "--thread-affinity",
        help="thread affinity",
        case_sensitive=False,
    ),
    thread_smt: Optional[str] = typer.Option(
        None,
        "--thread-smt",
        help="thread smt",
        case_sensitive=False,
    ),
    mode: str = typer.Option(
        "benchmark",
        "--mode",
        help="run mode",
        case_sensitive=False,
    ),
    oneshot: bool = typer.Option(
        False,
        "--oneshot",
        help="set mode to experiment",
    ),
) -> None:
    construction = _normalize_construction(construction)
    resolved_mode = _resolve_mode(mode, oneshot)
    resolved_affinity = _resolve_thread_affinity(thread_affinity)
    resolved_smt = _resolve_thread_smt(thread_smt)

    manual_params = _parse_param_overrides(params)
    if "thread_affinity" in manual_params:
        manual_params["thread_affinity"] = _resolve_thread_affinity(
            str(manual_params["thread_affinity"])
        )
    if "thread_smt" in manual_params:
        manual_params["thread_smt"] = _resolve_thread_smt(
            str(manual_params["thread_smt"])
        )
    base = {
        "construction": construction,
        "accesses": accesses,
        "mode": resolved_mode,
        **manual_params,
    }
    if resolved_affinity is not None:
        base["thread_affinity"] = resolved_affinity
    if resolved_smt is not None:
        base["thread_smt"] = resolved_smt
    try:
        enriched = {**base, **auto_params(construction, base)}
    except RunnerError as exc:
        console.print(f"[red]{exc}[/]")
        raise typer.Exit(code=1)

    prepared, rejected = apply_constraints([enriched], construction)

    if not prepared:
        console.print("[red]params failed constraints[/]")
        if rejected:
            failure_str = "; ".join(rejected[0][1])
            console.print(f"[yellow]{failure_str}[/]")
        raise typer.Exit(code=1)

    runner = _create_runner(binary, dry_run)
    record = runner.run(prepared[0], timeout=timeout)
    console.print(build_summary_table([record], list(record.metrics.keys())))
    if show_output:
        _print_run_outputs([record])
    elif record.status is RunStatus.ERROR:
        console.print("[yellow]run failed; rerun with --show-output[/]")


def _parse_param_overrides(pairs: Iterable[str]) -> Dict[str, object]:
    parsed: Dict[str, object] = {}
    for pair in pairs:
        if "=" not in pair:
            raise typer.BadParameter(f"expected key=value: '{pair}'")
        key, raw_value = pair.split("=", 1)
        key = key.strip()
        raw_value = raw_value.strip()
        if not key:
            raise typer.BadParameter(f"empty key in '{pair}'")
        try:
            parsed[key] = evaluate_expression(raw_value)
        except ExpressionError:
            parsed[key] = raw_value
    return parsed


def _normalize_construction(value: str) -> str:
    normalized = value.lower()
    allowed = {
        "zingoram",
        "zingoram-disjoint",
        "zingoram-tiered",
        "zingoram-disjoint-tiered",
    }
    if normalized not in allowed:
        raise typer.BadParameter(f"unknown construction '{value}'")
    return normalized


def _resolve_mode(raw_mode: str, oneshot: bool) -> str:
    mode = raw_mode.lower()
    if oneshot:
        if raw_mode and mode not in {"benchmark", "experiment"}:
            raise typer.BadParameter(f"unknown mode '{raw_mode}'")
        mode = "experiment"
    if mode not in {"experiment", "benchmark"}:
        raise typer.BadParameter(f"unknown mode '{raw_mode}'")
    return mode


def _resolve_thread_affinity(value: Optional[str]) -> Optional[str]:
    if value is None:
        return None
    affinity = value.lower()
    if affinity not in {"inherit", "dedicated"}:
        raise typer.BadParameter(f"unknown thread-affinity '{value}'")
    return affinity


def _resolve_thread_smt(value: Optional[str]) -> Optional[str]:
    if value is None:
        return None
    smt = value.lower()
    if smt not in {"avoid", "allow"}:
        raise typer.BadParameter(f"unknown thread-smt '{value}'")
    return smt


def _parse_metric_option(text: str) -> List[MetricSpec]:
    try:
        return parse_metric_specs(text)
    except ValueError as exc:
        raise typer.BadParameter(str(exc)) from exc


def _parse_matrix_option(text: str) -> ParameterMatrix:
    try:
        return parse_matrix(text)
    except MatrixParseError as exc:
        console.print(f"[red]matrix error: {exc}[/]")
        raise typer.Exit(code=1)


def _prepare_param_sets(
    construction: str, base_params: Dict[str, object], matrix_def: ParameterMatrix
) -> List[Dict[str, object]]:
    raw_param_sets = list(matrix_def.iter_param_sets())
    try:
        prepared, rejected = prepare_param_sets(
            construction, base_params, raw_param_sets
        )
    except RunnerError as exc:
        console.print(f"[red]{exc}[/]")
        raise typer.Exit(code=1)

    _report_rejected_sets(rejected)
    if not prepared:
        console.print("[yellow]no parameter sets left after constraints[/]")
        raise typer.Exit(code=1)
    return prepared


def _report_rejected_sets(rejected: List[Tuple[Dict[str, object], List[str]]]) -> None:
    if not rejected:
        return

    console.print(f"[yellow]filtered {len(rejected)} parameter sets[/]")
    for params, failures in rejected[:3]:
        failure_str = "; ".join(failures)
        console.print(f"  • {params} → {failure_str}")
    if len(rejected) > 3:
        console.print(f"  … {len(rejected) - 3} more filtered combinations")


def _create_runner(binary: str, dry_run: bool) -> SonicRunner:
    binary_path, tried_paths = _resolve_path(binary)
    if not binary_path.exists():
        tried_str = ", ".join(str(path) for path in tried_paths)
        raise typer.BadParameter(f"binary not found: {tried_str}")

    try:
        return SonicRunner(binary_path, dry_run=dry_run)
    except RunnerError as exc:
        console.print(f"[red]{exc}[/]")
        raise typer.Exit(code=1)


def _execute_grid(
    runner: SonicRunner,
    param_sets: List[Dict[str, object]],
    timeout: Optional[int],
) -> List[ExperimentRecord]:
    records: List[ExperimentRecord]
    with Progress(
        TextColumn("[progress.description]{task.description}"),
        BarColumn(),
        TimeElapsedColumn(),
        TimeRemainingColumn(),
        console=console,
    ) as progress:
        task = progress.add_task("running experiments", total=len(param_sets))

        def on_progress(
            idx: int,
            total: int,
            params: Dict[str, object],
            record: ExperimentRecord,
            progress_info: str,
        ) -> None:
            description = progress_info
            if record.duration is not None:
                description = f"{progress_info} last={record.duration:.2f}s"
            progress.update(task, completed=idx, description=description)

        records = run_grid(
            runner,
            param_sets,
            timeout=timeout,
            callback=on_progress,
        )
    return records


def _render_grid_results(
    records: List[ExperimentRecord],
    metric_specs: Sequence[MetricSpec],
    top: int,
    csv_path: Optional[Path],
    show_output: bool,
) -> None:
    _augment_benchmark_display_metrics(records)
    display_metrics = _select_display_metrics(records, metric_specs)
    console.print(build_summary_table(records, display_metrics))

    if show_output:
        _print_run_outputs(records)
    elif any(record.status is RunStatus.ERROR for record in records):
        console.print("[yellow]some runs failed; rerun with --show-output[/]")

    successful = [record for record in records if record.status is RunStatus.SUCCESS]
    if successful and metric_specs and top > 0:
        leaders = top_records(successful, metric_specs, min(top, len(successful)))
        console.print(_build_top_table(leaders, metric_specs, display_metrics))

    if csv_path:
        export_csv(records, csv_path)
        console.print(f"[green]wrote csv to {csv_path}[/]")


def _select_display_metrics(
    records: Sequence[ExperimentRecord], metric_specs: Sequence[MetricSpec]
) -> List[str]:
    def metric_exists(name: str) -> bool:
        return any(name in record.metrics for record in records)

    ordered: List[str] = []
    seen = set()

    for spec in metric_specs:
        if metric_exists(spec.name) and spec.name not in seen:
            ordered.append(spec.name)
            seen.add(spec.name)

    default_extras = [
        "throughput_mean_ops",
        "throughput.ci95_ops",
        "latency.mean_us",
    ]
    for candidate in default_extras:
        if candidate not in seen and metric_exists(candidate):
            ordered.append(candidate)
            seen.add(candidate)

    return ordered


def _build_top_table(leaders, metric_specs, display_metrics):
    table = Table(title="top results", show_lines=False)
    table.add_column("#", style="dim")
    table.add_column("score", justify="right")
    table.add_column("params", overflow="fold")

    spec_names = [spec.name for spec in metric_specs]
    extra_metrics = [name for name in display_metrics if name not in spec_names]

    for spec in metric_specs:
        table.add_column(spec.name, justify="right")
    for metric in extra_metrics:
        table.add_column(metric, justify="right")

    for rank, (record, score) in enumerate(leaders, start=1):
        param_str = " ".join(
            f"{k}={record.params[k]}"
            for k in sorted(record.params.keys())
            if k not in {"construction", "accesses"}
        )
        row = [str(rank), format_metric(score), param_str or "<base>"]
        for spec in metric_specs:
            value = record.metrics.get(spec.name)
            row.append(format_metric(value) if value is not None else "-")
        for metric in extra_metrics:
            value = record.metrics.get(metric)
            row.append(format_metric(value) if value is not None else "-")
        table.add_row(*row)
    return table


def _augment_benchmark_display_metrics(records: Sequence[ExperimentRecord]) -> None:
    for record in records:
        metrics = record.metrics

        lower = metrics.get("throughput.ci95_lower_ops")
        upper = metrics.get("throughput.ci95_upper_ops")
        if (
            lower is not None
            and upper is not None
            and "throughput.ci95_ops" not in metrics
        ):
            metrics["throughput.ci95_ops"] = _format_range(lower, upper)

        lower_lat = metrics.get("latency.ci95_lower_us")
        upper_lat = metrics.get("latency.ci95_upper_us")
        if (
            lower_lat is not None
            and upper_lat is not None
            and "latency.ci95_us" not in metrics
        ):
            metrics["latency.ci95_us"] = _format_range(lower_lat, upper_lat)


def _format_range(lower: float, upper: float) -> str:
    return f"[{format_metric(lower)}, {format_metric(upper)}]"


def _print_run_outputs(records: Iterable[ExperimentRecord]) -> None:
    for record in records:
        console.print(f"[dim]command: {' '.join(record.command)}[/]")
        if record.stdout:
            console.print("[cyan]stdout:[/]")
            console.print(record.stdout.rstrip())
        if record.stderr:
            console.print("[magenta]stderr:[/]")
            console.print(record.stderr.rstrip())
        if not record.stdout and not record.stderr:
            console.print("[dim]<no output captured>[/]")


def _resolve_path(raw_path: str) -> Tuple[Path, List[Path]]:
    path = Path(raw_path).expanduser()
    tried: List[Path] = []

    if path.is_absolute():
        tried.append(path)
        return path, tried

    cwd_candidate = Path.cwd() / path
    tried.append(cwd_candidate)
    return cwd_candidate, tried


@app.callback()
def main_callback(
    cwd: Optional[Path] = typer.Option(
        None,
        "--cwd",
        help="chdir before running",
        dir_okay=True,
        file_okay=False,
        exists=True,
    ),
) -> None:
    """CLI entry point."""
    if cwd is not None:
        os.chdir(cwd)


def main() -> None:
    app()


if __name__ == "__main__":
    main()
