from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Sequence, Tuple

from .matrix import dedupe_param_sets


@dataclass(frozen=True)
class Constraint:
    predicate: Callable[[Dict[str, object]], bool]
    message: str


def filter_param_sets(
    param_sets: Iterable[Dict[str, object]],
    checks: Sequence[Constraint],
) -> Tuple[List[Dict[str, object]], List[Tuple[Dict[str, object], List[str]]]]:
    valid: List[Dict[str, object]] = []
    rejected: List[Tuple[Dict[str, object], List[str]]] = []

    for params in param_sets:
        failures: List[str] = []
        try:
            for constraint in checks:
                if not constraint.predicate(params):
                    failures.append(constraint.message)
        except (KeyError, TypeError, ValueError) as exc:
            failures.append(f"constraint error: {exc}")

        if failures:
            rejected.append((params, failures))
        else:
            valid.append(params)

    return dedupe_param_sets(valid), rejected
