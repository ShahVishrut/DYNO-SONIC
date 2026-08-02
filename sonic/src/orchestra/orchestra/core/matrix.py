from __future__ import annotations

import itertools
import re
from dataclasses import dataclass
from typing import Any, Dict, Iterable, Iterator, List, Sequence

from .util import evaluate_expression

DEFAULT_MAX_COMBINATIONS = 1 << 16


class MatrixParseError(ValueError):
    """Raised when the parameter matrix string is invalid."""


@dataclass(frozen=True)
class ParameterMatrix:
    values: Dict[str, Sequence[Any]]

    def iter_param_sets(self) -> Iterator[Dict[str, Any]]:
        keys = list(self.values.keys())
        sequences = [self.values[k] for k in keys]
        for combo in itertools.product(*sequences):
            yield {k: v for k, v in zip(keys, combo) if v is not None}

    def __len__(self) -> int:
        total = 1
        for values in self.values.values():
            total *= max(1, len(values))
        return total


def parse_matrix(
    text: str, *, max_combinations: int = DEFAULT_MAX_COMBINATIONS
) -> ParameterMatrix:
    """
    Parse a semicolon-separated parameter matrix string.

    Example: "N=2**10,2**12;B=64;Z=4..32+4"
    """

    entries = _split_entries(text)
    if not entries:
        raise MatrixParseError("empty matrix")

    values: Dict[str, List[Any]] = {}
    for entry in entries:
        name, raw_values = _split_entry(entry)
        parsed = []
        for token in _split_values(raw_values):
            parsed.extend(_parse_token(token))
        if not parsed:
            raise MatrixParseError(f"param '{name}' has no values")
        values[name] = [_coerce_value(v) for v in parsed]

    matrix = ParameterMatrix(values)
    if len(matrix) > max_combinations:
        raise MatrixParseError(
            f"matrix too large: {len(matrix)} combos > {max_combinations}"
        )
    return matrix


def dedupe_param_sets(param_sets: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    seen = set()
    deduped: List[Dict[str, Any]] = []
    for params in param_sets:
        key = tuple(sorted(params.items()))
        if key not in seen:
            seen.add(key)
            deduped.append(params)
    return deduped


def _split_entries(text: str) -> List[str]:
    return [part.strip() for part in text.split(";") if part.strip()]


def _split_entry(entry: str) -> tuple[str, str]:
    if "=" not in entry:
        raise MatrixParseError(f"missing '=' in '{entry}'")
    name, raw_values = entry.split("=", 1)
    name = name.strip()
    if not name:
        raise MatrixParseError(f"empty key in '{entry}'")
    return name, raw_values


def _split_values(raw_values: str) -> List[str]:
    return [token.strip() for token in raw_values.split(",") if token.strip()]


def _parse_token(token: str) -> List[Any]:
    if token == "$":
        return [None]
    range_match = re.fullmatch(
        r"""
        (?P<start>.+?)
        \.\.
        (?P<end>.+?)
        (?:\s*(?P<op>[+*])\s*(?P<step>.+))?
        """,
        token,
        re.VERBOSE,
    )
    if range_match:
        return _parse_range(range_match)

    try:
        return [evaluate_expression(token)]
    except Exception:
        return [token]


def _parse_range(match: re.Match[str]) -> List[Any]:
    start = evaluate_expression(match.group("start"))
    end = evaluate_expression(match.group("end"))
    op = match.group("op")
    step_expr = match.group("step")

    if not isinstance(start, (int, float)) or not isinstance(end, (int, float)):
        raise MatrixParseError("range bounds must be numeric")

    if op is None:
        if end < start:
            raise MatrixParseError("range end must be >= start")
        values = []
        current = float(start)
        while current <= float(end) + 1e-9:
            values.append(_coerce_number(current))
            current += 1.0
        return values

    step = evaluate_expression(step_expr) if step_expr else None
    if not isinstance(step, (int, float)):
        raise MatrixParseError("range step must be numeric")

    values: List[Any] = []
    current = float(start)
    end = float(end)
    if op == "+":
        if step <= 0:
            raise MatrixParseError("additive step must be > 0")
        while current <= end + 1e-9:
            values.append(_coerce_number(current))
            current += step
    elif op == "*":
        if step <= 1:
            raise MatrixParseError("multiplicative step must be > 1")
        while current <= end * (1 + 1e-9):
            values.append(_coerce_number(current))
            current *= step
    else:  # pragma: no cover
        raise MatrixParseError(f"unsupported operator '{op}' in range")

    return values


def _coerce_number(value: float) -> Any:
    if value.is_integer():
        return int(round(value))
    return value


def _coerce_value(value: Any) -> Any:
    if isinstance(value, float) and value.is_integer():
        return int(value)
    return value
