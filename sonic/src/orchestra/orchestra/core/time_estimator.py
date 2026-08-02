from __future__ import annotations

import statistics
import time
from collections import deque
from typing import Deque, Optional


class TimeEstimator:
    def __init__(self, total_tasks: int, window: int = 20) -> None:
        self.total_tasks = total_tasks
        self.completed = 0
        self.start_time = time.time()
        self.samples: Deque[float] = deque(maxlen=window)

    def add(self, duration: Optional[float]) -> None:
        self.completed += 1
        if duration is None:
            return
        self.samples.append(duration)

    def eta(self) -> Optional[float]:
        if not self.samples or self.completed >= self.total_tasks:
            return None
        mean = statistics.mean(self.samples)
        remaining = self.total_tasks - self.completed
        return mean * remaining

    def progress_str(self) -> str:
        elapsed = time.time() - self.start_time
        eta = self.eta()
        if eta is None:
            return f"[{self.completed}/{self.total_tasks} {int(elapsed)}s]"
        return f"[{self.completed}/{self.total_tasks} {int(elapsed)}s<{int(eta)}s]"
