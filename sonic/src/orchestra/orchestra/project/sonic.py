from __future__ import annotations

import math
import re
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Optional, Sequence, Tuple

from ..core.config import ExperimentRecord
from ..core.constraints import Constraint, filter_param_sets
from ..core.runners import BinaryRunner, RunnerError
from ..core.time_estimator import TimeEstimator

_ALLOWED_CONSTRUCTIONS = {
    "zingoram",
    "zingoram-disjoint",
    "zingoram-tiered",
    "zingoram-disjoint-tiered",
}
_ALLOWED_MODES = {"experiment", "benchmark"}
_THREAD_AFFINITIES = {"inherit", "dedicated"}
_THREAD_SMT_MODES = {"avoid", "allow"}
_OPTIONAL_FLAG_MAP = {
    "Z": "-Z",
    "S": "-S",
    "J": "-J",
    "T": "-T",
    "A": "-A",
    "eviction_rate": "-A",
    "evict_batch": "--evict-batch",
    "evict_batch_size": "--evict-batch",
    "evict_threads": "--evict-threads",
    "hot_budget": "--hot-budget",
    "hot_budget_bytes": "--hot-budget",
    "cache_budget": "--cache-budget",
    "cache_budget_bytes": "--cache-budget",
    "backend_cache_budget": "--backend-cache-budget",
    "backend_cache_budget_bytes": "--backend-cache-budget",
    "cache_pack_factor": "--cache-pack-factor",
    "cache_path": "--cache-path",
    "stash_threads": "--stash-threads",
    "routing_depth": "--routing-depth",
}


def zingoram_tree_height(block_count: int, eviction_rate: int) -> int:
    _2n_a = int(math.ceil(2.0 * block_count / eviction_rate))
    return int(math.ceil(math.log2(_2n_a)))


def binary_search_int(
    low: int, high: int, predicate: Callable[[int], bool], *, default: int
) -> int:
    result = default
    while low <= high:
        mid = (low + high) // 2
        if predicate(mid):
            result = mid
            low = mid + 1
        else:
            high = mid - 1
    return result


def zingoram_max_eviction_rate(bucket_real_size: int) -> int:
    def predicate(y: int) -> bool:
        if y <= 0:
            return False
        lhs = (
            bucket_real_size * math.log(2 * bucket_real_size / y)
            + (y / 2)
            - bucket_real_size
            - math.log(4)
        )
        return lhs >= 0 and y <= 2 * bucket_real_size

    return binary_search_int(
        bucket_real_size, 2 * bucket_real_size, predicate, default=bucket_real_size
    )


_PARAM_KEY_ALIASES: Dict[str, str] = {
    "R": "routing_depth",
    "r": "routing_depth",
    "A": "eviction_rate",
    "evict_batch_size": "evict_batch",
    "hot_budget_bytes": "hot_budget",
    "cache_budget_bytes": "cache_budget",
    "backend_cache_budget_bytes": "backend_cache_budget",
}


def _canonical_construction(name: str) -> str:
    lowered = name.lower()
    if lowered in {"zingoram-disjoint", "zingoram-disjoint-tiered"}:
        return "zingoram"
    if lowered in {"zingoram-tiered", "zingoram-disjoint-tiered"}:
        return "zingoram"
    return lowered


def _apply_param_aliases(params: Dict[str, object]) -> Dict[str, object]:
    normalized: Dict[str, object] = {}
    for key, value in params.items():
        alias = _PARAM_KEY_ALIASES.get(key, key)
        if alias in normalized and alias != key:
            # keep the first occurrence (prefer already-normalized key)
            continue
        normalized[alias] = value
    return normalized


def _missing_required(params: Dict[str, object], keys: Sequence[str]) -> List[str]:
    return [key for key in keys if key not in params]


def _normalize_mode(value: object) -> str:
    mode = str(value if value is not None else "experiment").lower()
    if mode not in _ALLOWED_MODES:
        raise RunnerError(f"unknown mode '{value}'")
    return mode


