from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Sequence


class RunStatus(str, Enum):
    SUCCESS = "success"
    DRY_RUN = "dry-run"
    ERROR = "error"
    TIMEOUT = "timeout"


@dataclass
class ExperimentRecord:
    params: Dict[str, Any]
    status: RunStatus
    command: Sequence[str]
    metrics: Dict[str, float] = field(default_factory=dict)
    stdout: str = ""
    stderr: str = ""
    duration: Optional[float] = None


class MetricGoal(str, Enum):
    MAX = "max"
    MIN = "min"


@dataclass
class MetricSpec:
    name: str
    goal: MetricGoal
    weight: float = 1.0
    limit: Optional[float] = None
    limit_goal: Optional[MetricGoal] = None

    def __post_init__(self) -> None:
        if self.weight <= 0:
            raise ValueError("metric weight must be > 0")
        if self.limit is not None and self.limit_goal is None:
            raise ValueError("missing limit_goal")
