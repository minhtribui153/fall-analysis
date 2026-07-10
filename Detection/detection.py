"""
detection.py
Fall / near-miss classifier over a rolling IMU buffer.
Hardware-agnostic: feed it (ax, ay, az, ts) from BLE, Kafka, CSV replay, or a simulator.

1. Trigger condition is now OR, not AND.
   Original: `smv >= STUMBLE_THRESH and gyro_mag >= TRIGGER_GYRO` on the SAME sample.
   In a real fall the rotational peak (mid-fall tumble) and the impact peak (ground
   contact) rarely land on the same sample, so an AND trigger risks missing real
   events. Widening the trigger to OR doesn't hurt precision: classification still
   independently re-confirms BOTH an impact spike and a rotation spike before a
   FALL is ever returned.

2. FALL vs NEAR_MISS no longer depends on post-impact stillness.
   Original: a FALL required `is_still` afterwards. A conscious worker who is
   down and struggling/in pain will keep moving -- under the old logic that
   worker got demoted to NEAR_MISS, which is backwards for a safety-alert system.
   Now: FALL is decided by whether the worker ends up DOWN (large orientation
   change + is_down), and NEAR_MISS is a rotation/orientation disturbance where
   the worker does NOT end up down (i.e. they stumbled and recovered balance).
   Stillness is still computed, but reported separately as `responsiveness`
   ("still" / "active") for alert triage (e.g. "still" may mean unconscious and
   therefore higher urgency), instead of gating the event type.

3. Timing is anchored to the event's own timestamps, with a sample-count fallback.
   Original: `elapsed_samples = self._counter - self.event_start_count`, and the
   impact-timing check inside `_classify_event` divided raw buffer index by the
   nominal `sample_rate`. Both assume perfectly uniform sampling, which wireless
   links (BLE/Wi-Fi to the Nesso N1) don't guarantee. Timing is now computed from
   actual sample timestamps (`ts`) relative to `event_start_ts`, and only falls
   back to `sample_count * nominal_dt` when timestamps are missing or non-monotonic.
   This also removes the old implicit (undocumented) coupling between buffer size
   and RECOVERY_WINDOW that the impact-time window used to rely on.

4. Basic bad-sample handling.
   A single corrupted BLE packet (None / NaN / non-finite values) is no longer
   appended to the buffer as-is, which could previously produce a phantom trigger
   or corrupt the tilt/impact calculations. Bad samples are now dropped; a run of
   consecutive bad samples long enough to represent a real link outage resets any
   in-progress event instead of classifying across a dead zone.

None of this replaces real calibration. Thresholds below are still placeholders --
calibrate them against captured data from the actual mounted device and realistic
site motions (kneeling, ladder climbing, bending, sitting, riding in a vehicle)
before trusting the false positive / false negative rate.
"""

import math
import statistics
from collections import deque
from datetime import datetime

# ── Tunable thresholds — calibrate against real captured data ──────────────
FREEFALL_THRESH   = 0.5    # g, below this = possible free-fall
IMPACT_THRESH     = 2.5    # g, above this = impact spike
STUMBLE_THRESH    = 1.5    # g, elevated but not a full impact

STILL_VARIANCE    = 0.15   # g^2, low variance = not moving
RECOVERY_WINDOW   = 2.0    # seconds to check for quick recovery / settle
FFH_FREEFALL_MIN  = 0.2    # seconds — above this, classify as FFH not STF

TRIGGER_GYRO      = 120    # deg/s
ROTATION_SPIKE    = 180    # deg/s

MAX_DROPPED_IN_A_ROW = 10  # consecutive bad samples treated as a real link outage


def _smv(ax, ay, az):
    return math.sqrt(ax ** 2 + ay ** 2 + az ** 2)


def _gyro_magnitude(gx, gy, gz):
    """Angular velocity magnitude (deg/s). Used only as a secondary
    confidence check -- NOT the sole trigger. A tumbling fall
    typically produces a sharp rotational spike; a controlled motion
    (e.g. sitting down, placing the device on a table) usually doesn't,
    even if it briefly crosses the accelerometer impact threshold."""
    return math.sqrt(gx ** 2 + gy ** 2 + gz ** 2)