def _normalize_thread_affinity(value: object) -> Optional[str]:
    if value is None:
        return None
    affinity = str(value).lower()
    if affinity not in _THREAD_AFFINITIES:
        raise RunnerError(f"unknown thread-affinity '{value}'")
    return affinity


def _normalize_thread_smt(value: object) -> Optional[str]:
    if value is None:
        return None
    smt = str(value).lower()
    if smt not in _THREAD_SMT_MODES:
        raise RunnerError(f"unknown thread-smt '{value}'")
    return smt


def _extend_optional_args(args: List[str], params: Dict[str, object]) -> None:
    added_flags = set()
    for key, flag in _OPTIONAL_FLAG_MAP.items():
        if key in params and flag not in added_flags:
            args.extend([flag, str(params[key])])
            added_flags.add(flag)


def auto_params(construction: str, params: Dict[str, object]) -> Dict[str, object]:
    canonical = _canonical_construction(construction)
    auto: Dict[str, object] = {}

    base_eviction_rate = None
    if canonical == "zingoram":
        if "Z" not in params:
            raise RunnerError(f"missing Z for {construction}")
        bucket_real_size = int(params["Z"])
        best_eviction_rate = zingoram_max_eviction_rate(bucket_real_size)
        base_eviction_rate = best_eviction_rate
        auto["base_A"] = best_eviction_rate
    else:
        raise RunnerError(f"unknown construction '{construction}'")

    if base_eviction_rate is None:
        raise RunnerError("missing base eviction rate")
    _ = zingoram_tree_height(int(params["N"]), base_eviction_rate)

    if canonical == "zingoram":
        if "routing_depth" not in params:
            raise RunnerError("missing routing_depth")

    return auto


def constraints(construction: str) -> Sequence[Constraint]:
    canonical = _canonical_construction(construction)
    constraint_list: List[Constraint] = []

    if canonical == "zingoram":
        constraint_list.append(
            Constraint(
                lambda p: (
                    "S" in p and 0.1 * int(p["Z"]) <= float(p["S"]) <= 2 * int(p["Z"])
                ),
                "S out of range for Z",
            )
        )
    if canonical == "zingoram":
        constraint_list.append(
            Constraint(
                lambda p: (
                    "routing_depth" in p
                    and "base_A" in p
                    and int(p["routing_depth"])
                    < zingoram_tree_height(int(p["N"]), int(p["base_A"])) - 1
                ),
                "routing_depth too large",
            )
        )

    return constraint_list


def apply_constraints(
    param_sets: Iterable[Dict[str, object]],
    construction: str,
) -> Tuple[List[Dict[str, object]], List[Tuple[Dict[str, object], List[str]]]]:
    checks = constraints(construction)
    return filter_param_sets(param_sets, checks)


