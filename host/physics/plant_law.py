"""Relais — Python mirror of the rack model and of the control law (M31).

Mirrors `core/src/plant.c` and `core/src/control.c` operation by operation, in
float32 precision (every intermediate is rounded like the C `float` type), so a
replay of a journal reproduces the C values within 1 ulp. The C code stays the
authority: this mirror exists only to answer "what would ANOTHER testament
strategy have played?" offline (idea 69) and to drive the dataset world (idea
62) where the setpoint comes from a CSV file instead of the C generator.

Requirement IDs: CMD-02 (law), CMD-05 (rack), CMD-06 / ACT-05 (bounds),
ACT-02 (testament), CMD-08 (brake profile, used through the journals).

Standard library only. Every function is pure or mutates only its own state.
"""
from __future__ import annotations

import math
import struct
from typing import Callable, Dict, List, Optional, Sequence

# ---- rack model constants (core/src/plant.c) ----
PLANT_GAIN = 4.0
PLANT_DAMP = 1.0
PLANT_TAU = 0.02
VEL_LIMIT = 10.0 * PLANT_GAIN

# ---- control law constants (core/src/control.c) ----
CTL_KP = 60.0
CTL_KD = 0.5
CTL_KI = 4.0
CTL_FILTER = 0.35
CTL_INTEG_LIMIT = 0.25
CTL_RATE_LIMIT = 20.0
FALLBACK_DAMP = 0.5
TESTAMENT_MAX = 8

_F32 = struct.Struct("<f")


def f32(x: float) -> float:
    """Round to IEEE-754 single precision, like every C `float` operation."""
    return _F32.unpack(_F32.pack(x))[0]


def default_params() -> Dict[str, float]:
    """Defaults matching relais_params_default() (core/src/params.c)."""
    return {"T": 5, "p": 1, "N": 8, "certify": 3, "bail": 4,
            "eps": 0.005, "delta": 0.02, "degraded": 6000,
            "amp": 1.0, "slew": 0.25}


def step_s(params: Dict[str, float]) -> float:
    """Physical step of one tick in seconds (relais_params_step_s)."""
    return float(params["T"]) / 1000.0


def clamp_value(v: float, params: Dict[str, float]) -> float:
    """ACT-05 amplitude bound; also catches NaN, like relais_clamp_value."""
    amp = float(params["amp"])
    if not (v > -amp):
        return f32(-amp)
    if v > amp:
        return f32(amp)
    return v


def clamp_slew(previous: float, wanted: float, params: Dict[str, float]) -> float:
    """ACT-05 rate bound per tick (relais_clamp_slew)."""
    limit = f32(float(params["slew"]))
    delta = f32(wanted - previous)
    if delta > limit:
        return f32(previous + limit)
    if delta < -limit:
        return f32(previous - limit)
    return wanted


class Plant:
    """The simulated rack (CMD-05), mirror of relais_plant_step."""

    def __init__(self, pos0: float = 0.0):
        self.pos = f32(pos0)
        self.vel = 0.0
        self.t = 0.0

    def step(self, value: float, disturbance: float, dt: float) -> None:
        """One exact-integrated velocity-loop step, clamped like the C."""
        alpha = f32(1.0 - f32(math.exp(f32(-dt / PLANT_TAU))))
        target = f32(f32(PLANT_GAIN / PLANT_DAMP) * value)
        acc = f32(f32((target - self.vel) * alpha) + f32(disturbance * dt))
        if not math.isfinite(acc):
            acc = 0.0
        self.vel = f32(self.vel + acc)
        if not math.isfinite(self.vel):
            self.vel = 0.0
        self.vel = min(VEL_LIMIT, max(-VEL_LIMIT, self.vel))
        self.pos = f32(self.pos + f32(self.vel * dt))
        if not math.isfinite(self.pos):
            self.pos = 0.0
        self.t = f32(self.t + dt)


