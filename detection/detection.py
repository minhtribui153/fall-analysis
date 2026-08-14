"""
detection.py

Turns raw IMU readings (accelerometer + gyro) from the waist-mounted
NESSO N1 into two things:

  1. FallDetector -- watches for falls and near-misses. Event-triggered:
     stays quiet during normal activity, fires once when something
     worth flagging happens.

  2. ActivityClassifier -- a lighter, always-on guess at what the
     person's currently doing (standing still, walking, running,
     jumping). Runs continuously in the background, mostly useful for
     dashboard context and for tagging what someone was doing right
     before a fall got flagged.

Everything here is threshold-based, no ML. That's a deliberate choice
given the timeline -- it's easy to reason about, easy to recalibrate
against new recordings, and doesn't need a training pipeline. The
tradeoff is these numbers are only as good as the data we've tested
them against, which so far is a handful of staged recordings. Treat
every threshold below as a starting point, not gospel -- keep feeding
it more real captures and adjust as patterns show up.

Quick history on where the fall-detection thresholds came from, since
it's not obvious just from reading the numbers:
  - Started with a simple "is the accel spike big enough" check.
  - That missed real falls with a fast impact (one of our test falls
    had only a 40ms rise time and got filtered out by an overly strict
    timing window -- that check got removed).
  - Switched from a single end-of-window tilt reading to "was the
    person down for a sustained stretch", because staged test falls
    have the person hop back up quickly to reset for the next take,
    and a single snapshot can catch them after they've already stood
    back up.
  - Added a cooldown after each detected event so the same fall
    doesn't fire five separate alerts as the person gets up and moves
    around afterward.
"""

import math
import statistics
from collections import deque
from datetime import datetime

# ── Fall detection thresholds ───────────────────────────────────────────
# Calibrated against real staged recordings (walking, tripping, slipping,
# and a couple of actual falls). Small sample size -- keep an eye on
# these as more data comes in.

FREEFALL_THRESH   = 0.5    # g -- below this, we're probably airborne
IMPACT_THRESH     = 2.0    # g -- a spike this big means something hit the ground
STUMBLE_THRESH    = 1.7    # g -- not necessarily a fall, but enough to start paying attention

STILL_VARIANCE    = 0.15   # g^2 -- how "flat" the signal needs to be to call it stillness
RECOVERY_WINDOW   = 2.8    # seconds we wait after a trigger before deciding what happened.
                             # (bumped up from 2.0s -- a real recovered slip was still
                             # mid-recovery at the 2s mark and got misread)
FFH_FREEFALL_MIN  = 0.2    # seconds of freefall before we'd call it a height-fall

TRIGGER_GYRO      = 180    # deg/s -- rotation alone can also kick off an event
ROTATION_SPIKE    = 180    # deg/s -- a proper tumble, not just a wobble

IS_DOWN_ANGLE        = 55   # degrees from "upright" -- past this, we're calling it down
IS_DOWN_SUSTAIN_MIN  = 0.15 # seconds -- has to stay down for a beat, not just flicker through
ORIENTATION_SPIKE_THRESH = 20  # degrees -- how far they have to lean/rotate to count as "something happened"

MAX_DROPPED_IN_A_ROW = 10   # bad packets in a row before we assume the link dropped
POST_EVENT_COOLDOWN  = 3.0  # seconds of quiet after a FALL/NEAR_MISS before we'll trigger again

PRE_TRIGGER_SECONDS       = 0.25  # small lead-in kept with each event
POST_IMPACT_TAIL_SECONDS  = 0.80  # tail used to judge stillness/recovery
FALL_DOWN_SUSTAIN_MIN     = 0.80  # stronger posture evidence than the trigger-level down check
NEAR_MISS_IMPACT_MIN      = 1.70
NEAR_MISS_ROTATION_MIN    = 220
NEAR_MISS_TILT_MIN        = 35
RECOVERY_UPRIGHT_ANGLE    = 45
RECOVERY_GYRO_MAX         = 180
RECOVERY_VARIANCE_MAX     = 0.10
RECOVERY_DOWN_ANGLE       = 55
NEAR_MISS_MAX_FREEFALL    = 0.10

