"""Small synchronous helpers; platform build and publication policy stays local."""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Iterable


def remove_owned_pch_files(paths: Iterable[Path]) -> list[dict[str, str]]:
    """Remove only the caller's explicit generated paths, preserving receipt order."""
    removed = []
    for path in paths:
        if path.is_file():
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            path.unlink()
            removed.append({'path': str(path), 'sha256': digest})
    return removed


def build_failure_payload(phase: dict[str, object], *, source: dict[str, object] | None = None,
                          pch_recovery: dict[str, object] | None = None) -> dict[str, object]:
    """Construct failure data without serialization, output or exit-status policy."""
    payload: dict[str, object] = {
        "ok": False,
        "phase": phase["label"],
        "exit_status": phase["exit_status"],
        "diagnostic": phase["diagnostic"],
        "logs": phase,
    }
    if source is not None:
        payload["source"] = source
    if pch_recovery is not None:
        payload['pch_recovery'] = pch_recovery
    return payload