class SonicRunner(BinaryRunner):
    def __init__(self, binary: Path, *, dry_run: bool = False) -> None:
        super().__init__(binary, dry_run=dry_run)

    def params_to_args(self, params: Dict[str, object]) -> List[str]:
        construction_raw = str(params.get("construction", "")).lower()
        if construction_raw not in _ALLOWED_CONSTRUCTIONS:
            raise RunnerError(f"unknown construction '{construction_raw}'")

        missing = _missing_required(params, ["N", "B", "accesses"])
        if missing:
            raise RunnerError(f"missing params: {', '.join(missing)}")

        mode = _normalize_mode(params.get("mode"))
        thread_affinity = _normalize_thread_affinity(params.get("thread_affinity"))
        thread_smt = _normalize_thread_smt(params.get("thread_smt"))
        is_disjoint = construction_raw in {
            "zingoram-disjoint",
            "zingoram-disjoint-tiered",
        }
        is_tiered = construction_raw in {"zingoram-tiered", "zingoram-disjoint-tiered"}

        tiered_param_keys = {
            "hot_budget",
            "hot_budget_bytes",
            "cache_budget",
            "cache_budget_bytes",
            "backend_cache_budget",
            "backend_cache_budget_bytes",
            "cache_pack_factor",
            "cache_path",
        }
        tiered_supplied = [key for key in tiered_param_keys if key in params]
        if is_tiered:
            if "cache_budget" not in params and "cache_budget_bytes" not in params:
                raise RunnerError("missing cache_budget")
        elif tiered_supplied:
            raise RunnerError("tiered params require a tiered construction")

        args: List[str] = ["-v"]
        if thread_affinity is not None:
            args.extend(["--thread-affinity", thread_affinity])
        if thread_smt is not None:
            args.extend(["--thread-smt", thread_smt])
        args.extend([construction_raw, mode])
        args.extend(["-N", str(int(params["N"]))])
        args.extend(["-B", str(int(params["B"]))])
        _extend_optional_args(args, params)

        disjoint_epoch = params.get("disjoint_epoch")
        online_only = bool(params.get("online_only"))

        if disjoint_epoch is not None:
            if not is_disjoint:
                raise RunnerError("disjoint_epoch requires a disjoint construction")
            args.extend(["--disjoint-epoch", str(disjoint_epoch)])

        if online_only:
            if not is_disjoint:
                raise RunnerError("online_only requires a disjoint construction")
            args.append("--online-only")

        if is_disjoint:
            has_window = disjoint_epoch is not None
            if online_only and has_window:
                raise RunnerError("choose online_only or disjoint_epoch")
            if not online_only and not has_window:
                raise RunnerError("set online_only or disjoint_epoch")

        args.extend(["--accesses", str(int(params["accesses"]))])
        return args

    def parse_output(self, params: Dict[str, object], stdout: str) -> Dict[str, float]:
        metrics: Dict[str, float] = {}
        mode = _normalize_mode(params.get("mode"))

        if mode == "benchmark":
            metrics.update(_parse_benchmark_metrics(stdout))

        summary = _extract_block("SUMMARY", stdout)
        metrics.update(_extract_metrics(summary))

        perf = _extract_block("PERFORMANCE TIMERS", stdout)
        metrics.update(_extract_perf_metrics(perf))

        legacy_metrics = _parse_demo_metrics(stdout)
        for key, value in legacy_metrics.items():
            metrics.setdefault(key, value)

        return metrics


def _extract_block(title: str, text: str) -> str:
    pattern = rf"{re.escape(title)}(.*?)(?:\n\s*\n|\Z)"
    match = re.search(pattern, text, re.DOTALL)
    return match.group(1) if match else ""


def _extract_metrics(block: str) -> Dict[str, float]:
    metrics: Dict[str, float] = {}
    pattern = re.compile(r"\[.*?\]\s+\[inf\]\s+([\w:/-]+)\s*:\s*([0-9.eE+-]+)")
    for match in pattern.finditer(block):
        metrics[match.group(1)] = float(match.group(2))
    return metrics


def _extract_perf_metrics(block: str) -> Dict[str, float]:
    metrics: Dict[str, float] = {}
    pattern = re.compile(r"\[.*?\]\s+\[vrb\]\s+([\w_/:-]+)\s*:\s*total=([0-9.eE+-]+)s")
    for match in pattern.finditer(block):
        metrics[match.group(1)] = float(match.group(2))
    return metrics


def _parse_benchmark_metrics(text: str) -> Dict[str, float]:
    metrics: Dict[str, float] = {}
    for section, body in _iter_benchmark_sections(text):
        section_metrics = _parse_benchmark_block(section, body)
        metrics.update(section_metrics)

        if section == "throughput":
            total_key = f"{section}.total_ops"
            if total_key in section_metrics:
                metrics.setdefault("throughput_total_ops", section_metrics[total_key])
            mean_key = f"{section}.mean_ops"
            if mean_key in section_metrics:
                metrics.setdefault("throughput_mean_ops", section_metrics[mean_key])
    return metrics


