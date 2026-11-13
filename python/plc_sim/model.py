"""Pure conveyor simulation (no I/O, no clock): feed it a setpoint and step(dt)."""
from __future__ import annotations

from dataclasses import dataclass

FAULT_NONE = 0
# The commanded setpoint is above max_speed. The belt runs clamped at
# max_speed; the fault clears as soon as a valid setpoint is written.
FAULT_SETPOINT_RANGE = 1


@dataclass
class ConveyorParams:
    initial_speed: float  # m/s; also the initial setpoint
    max_speed: float  # m/s
    ramp_per_s: float  # m/s^2: how fast the actual speed follows the setpoint
    item_period_s: float  # an item passes the photo-eye this often at nominal_speed
    item_duty: float  # fraction of that period the photo-eye is blocked
    nominal_speed: float  # m/s; with item_period_s it fixes the item spacing

    def validate(self) -> None:
        if self.max_speed <= 0:
            raise ValueError("max_speed must be positive")
        if not 0 <= self.initial_speed <= self.max_speed:
            raise ValueError("initial_speed must be within 0..max_speed")
        if self.ramp_per_s <= 0:
            raise ValueError("ramp_per_s must be positive")
        if self.item_period_s <= 0 or self.nominal_speed <= 0:
            raise ValueError("item_period_s and nominal_speed must be positive")
        if not 0 <= self.item_duty <= 1:
            raise ValueError("item_duty must be within 0..1")


class ConveyorModel:
    """Belt speed ramp plus a photo-eye driven by belt travel.

    Items sit on the belt at a fixed pitch (item_period_s * nominal_speed
    metres), so the photo-eye toggles as a function of distance travelled:
    faster belt, faster items; stopped belt, no new items.
    """

    def __init__(self, params: ConveyorParams) -> None:
        params.validate()
        self.params = params
        self.setpoint = params.initial_speed
        self.speed = params.initial_speed
        self.travel_m = 0.0
        self._pitch_m = params.item_period_s * params.nominal_speed

    def step(self, dt: float) -> None:
        target = min(max(self.setpoint, 0.0), self.params.max_speed)
        max_delta = self.params.ramp_per_s * dt
        delta = target - self.speed
        self.speed += max(-max_delta, min(max_delta, delta))
        self.travel_m += self.speed * dt

    @property
    def item_present(self) -> bool:
        phase = (self.travel_m % self._pitch_m) / self._pitch_m
        return phase < self.params.item_duty

    @property
    def fault_code(self) -> int:
        return FAULT_SETPOINT_RANGE if self.setpoint > self.params.max_speed else FAULT_NONE