# A heads-up on FFH_FREEFALL_MIN: in testing, this hasn't actually been able to
# tell a real height-fall apart from a same-level trip/slip -- both showed
# similarly short freefall times, probably because staged test falls don't
# involve dropping from an actual height. A real height fall should produce a
# noticeably longer freefall phase, but we haven't captured one of those yet
# to confirm the number. Worth flagging as a known limitation rather than
# pretending this threshold is solid.


def _smv(ax, ay, az):
    """Signal magnitude vector -- basically 'how much total acceleration',
    ignoring which direction it's pointing."""
    return math.sqrt(ax ** 2 + ay ** 2 + az ** 2)


def _gyro_magnitude(gx, gy, gz):
    """Same idea but for rotation speed (deg/s). Used as supporting
    evidence, not the main trigger -- a controlled motion like sitting
    down can still spike gyro briefly without being a fall."""
    return math.sqrt(gx ** 2 + gy ** 2 + gz ** 2)


def _tilt_angle(ax, ay, az):
    """Roughly: how far off 'upright' is the device right now, in degrees.

    this assumes gravity mostly sits on the X axis when the
    device is worn upright -- which is true for how the NESSO N1 sits
    on a waist clip, confirmed against real walking data (ax averaged
    around -0.91g at rest). If someone mounts the device differently
    (different clip angle, different body position), this axis
    assumption breaks and tilt readings will be meaningless. Worth
    double-checking if a second sensor gets added with a different mount.
    """
    mag = _smv(ax, ay, az)
    if mag == 0:
        return 0
    return math.degrees(math.acos(min(1.0, abs(ax) / mag)))


def _elapsed_seconds(ts_start, ts_end, fallback_dt=None):
    """Seconds between two timestamps, whether they're floats or
    datetimes. Falls back to `fallback_dt` if something's off (missing
    timestamp, clock jumped backwards, whatever) -- BLE over a wireless
    link isn't always perfectly behaved."""
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


class ActivityClassifier:
    """Keeps a rolling window of accel data and takes a guess at what
    the person's doing right now: stationary, walking, running, or
    jumping.

    This is intentionally simple -- no FFT, no ML, just basic stats on
    the accel magnitude over a short window. It won't win any research
    awards, but it correctly identifies all four states on real
    continuous recordings (stationary, walking, running, jumping --
    ~300-400 samples each, done back to back) once the window has
    enough data to say something.

    Worth knowing how walking/running/jumping actually get told apart:
    walking never dips below 0.6g in the real data (lowest recorded was
    0.678g), while running and jumping both do, repeatedly -- so "any
    dip at all" is what separates walking from the other two. Running
    and jumping then split on how OFTEN they dip: running's stride
    cycle is faster (~11 dips/sec in the recording) than a jump's
    land-and-reset cycle (~6.6 dips/sec). This is all calibrated
    against one continuous recording per activity, so it's a solid
    first pass, not a bulletproof one -- more recordings (different
    people, different paces) would help firm up that 8.5 dips/sec
    split point.

    One thing to watch for: right before a fall, the brief stumble/
    destabilization phase can look a lot like a jump's dip-then-spike
    signature. So `activity_before_event` on a FALL result sometimes
    reflects the fall's own onset rather than what the person was
    genuinely doing a second earlier -- seen this happen a few times
    in testing. Not a bug exactly, just a real limit of using a short
    rolling window right up against the moment something dramatic
    starts happening.

    Doesn't influence FallDetector's decisions right now, just runs
    alongside it and reports separately.
    """

    def __init__(self, sample_rate=100, window_seconds=1.0, update_every=20):
        self.sample_rate = sample_rate
        self.window_size = int(sample_rate * window_seconds)
        self.buffer = deque(maxlen=self.window_size)
        self.update_every = update_every  # recompute every N samples, not every single one --
                                            # activity doesn't change fast enough to justify
                                            # redoing this math 100x/sec
        self._since_update = 0
        self.state = "unknown"  # not enough data yet to say anything

    def add_sample(self, ax, ay, az):
        self.buffer.append(_smv(ax, ay, az))
        self._since_update += 1

        if len(self.buffer) < self.window_size:
            return self.state  # still filling the window

        if self._since_update >= self.update_every:
            self.state = self._classify()
            self._since_update = 0

        return self.state

    def _classify(self):
        values = list(self.buffer)
        std_smv = statistics.pstdev(values)

        # Barely moving at all -- standing around, working with hands,
        # sitting, that sort of thing. Real stationary data measured
        # std_smv ~0.003, so 0.08 leaves a lot of headroom.
        if std_smv < 0.08:
            return "stationary"

        # Everything past this point is about "airborne dips" -- brief
        # stretches where SMV drops toward near-weightlessness, which
        # happens during the flight phase of a stride or a jump but
        # basically never during walking. Real data backs this up:
        # walking's lowest recorded SMV was 0.678g -- it never dipped
        # below 0.6 at all, while running and jumping both did,
        # repeatedly.
        dips = self._find_dips(values, thresh=0.6)
        window_seconds = self.window_size / self.sample_rate
        dip_rate = len(dips) / window_seconds  # dips per second

        if not dips:
            return "walking"

        # Running and jumping both dip, but at different rates -- real
        # data showed running dipping about twice as often as jumping
        # (~11/sec vs ~6.6/sec), which makes sense: a running stride
        # cycles faster than a jump-land-reset cycle. 8.5 splits the
        # difference between the two measured rates.
        if dip_rate >= 8.5:
            return "running"
        return "jumping"

    @staticmethod
    def _find_dips(values, thresh):
        """Returns one entry per separate stretch where the signal drops
        below `thresh` -- each one is a brief 'airborne' moment."""
        dips = []
        in_dip = False
        for v in values:
            if v < thresh and not in_dip:
                dips.append(True)
                in_dip = True
            elif v >= thresh:
                in_dip = False
        return dips