def _iter_benchmark_sections(text: str) -> Iterable[Tuple[str, str]]:
    pattern = re.compile(
        r"^\[bench\]\s+\[[^\]]+\]\s+([A-Za-z0-9_-]+)\s*:\s*(.+)$", re.MULTILINE
    )
    matches = list(pattern.finditer(text))
    for idx, match in enumerate(matches):
        section = _normalize_label(match.group(1))
        start = match.end()
        end = matches[idx + 1].start() if idx + 1 < len(matches) else len(text)
        body = text[start:end]
        yield section, body


def _parse_benchmark_block(section: str, body: str) -> Dict[str, float]:
    metrics: Dict[str, float] = {}
    for raw_line in body.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("["):
            continue

        match = re.match(r"^(.+?)\s{2,}(.*)$", line)
        if not match:
            continue

        label_raw = match.group(1).strip()
        value_raw = match.group(2).strip()
        metrics.update(_convert_benchmark_entry(section, label_raw, value_raw))
    return metrics


def _convert_benchmark_entry(
    section: str, label_raw: str, value_raw: str
) -> Dict[str, float]:
    metrics: Dict[str, float] = {}
    label_norm = _normalize_label(label_raw)
    base_key = f"{section}.{label_norm}"

    if label_norm in {"min_max", "q1_q3"} and "/" in value_raw:
        label_parts = [_normalize_label(part) for part in label_raw.split("/") if part]
        delimiter = " / " if " / " in value_raw else "/"
        value_parts = [
            part.strip() for part in value_raw.split(delimiter) if part.strip()
        ]
        if len(label_parts) == len(value_parts):
            for part_label, part_value in zip(label_parts, value_parts):
                for suffix, converted in _quantity_to_metric_map(part_value).items():
                    key = f"{section}.{part_label}"
                    if suffix:
                        key = f"{key}_{suffix}"
                    metrics[key] = converted
        return metrics

    if label_norm == "95_pct_ci":
        if value_raw.startswith("[") and value_raw.endswith("]"):
            inner = value_raw[1:-1]
            parts = [part.strip() for part in inner.split(",") if part.strip()]
        elif " - " in value_raw:
            parts = [part.strip() for part in value_raw.split(" - ") if part.strip()]
            if len(parts) == 2:
                candidate_unit = None
                for token in ("op/s", "B/s", "s", "ms", "us", "ns"):
                    if token in parts[1]:
                        candidate_unit = token
                        break
                if candidate_unit and candidate_unit not in parts[0]:
                    parts[0] = f"{parts[0]} {candidate_unit}"
        else:
            parts = []

        if len(parts) == 2:
            for suffix, raw in zip(("lower", "upper"), parts):
                for unit_suffix, converted in _quantity_to_metric_map(raw).items():
                    key = f"{section}.ci95_{suffix}"
                    if unit_suffix:
                        key = f"{key}_{unit_suffix}"
                    metrics[key] = converted
        return metrics

    if label_norm == "environment":
        entries = [segment.strip() for segment in value_raw.split(",")]
        for entry in entries:
            if "=" not in entry:
                continue
            sub_label, raw_value = entry.split("=", 1)
            for suffix, converted in _quantity_to_metric_map(raw_value.strip()).items():
                key = f"{section}.{_normalize_label(sub_label)}"
                if suffix:
                    key = f"{key}_{suffix}"
                metrics[key] = converted
        return metrics

    if label_norm == "samples":
        match = re.match(r"(\d+)\s+recorded,\s+(\d+)\s+rejected", value_raw)
        if match:
            metrics[f"{section}.samples_recorded"] = float(match.group(1))
            metrics[f"{section}.samples_rejected"] = float(match.group(2))
        else:
            for suffix, converted in _quantity_to_metric_map(value_raw).items():
                key = base_key
                if suffix:
                    key = f"{key}_{suffix}"
                metrics[key] = converted
        return metrics

    for suffix, converted in _quantity_to_metric_map(value_raw).items():
        key = base_key
        if suffix:
            key = f"{key}_{suffix}"
        metrics[key] = converted

    return metrics