class CtlState:
    """The control law state (relais_ctl_state) and one period of CMD-02."""

    FIELDS = ("integ", "filt", "last_pos", "last_value", "phase", "fb_value",
              "prev_setpoint", "setpoint_rate", "ticks")

    def __init__(self):
        self.integ = 0.0
        self.filt = 0.0
        self.last_pos = 0.0
        self.last_value = 0.0
        self.phase = 0.0
        self.fb_value = 0.0
        self.prev_setpoint = 0.0
        self.setpoint_rate = 0.0
        self.ticks = 0

    def snapshot(self) -> Dict[str, float]:
        """A plain copy, what ETAT carries between the two computers (REP-01)."""
        return {k: getattr(self, k) for k in self.FIELDS}

    def restore(self, snap: Dict[str, float]) -> None:
        """Import a replicated state (REP-02)."""
        for k in self.FIELDS:
            setattr(self, k, snap[k])

    def step(self, meas: Dict[str, float], params: Dict[str, float]) -> float:
        """One period of the law, mirror of relais_ctl_step (CMD-02, CMD-06)."""
        dt = f32(step_s(params))
        rate = f32(f32(meas["setpoint"] - self.prev_setpoint) / dt)
        rate = min(CTL_RATE_LIMIT, max(-CTL_RATE_LIMIT, rate))
        self.setpoint_rate = f32(self.setpoint_rate
                                 + f32(CTL_FILTER * f32(rate - self.setpoint_rate)))
        self.prev_setpoint = f32(meas["setpoint"])
        error = f32(meas["setpoint"] - meas["pos"])
        self.filt = f32(self.filt + f32(CTL_FILTER * f32(meas["pos"] - self.filt)))
        self.integ = f32(self.integ + f32(error * dt))
        self.integ = min(CTL_INTEG_LIMIT, max(-CTL_INTEG_LIMIT, self.integ))
        wanted = f32(f32(f32(CTL_KP * error) - f32(CTL_KD * meas["vel"]))
                     + f32(CTL_KI * self.integ))
        bounded = clamp_value(wanted, params)
        if bounded != wanted:
            self.integ = f32(self.integ - f32(error * dt))  # anti-windup
        value = clamp_slew(self.last_value, bounded, params)
        self.last_value = value
        self.last_pos = f32(meas["pos"])
        self.phase = f32(self.phase + dt)
        self.fb_value = value
        self.ticks += 1
        return value

    def reconstruct(self, meas: Dict[str, float], params: Dict[str, float]) -> None:
        """REP-04: rebuild a bump-free state from what the actuator publishes."""
        self.last_value = clamp_value(meas.get("applied", 0.0), params)
        self.fb_value = self.last_value
        self.filt = meas["pos"]
        self.last_pos = meas["pos"]
        self.integ = 0.0


def fallback_step(fb_state: float, meas: Dict[str, float],
                  params: Dict[str, float]) -> float:
    """ACT-03/ACT-04 local damped loop, mirror of relais_fallback_step."""
    wanted = clamp_value(f32(-FALLBACK_DAMP * meas.get("vel", 0.0)), params)
    return clamp_slew(fb_state, wanted, params)


def unit_noise(seed: int, seconds: float) -> float:
    """Deterministic pseudo noise of plant.c, mirror of unit_noise()."""
    step = int(seconds * 1000.0) & 0xFFFFFFFF
    mixed = ((step * 0x9E3779B97F4A7C15) ^ ((seed & 0xFFFFFFFF) << 32)) & 0xFFFFFFFFFFFFFFFF
    mixed ^= mixed >> 33
    mixed = (mixed * 0xFF51AFD7ED558CCD) & 0xFFFFFFFFFFFFFFFF
    mixed ^= mixed >> 33
    return f32((mixed & 0xFFFFFF) / 8388607.5 - 1.0)


def traj_disturbance(dist_amp: float, dist_freq: float, seed: int,
                     seconds: float) -> float:
    """Mirror of relais_traj_disturbance (road push, CMD-05)."""
    if not dist_amp > 0.0:
        return 0.0
    wave = f32(math.sin(f32(6.283185307179586 * dist_freq * seconds)))
    noise = unit_noise(seed, seconds)
    return f32(dist_amp * f32(f32(0.6 * wave) + f32(0.4 * noise)))


