from __future__ import annotations

from typing import Iterable, List, Sequence, Tuple

from .config import ExperimentRecord, MetricGoal, MetricSpec


def parse_metric_specs(text: str) -> List[MetricSpec]:
    """
    Parse metric specifications from a semicolon-separated string.

    Format per metric: "name:goal[:weight]" where goal can include an optional bound,
    e.g. "throughput:max", "latency:min~0.5", "latency:min~0.5:2".
    """

    specs: List[MetricSpec] = []
    for chunk in text.split(";"):
        chunk = chunk.strip()
        if not chunk:
            continue
        specs.append(_parse_metric_chunk(chunk))
    if not specs:
        raise ValueError("no metrics")
    return specs


def _parse_metric_chunk(chunk: str) -> MetricSpec:
    parts = chunk.split(":")
    if len(parts) not in (2, 3):
        raise ValueError(f"bad metric spec '{chunk}'")

    name = parts[0].strip()
    if not name:
        raise ValueError(f"missing metric name in '{chunk}'")

    goal_part = parts[1].strip()
    weight = float(parts[2]) if len(parts) == 3 else 1.0

    limit = None
    limit_goal = None
    if "~" in goal_part:
        goal_str, limit_str = goal_part.split("~", 1)
        limit = float(limit_str)
    else:
        goal_str = goal_part

    try:
        goal = MetricGoal(goal_str)
    except ValueError as exc:
        raise ValueError(f"bad metric goal '{goal_str}' in '{chunk}'") from exc

    if limit is not None:
        limit_goal = MetricGoal.MAX if goal is MetricGoal.MAX else MetricGoal.MIN

    return MetricSpec(
        name=name, goal=goal, weight=weight, limit=limit, limit_goal=limit_goal
    )


def score_record(record: ExperimentRecord, metrics: Sequence[MetricSpec]) -> float:
    score = 0.0
    penalty = 0.0
    for spec in metrics:
        value = record.metrics.get(spec.name)
        if value is None:
            continue
        if spec.goal is MetricGoal.MAX:
            score += value * spec.weight
        else:
            score += (1.0 / value) * spec.weight if value != 0 else 0.0

        if spec.limit is not None:
            if spec.limit_goal is MetricGoal.MAX and value < spec.limit:
                penalty += 1000.0 * spec.weight
            elif spec.limit_goal is MetricGoal.MIN and value > spec.limit:
                penalty += 1000.0 * spec.weight

    return score - penalty


def top_records(
    records: Iterable[ExperimentRecord], metrics: Sequence[MetricSpec], top_n: int
) -> List[Tuple[ExperimentRecord, float]]:
    scored = [(record, score_record(record, metrics)) for record in records]
    scored.sort(key=lambda item: item[1], reverse=True)
    return scored[:top_n]