def _normalize_label(label: str) -> str:
    normalized = label.strip().lower()
    normalized = normalized.replace("%", "_pct")
    normalized = normalized.replace("/", "_")
    normalized = normalized.replace("-", "_")
    normalized = normalized.replace(" ", "_")
    while "__" in normalized:
        normalized = normalized.replace("__", "_")
    normalized = re.sub(r"[^a-z0-9_]", "", normalized)
    normalized = normalized.strip("_")
    return normalized


def _quantity_to_metric_map(value_raw: str) -> Dict[Optional[str], float]:
    cleaned = value_raw.strip()
    percent_match = re.match(r"^([+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)%$", cleaned)
    if percent_match:
        return {"pct": float(percent_match.group(1))}

    unit_match = re.match(
        r"^([+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\s*(ns|us|ms|s|op/s|B/s)$",
        cleaned,
    )
    if unit_match:
        value = float(unit_match.group(1))
        unit = unit_match.group(2)
        if unit == "s":
            return {"seconds": value}
        if unit == "ms":
            return {"seconds": value / 1_000.0, "ms": value}
        if unit == "us":
            return {"seconds": value / 1_000_000.0, "us": value}
        if unit == "ns":
            return {"seconds": value / 1_000_000_000.0, "ns": value}
        if unit == "op/s":
            return {"ops": value}
        if unit == "B/s":
            return {}

    try:
        return {None: float(cleaned)}
    except ValueError:
        return {}


def prepare_param_sets(
    construction: str,
    base_params: Dict[str, object],
    param_sets: Iterable[Dict[str, object]],
) -> Tuple[List[Dict[str, object]], List[Tuple[Dict[str, object], List[str]]]]:
    base_normalized = _apply_param_aliases(base_params)
    prepared: List[Dict[str, object]] = []
    for param in param_sets:
        normalized_param = _apply_param_aliases(param)
        merged = {**base_normalized, **normalized_param}
        merged.update(auto_params(construction, merged))
        prepared.append(merged)
    return apply_constraints(prepared, construction)


def run_grid(
    runner: SonicRunner,
    param_sets: List[Dict[str, object]],
    *,
    timeout: int | None,
    callback: (
        Callable[[int, int, Dict[str, object], ExperimentRecord, str], None] | None
    ) = None,
) -> List[ExperimentRecord]:
    records: List[ExperimentRecord] = []
    total = len(param_sets)
    estimator = TimeEstimator(total)

    for idx, param in enumerate(param_sets, start=1):
        record = runner.run(param, timeout=timeout)
        records.append(record)
        estimator.add(record.duration)
        progress_info = estimator.progress_str()
        if callback:
            callback(idx, total, param, record, progress_info)
    return records


def _parse_demo_metrics(text: str) -> Dict[str, float]:
    metrics: Dict[str, float] = {}
    pattern = re.compile(r"\[.*?\]\s+\[inf\]\s+([\w_]+)\s*:\s*(.+)")
    for line in text.splitlines():
        match = pattern.search(line.strip())
        if not match:
            continue
        key = match.group(1)
        raw_value = match.group(2).strip()
        value = _normalize_metric_value(raw_value)
        if isinstance(value, (int, float)):
            metrics[key] = value
    return metrics


def _normalize_metric_value(raw: str) -> float | int | str:
    cleaned = raw.strip()
    quantity = _quantity_to_metric_map(cleaned)
    preferred_order = (
        "seconds",
        "ops",
        "ms",
        "us",
        "ns",
        "pct",
        None,
    )
    for key in preferred_order:
        if key in quantity:
            value = quantity[key]
            if isinstance(value, float) and value.is_integer():
                return int(value)
            return value
    try:
        value = float(cleaned)
        return int(value) if value.is_integer() else value
    except ValueError:
        return raw.strip()
