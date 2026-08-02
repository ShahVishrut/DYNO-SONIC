from __future__ import annotations

import os
import subprocess
import time
from pathlib import Path
from typing import Dict, List, Sequence

from .config import ExperimentRecord, RunStatus


class RunnerError(RuntimeError):
    """Raised when a runner cannot execute."""


class Runner:
    def run(
        self, params: Dict[str, object], timeout: int | None = None
    ) -> ExperimentRecord:
        raise NotImplementedError


class BinaryRunner(Runner):
    def __init__(self, binary: Path, *, dry_run: bool = False) -> None:
        self.binary = Path(binary)
        self.dry_run = dry_run
        if not self.binary.exists():
            raise RunnerError(f"binary not found: {self.binary}")

    def params_to_args(self, params: Dict[str, object]) -> Sequence[str]:
        raise NotImplementedError

    def parse_output(self, params: Dict[str, object], stdout: str) -> Dict[str, float]:
        raise NotImplementedError

    def run(
        self, params: Dict[str, object], timeout: int | None = None
    ) -> ExperimentRecord:
        args = list(self.params_to_args(params))
        command = [str(self.binary), *map(str, args)]

        if self.dry_run:
            return ExperimentRecord(
                params=params,
                status=RunStatus.DRY_RUN,
                command=command,
            )

        env = os.environ.copy()
        env.setdefault("TERM", "dumb")

        start = time.time()
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout,
                check=True,
            )
        except subprocess.TimeoutExpired as exc:
            elapsed = time.time() - start
            return ExperimentRecord(
                params=params,
                status=RunStatus.TIMEOUT,
                command=command,
                stdout=exc.stdout or "",
                stderr=exc.stderr or "",
                duration=elapsed,
            )
        except subprocess.CalledProcessError as exc:
            elapsed = time.time() - start
            return ExperimentRecord(
                params=params,
                status=RunStatus.ERROR,
                command=command,
                stdout=exc.stdout or "",
                stderr=exc.stderr or "",
                duration=elapsed,
            )

        duration = time.time() - start
        metrics = self.parse_output(params, completed.stdout)
        return ExperimentRecord(
            params=params,
            status=RunStatus.SUCCESS,
            command=command,
            metrics=metrics,
            stdout=completed.stdout,
            stderr=completed.stderr,
            duration=duration,
        )