def _tilt_angle(ax, ay, az):
    """Angle from vertical (upright), in degrees."""
    mag = _smv(ax, ay, az)
    if mag == 0:
        return 0
    return math.degrees(math.acos(min(1.0, abs(ax) / mag)))


def _elapsed_seconds(ts_start, ts_end, fallback_dt=None):
    """Elapsed seconds between two timestamps. Handles both numeric
    (float/int, seconds) and datetime timestamps. Falls back to
    `fallback_dt` if timestamps are missing, mismatched in type, or
    non-monotonic (e.g. a clock jump / out-of-order packet)."""
    if ts_start is None or ts_end is None:
        return fallback_dt
    try:
        if isinstance(ts_start, datetime) and isinstance(ts_end, datetime):
            delta = (ts_end - ts_start).total_seconds()
        else:
            delta = float(ts_end) - float(ts_start)
    except (TypeError, ValueError):
        return fallback_dt
    if delta < 0:
        return fallback_dt
    return delta


class FallDetector:
    """Event-triggered fall/near-miss classifier over a rolling IMU buffer.

    Uses a monotonic sample counter as a fallback time base only -- real
    elapsed time (from `ts`) is preferred whenever timestamps are usable,
    since wireless sampling is rarely perfectly uniform.
    """

    def __init__(self, sample_rate=100):
        self.sample_rate = sample_rate
        self.nominal_dt = 1.0 / sample_rate
        self.buffer = deque(maxlen=sample_rate * 3)  # ~3s of history (physical capacity)
        self.in_event = False
        self.event_start_count = None
        self.event_start_ts = None
        self._counter = 0
        self._dropped_in_a_row = 0

    def add_sample(self, ax, ay, az, ts, gx=0.0, gy=0.0, gz=0.0):
        """
        ts: seconds (float) or datetime — must be consistent within a session.
        gx, gy, gz: optional gyroscope values, used only as a secondary
        confidence signal on top of the primary accelerometer-based trigger.
        Returns a classification dict when an event completes, else None.
        """
        clean = self._sanitize(ax, ay, az, gx, gy, gz)
        if clean is None:
            self._dropped_in_a_row += 1
            if self._dropped_in_a_row >= MAX_DROPPED_IN_A_ROW:
                # Long enough gap that it's a real link outage, not sensor
                # noise -- don't let a stale in-progress event span across it.
                self.in_event = False
                self.event_start_count = None
                self.event_start_ts = None
            self._counter += 1
            return None

        self._dropped_in_a_row = 0
        ax, ay, az, gx, gy, gz = clean

        smv = _smv(ax, ay, az)
        tilt = _tilt_angle(ax, ay, az)
        gyro_mag = _gyro_magnitude(gx, gy, gz)
        self.buffer.append({"ts": ts, "smv": smv, "tilt": tilt,
                             "gyro_mag": gyro_mag, "count": self._counter})

        # Trigger on EITHER signal (see revision note 1 above). Classification
        # still independently requires both an impact spike and a rotation
        # spike before labeling anything a FALL.
        if not self.in_event and (smv >= STUMBLE_THRESH or gyro_mag >= TRIGGER_GYRO):
            self.in_event = True
            self.event_start_count = self._counter
            self.event_start_ts = ts

        result = None
        if self.in_event:
            fallback = (self._counter - self.event_start_count) * self.nominal_dt
            elapsed = _elapsed_seconds(self.event_start_ts, ts, fallback_dt=fallback)
            if elapsed >= RECOVERY_WINDOW:
                result = self._classify_event()
                self.in_event = False
                self.event_start_count = None
                self.event_start_ts = None

        self._counter += 1
        return result

    @staticmethod
    def _sanitize(ax, ay, az, gx, gy, gz):
        """Reject packets with missing or non-finite values instead of
        letting them corrupt the buffer / trigger a phantom event."""
        values = (ax, ay, az, gx, gy, gz)
        if any(v is None for v in values):
            return None
        try:
            values = tuple(float(v) for v in values)
        except (TypeError, ValueError):
            return None
        if any(not math.isfinite(v) for v in values):
            return None
        return values

    def _time_since_event_start(self, sample):
        fallback = (sample["count"] - self.event_start_count) * self.nominal_dt
        return _elapsed_seconds(self.event_start_ts, sample["ts"], fallback_dt=fallback)

    def _time_between(self, sample_a, sample_b):
        fallback = (sample_b["count"] - sample_a["count"]) * self.nominal_dt
        return _elapsed_seconds(sample_a["ts"], sample_b["ts"], fallback_dt=fallback)

    def _classify_event(self):

        event = list(self.buffer)

        if len(event) < self.sample_rate:
            return None

        smv_series = [s["smv"] for s in event]
        gyro_series = [s["gyro_mag"] for s in event]
        tilt_series = [s["tilt"] for s in event]

        # -----------------------------
        # Peak impact -- timing is measured relative to the event's own
        # start (see revision note 3), not raw buffer index.
        # -----------------------------
        max_smv = max(smv_series)
        impact_index = smv_series.index(max_smv)
        impact_sample = event[impact_index]

        impact_time = self._time_since_event_start(impact_sample)
        time_after_impact = self._time_between(impact_sample, event[-1])

        valid_impact = (
            max_smv >= IMPACT_THRESH
            and 0.1 <= impact_time <= 1.5
            and time_after_impact >= 1.0
        )

        # -----------------------------
        # Consecutive freefall duration
        # -----------------------------
        longest_freefall = 0
        current = 0

        for smv in smv_series:
            if smv < FREEFALL_THRESH:
                current += 1
                longest_freefall = max(longest_freefall, current)
            else:
                current = 0

        freefall_duration = longest_freefall / self.sample_rate
        had_freefall = freefall_duration >= FFH_FREEFALL_MIN

        # -----------------------------
        # Orientation
        # -----------------------------
        start_tilt = statistics.mean(tilt_series[:20])
        end_tilt = statistics.mean(tilt_series[-20:])

        tilt_change = abs(end_tilt - start_tilt)

        is_down = end_tilt > 55
        large_orientation_change = tilt_change > 20

        # -----------------------------
        # Stillness AFTER impact -- kept as a signal, but (see revision note 2)
        # no longer gates FALL vs NEAR_MISS. Reported as `responsiveness` for
        # downstream alert triage instead.
        # -----------------------------
        post_impact = event[impact_index:]

        if len(post_impact) >= self.sample_rate:

            tail = post_impact[-self.sample_rate:]

            tail_smv = [s["smv"] for s in tail]

            variance = statistics.pvariance(tail_smv)

            mean_smv = statistics.mean(tail_smv)

        else:

            variance = 999
            mean_smv = 999

        is_still = (
            variance < STILL_VARIANCE
            and abs(mean_smv - 1.0) < 0.25
        )
        responsiveness = "still" if is_still else "active"

        # -----------------------------
        # Rotation
        # -----------------------------
        max_rotation = max(gyro_series)

        had_rotation_spike = max_rotation > ROTATION_SPIKE

        # =====================================================
        # Classification
        #
        # FALL: valid impact + rotation spike + large orientation change,
        # and the worker ends up down. `responsiveness` ("still"/"active")
        # travels with the event for alert triage but does not decide the
        # label -- a struggling, conscious worker is still a FALL.
        #
        # NEAR_MISS: rotation spike + large orientation change, but the
        # worker does NOT end up down -- i.e. they stumbled and recovered
        # their balance before settling.
        # =====================================================

        if (
            valid_impact
            and had_rotation_spike
            and large_orientation_change
            and is_down
        ):

            fall_type = "FFH" if had_freefall else "STF"

            return {
                "event_type": "FALL",
                "fall_type": fall_type,
                "responsiveness": responsiveness,
                "max_smv": max_smv,
                "max_rotation": max_rotation,
                "freefall_duration": freefall_duration,
                "tilt_change": tilt_change,
                "ts": event[-1]["ts"],
            }

        elif (
            had_rotation_spike
            and large_orientation_change
            and not is_down
        ):

            return {
                "event_type": "NEAR_MISS",
                "fall_type": None,
                "responsiveness": responsiveness,
                "max_smv": max_smv,
                "max_rotation": max_rotation,
                "freefall_duration": freefall_duration,
                "tilt_change": tilt_change,
                "ts": event[-1]["ts"],
            }

        return {
            "event_type": "NORMAL",
            "fall_type": None,
            "responsiveness": responsiveness,
            "max_smv": max_smv,
            "max_rotation": max_rotation,
            "freefall_duration": freefall_duration,
            "tilt_change": tilt_change,
            "ts": event[-1]["ts"],
        }