# ---------------------------------------------------------------------------
# Testament filling strategies (ACT-02, idea 69). Each takes the holder state
# AFTER it computed its order, the measurements of that tick, the parameters,
# and the recent command history (oldest first); each returns exactly N values
# for the next N ticks, bounded like the law itself (CMD-06).
# ---------------------------------------------------------------------------

Strategy = Callable[[CtlState, Dict[str, float], Dict[str, float],
                     Sequence[float]], List[float]]


def testament_model(state: CtlState, meas: Dict[str, float],
                    params: Dict[str, float],
                    history: Sequence[float]) -> List[float]:
    """The CURRENT core strategy: prediction by the model (control.c)."""
    n = min(int(params["N"]), TESTAMENT_MAX)
    copy = CtlState()
    copy.restore(state.snapshot())
    predicted = Plant(meas["pos"])
    predicted.vel = meas["vel"]
    dt = f32(step_s(params))
    last = copy.last_value
    out: List[float] = []
    for i in range(n):
        predicted.step(last, 0.0, dt)
        ahead = {"pos": predicted.pos, "vel": predicted.vel, "applied": last,
                 "setpoint": f32(meas["setpoint"]
                                 + f32(copy.setpoint_rate * f32(dt * (i + 1))))}
        last = copy.step(ahead, params)
        out.append(last)
    return out


def _bounded_chain(start: float, wanted: Sequence[float],
                   params: Dict[str, float]) -> List[float]:
    """Apply the ACT-05 bounds to a wanted command sequence (shared helper)."""
    out: List[float] = []
    prev = start
    for w in wanted:
        prev = clamp_slew(prev, clamp_value(w, params), params)
        out.append(prev)
    return out


def testament_hold_last(state: CtlState, meas: Dict[str, float],
                        params: Dict[str, float],
                        history: Sequence[float]) -> List[float]:
    """Strategy 2: freeze the command at the last value (maintien)."""
    n = min(int(params["N"]), TESTAMENT_MAX)
    return [state.last_value] * n


def testament_linear(state: CtlState, meas: Dict[str, float],
                     params: Dict[str, float],
                     history: Sequence[float]) -> List[float]:
    """Strategy 3: linear extrapolation of the last command slope."""
    n = min(int(params["N"]), TESTAMENT_MAX)
    last = state.last_value
    prev = history[-2] if len(history) >= 2 else last
    slope = f32(last - prev)
    return _bounded_chain(last, [f32(last + f32(slope * (i + 1)))
                                 for i in range(n)], params)


def testament_setpoint_slope(state: CtlState, meas: Dict[str, float],
                             params: Dict[str, float],
                             history: Sequence[float]) -> List[float]:
    """Strategy 4: extrapolate with the filtered slope of the setpoint."""
    n = min(int(params["N"]), TESTAMENT_MAX)
    last = state.last_value
    dt = f32(step_s(params))
    per_tick = f32(state.setpoint_rate * dt)
    return _bounded_chain(last, [f32(last + f32(per_tick * (i + 1)))
                                 for i in range(n)], params)


def testament_moving_average(state: CtlState, meas: Dict[str, float],
                             params: Dict[str, float],
                             history: Sequence[float]) -> List[float]:
    """Strategy 5: hold the mean of the last few commands (smoothing)."""
    n = min(int(params["N"]), TESTAMENT_MAX)
    window = list(history)[-4:] or [state.last_value]
    avg = f32(sum(window) / len(window))
    return _bounded_chain(state.last_value, [avg] * n, params)


STRATEGIES: Dict[str, Strategy] = {
    "model": testament_model,          # the current core choice (ACT-02)
    "hold_last": testament_hold_last,
    "linear": testament_linear,
    "setpoint_slope": testament_setpoint_slope,
    "moving_average": testament_moving_average,
}


def replay_law(rows: Sequence[Dict[str, float]], params: Dict[str, float],
               state: Optional[CtlState] = None) -> CtlState:
    """Advance the law over journal rows (each row = statut.meas of one tick).

    Returns the final state; `state` is mutated in place when given, so a
    caller can snapshot the chain at handover instants.
    """
    st = state if state is not None else CtlState()
    for r in rows:
        st.step(r, params)
    return st
