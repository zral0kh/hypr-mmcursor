"""Thin subprocess wrapper around `hyprctl -j mmcursor` and its subcommands.

Every call shells out fresh — no cached connection, no socket held open — so a
compositor restart or plugin reload never leaves this process talking to a
dead handle. hyprctl itself is the IPC layer; duplicating it here would be the
"second source of truth" this whole tool exists to avoid.
"""

from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass


class HyprctlError(RuntimeError):
    pass


def _run(*args: str) -> str:
    try:
        proc = subprocess.run(
            ["hyprctl", *args],
            capture_output=True,
            text=True,
            timeout=2.0,
        )
    except FileNotFoundError as exc:
        raise HyprctlError("hyprctl not found — is Hyprland running?") from exc
    except subprocess.TimeoutExpired as exc:
        raise HyprctlError("hyprctl timed out") from exc
    if proc.returncode != 0:
        raise HyprctlError(proc.stderr.strip() or f"hyprctl exited {proc.returncode}")
    return proc.stdout


@dataclass(frozen=True)
class MonitorState:
    name: str
    mm_x: float
    mm_y: float
    mm_w: float
    mm_h: float
    logical_x: float
    logical_y: float
    logical_w: float
    logical_h: float
    px_per_mm_x: float
    px_per_mm_y: float
    placed_how: str
    placed_anchor: str


@dataclass(frozen=True)
class MmcursorState:
    version: str
    enabled: bool
    layout_active: bool
    gap_mm: float
    align: str
    rebuilds: int
    deferred: int
    refused: int
    pending: str
    live_override_active: bool
    cursor_mm: tuple[float, float] | None
    monitors: list[MonitorState]
    warnings: list[str]
    seam_gaps: list[tuple[str, str, float]]

    def monitor(self, name: str) -> MonitorState | None:
        for m in self.monitors:
            if m.name == name:
                return m
        return None

    def gap_between(self, a: str, b: str) -> float:
        """The mm gap `buildLayout` actually inserted between these two,
        falling back to the global gap when there's no per-seam override —
        the same precedence `mmcursor-gap` takes over `gap_mm` in the plugin."""
        for sa, sb, mm in self.seam_gaps:
            if {sa, sb} == {a, b}:
                return mm
        return self.gap_mm


def get_state() -> MmcursorState:
    raw = _run("-j", "mmcursor")
    try:
        d = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise HyprctlError(f"could not parse mmcursor JSON: {raw!r}") from exc

    cursor_mm = None
    cursor = d.get("cursor") or {}
    if isinstance(cursor.get("mm"), dict):
        cursor_mm = (cursor["mm"]["x"], cursor["mm"]["y"])

    monitors = []
    for m in d.get("monitors", []):
        placement = m.get("placement") or {}
        monitors.append(
            MonitorState(
                name=m["name"],
                mm_x=m["mm"]["x"], mm_y=m["mm"]["y"], mm_w=m["mm"]["w"], mm_h=m["mm"]["h"],
                logical_x=m["logical"]["x"], logical_y=m["logical"]["y"],
                logical_w=m["logical"]["w"], logical_h=m["logical"]["h"],
                px_per_mm_x=m["pxPerMM"]["x"], px_per_mm_y=m["pxPerMM"]["y"],
                placed_how=placement.get("how", ""),
                placed_anchor=placement.get("anchor", ""),
            )
        )

    return MmcursorState(
        version=d.get("version", "?"),
        enabled=bool(d.get("enabled", False)),
        layout_active=d.get("layout") == "active",
        gap_mm=d.get("gapMM", 0.0),
        align=d.get("align", "derive"),
        rebuilds=d.get("rebuilds", 0),
        deferred=d.get("deferred", 0),
        refused=d.get("refused", 0),
        pending=d.get("pending", ""),
        live_override_active=bool(d.get("liveOverrideActive", False)),
        cursor_mm=cursor_mm,
        monitors=monitors,
        warnings=list(d.get("warnings", [])),
        seam_gaps=[(g["a"], g["b"], g["mm"]) for g in d.get("seamGaps", [])],
    )


def set_place(name: str, x_mm: float, y_mm: float) -> None:
    _run("mmcursor", "place", name, f"{x_mm:.3f}", f"{y_mm:.3f}")


def set_offset(name: str, dx_mm: float, dy_mm: float) -> None:
    _run("mmcursor", "offset", name, f"{dx_mm:.3f}", f"{dy_mm:.3f}")


def reload_config() -> None:
    """Drops all live GUI overrides and re-reads hyprland.conf. `hyprctl reload`
    re-parses the whole config (picking up a saved layout file); `hyprctl
    mmcursor reload` alone would only re-read plugin:mmcursor:* *values*, not
    clear live place/offset overrides left by a `keyword`-style poke — using
    the full reload keeps this button's meaning unambiguous: "throw away
    anything this session changed that wasn't saved."
    """
    _run("reload")