class FallDetector:
    """Watches a stream of IMU samples and flags falls / near-misses.

    Stays quiet during normal activity. Wakes up when something crosses
    a trigger threshold, watches what happens over the next couple of
    seconds, then decides: was that a real fall, a stumble that got
    caught, or nothing worth mentioning.
    """

    def __init__(self, sample_rate=100, track_activity=True):
        self.sample_rate = sample_rate
        self.nominal_dt = 1.0 / sample_rate
        self.buffer = deque(maxlen=int(sample_rate * 4))  # ~4s of context, enough to cover
                                                              # lead-in + the RECOVERY_WINDOW tail
        self.in_event = False
        self.event_start_count = None
        self.event_start_ts = None
        self._counter = 0
        self._dropped_in_a_row = 0
        self._cooldown_until_ts = None
        self._cooldown_until_count = None

        self.activity = ActivityClassifier(sample_rate=sample_rate) if track_activity else None
        self._activity_at_trigger = None

    def add_sample(self, ax, ay, az, ts, gx=0.0, gy=0.0, gz=0.0):
        """
        ts: seconds (float) or datetime -- just needs to be consistent
        within a session.
        gx, gy, gz: gyro readings, optional (default 0.0 if you don't have them).
        Returns a result dict once an event finishes, otherwise None.
        """
        clean = self._sanitize(ax, ay, az, gx, gy, gz)
        if clean is None:
            self._dropped_in_a_row += 1
            if self._dropped_in_a_row >= MAX_DROPPED_IN_A_ROW:
                # Enough bad packets in a row that this looks like a real
                # link outage, not just noise -- don't let a half-finished
                # event span across a dead zone in the data.
                self.in_event = False
                self.event_start_count = None
                self.event_start_ts = None
            self._counter += 1
            return None

        self._dropped_in_a_row = 0
        ax, ay, az, gx, gy, gz = clean

        if self.activity is not None:
            self.activity.add_sample(ax, ay, az)

        smv = _smv(ax, ay, az)
        tilt = _tilt_angle(ax, ay, az)
        gyro_mag = _gyro_magnitude(gx, gy, gz)
        self.buffer.append({"ts": ts, "smv": smv, "tilt": tilt,
                             "gyro_mag": gyro_mag, "count": self._counter})

        in_cooldown = False
        if self._cooldown_until_ts is not None:
            fallback = (self._counter - self._cooldown_until_count) * self.nominal_dt
            since_cooldown_started = _elapsed_seconds(self._cooldown_until_ts, ts, fallback_dt=fallback)
            in_cooldown = since_cooldown_started < POST_EVENT_COOLDOWN

        # Either signal can kick things off -- a real fall's rotation
        # peak and impact peak don't always land on the exact same
        # sample, so requiring both at once risks missing real events.
        # Classification below still independently checks for both
        # before calling anything a FALL, so this doesn't hurt precision.
        if not self.in_event and not in_cooldown and (smv >= STUMBLE_THRESH or gyro_mag >= TRIGGER_GYRO):
            self.in_event = True
            self.event_start_count = self._counter
            self.event_start_ts = ts
            self._activity_at_trigger = self.activity.state if self.activity else None

        result = None
        if self.in_event:
            fallback = (self._counter - self.event_start_count) * self.nominal_dt
            elapsed = _elapsed_seconds(self.event_start_ts, ts, fallback_dt=fallback)
            if elapsed >= RECOVERY_WINDOW:
                result = self._classify_event()
                self.in_event = False
                self.event_start_count = None
                self.event_start_ts = None

                # Only real hazards get the long cooldown. The old code also
                # cooled down after NORMAL windows; a harmless walking/running
                # trigger could then hide a real fall that happened seconds later.
                if result is not None and result["event_type"] != "NORMAL":
                    self._cooldown_until_ts = ts
                    self._cooldown_until_count = self._counter
                else:
                    self._cooldown_until_ts = None
                    self._cooldown_until_count = None

        self._counter += 1
        return result

    @property
    def current_activity(self):
        """What the person seems to be doing right now (stationary /
        walking / running / jumping / unknown). Useful for a live
        dashboard feed -- doesn't require an event to be in progress."""
        return self.activity.state if self.activity else None

    @staticmethod
    def _sanitize(ax, ay, az, gx, gy, gz):
        """Drop anything obviously broken (missing values, NaN, inf)
        instead of letting it corrupt the buffer or spawn a phantom
        trigger."""
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

    def _longest_condition_duration(self, event, predicate):
        """Return longest continuous duration matching predicate using timestamps."""
        best = 0.0
        run_start = None
        previous = None

        for sample in event:
            if predicate(sample):
                if run_start is None:
                    run_start = sample
                previous = sample
            elif run_start is not None:
                best = max(
                    best,
                    self._time_between(run_start, previous) + self.nominal_dt,
                )
                run_start = None
                previous = None

        if run_start is not None and previous is not None:
            best = max(
                best,
                self._time_between(run_start, previous) + self.nominal_dt,
            )

        return best

    def _tail_by_time(self, event, seconds):
        """Take the final `seconds` of samples using their real timestamps."""
        if not event:
            return []
        end = event[-1]
        tail = []
        for sample in reversed(event):
            if self._time_between(sample, end) > seconds:
                break
            tail.append(sample)
        tail.reverse()
        return tail

    def _classify_event(self):
        # Do NOT classify the whole rolling buffer. Only keep the current
        # event plus a short lead-in; otherwise an impact from a previous
        # movement can be incorrectly attributed to this event.
        pre_trigger_samples = int(PRE_TRIGGER_SECONDS * self.sample_rate)
        first_count = max(0, self.event_start_count - pre_trigger_samples)
        event = [s for s in self.buffer if s["count"] >= first_count]

        if not event:
            return None

        smv_series = [s["smv"] for s in event]
        gyro_series = [s["gyro_mag"] for s in event]
        tilt_series = [s["tilt"] for s in event]

        max_smv = max(smv_series)
        impact_index = smv_series.index(max_smv)
        impact_sample = event[impact_index]
        time_after_impact = self._time_between(impact_sample, event[-1])
        valid_impact = max_smv >= IMPACT_THRESH and time_after_impact >= 0.5

        # Duration features are timestamp-based. This remains correct even if
        # the nominal 100 Hz stream arrives with missing BLE packets.
        freefall_duration = self._longest_condition_duration(
            event, lambda s: s["smv"] < FREEFALL_THRESH
        )
        had_freefall = freefall_duration >= FFH_FREEFALL_MIN

        lead = [s for s in event if s["count"] <= self.event_start_count]
        if not lead:
            lead = event[: min(5, len(event))]
        start_tilt = statistics.mean(s["tilt"] for s in lead)

        sustained_down_duration = self._longest_condition_duration(
            event, lambda s: s["tilt"] > IS_DOWN_ANGLE
        )

        peak_tilt = max(tilt_series)
        tilt_change = abs(peak_tilt - start_tilt)
        had_orientation_spike = tilt_change > ORIENTATION_SPIKE_THRESH

        max_rotation = max(gyro_series)
        had_rotation_spike = max_rotation > ROTATION_SPIKE

        # Judge the person's state from the final ~0.8 s rather than from an
        # arbitrary number of samples.
        post_impact = event[impact_index:]
        tail = self._tail_by_time(post_impact, POST_IMPACT_TAIL_SECONDS)

        if len(tail) >= 3:
            tail_smv = [s["smv"] for s in tail]
            tail_gyro = [s["gyro_mag"] for s in tail]
            tail_tilt = [s["tilt"] for s in tail]
            variance = statistics.pvariance(tail_smv)
            mean_smv = statistics.mean(tail_smv)
            mean_gyro = statistics.mean(tail_gyro)
            mean_tilt = statistics.mean(tail_tilt)
        else:
            variance = mean_smv = mean_gyro = mean_tilt = 999

        is_still = (
            variance < STILL_VARIANCE
            and abs(mean_smv - 1.0) < 0.25
            and mean_gyro < RECOVERY_GYRO_MAX
        )
        recovery_stabilized = (
            variance < RECOVERY_VARIANCE_MAX
            and mean_tilt < RECOVERY_UPRIGHT_ANGLE
            and mean_gyro < RECOVERY_GYRO_MAX
        )
        responsiveness = "still" if is_still else "active"

        base = {
            "max_smv": max_smv,
            "max_rotation": max_rotation,
            "freefall_duration": freefall_duration,
            "tilt_change": tilt_change,
            "responsiveness": responsiveness,
            "activity_before_event": self._activity_at_trigger,
            "ts": event[-1]["ts"],
        }

        # FALL: an impact, rotation, and orientation disturbance are required.
        # Post-fall evidence can come from either sustained lying posture OR
        # near-1g stillness, so one unreliable waist-angle measurement no
        # longer has veto power over an otherwise convincing fall.
        fall_posture_evidence = sustained_down_duration >= FALL_DOWN_SUSTAIN_MIN
        fall_recovery_evidence = (
            is_still and mean_tilt >= RECOVERY_DOWN_ANGLE
        )

        if (
            valid_impact
            and had_rotation_spike
            and had_orientation_spike
            and (fall_posture_evidence or fall_recovery_evidence)
        ):
            fall_type = "FFH" if had_freefall else "STF"
            return {**base, "event_type": "FALL", "fall_type": fall_type}

        # NEAR_MISS: a single substantial destabilisation followed by recovery.
        # This is deliberately stricter than the old "rotation + tilt + not down"
        # rule, which labelled ordinary walking/running as repeated near-misses.
        near_miss = (
            max_smv >= NEAR_MISS_IMPACT_MIN
            and max_rotation >= NEAR_MISS_ROTATION_MIN
            and tilt_change >= NEAR_MISS_TILT_MIN
            and sustained_down_duration < FALL_DOWN_SUSTAIN_MIN
            and freefall_duration < NEAR_MISS_MAX_FREEFALL
            and recovery_stabilized
        )

        if near_miss:
            return {**base, "event_type": "NEAR_MISS", "fall_type": None}

        return {**base, "event_type": "NORMAL", "fall_type": None}

