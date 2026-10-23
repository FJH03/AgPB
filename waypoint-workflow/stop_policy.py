"""Versioned acceptance of sampled route endpoints; never changes waypoint data.

Intermediate route nodes establish traversal only. This policy applies to the
requested endpoint and its final continuous sample window. The matching terminal
controller uses the same 4-unit XY floor and separate 4-unit vertical bound.
"""
import math

VERSION = 1
MIN_XY_RADIUS = 4.0
MAX_VERTICAL_ERROR = 4.0
MAX_SPEED = 5.0
MIN_DURATION = 0.8
MAX_SAMPLE_GAP = 1.5
MIN_SAMPLES = 3
FL_ONGROUND = 1


def description():
    return {"version": VERSION, "scope": "sampled_endpoint_only",
            "min_xy_radius": MIN_XY_RADIUS, "max_vertical_error": MAX_VERTICAL_ERROR,
            "max_speed": MAX_SPEED, "min_duration": MIN_DURATION,
            "max_sample_gap": MAX_SAMPLE_GAP, "min_samples": MIN_SAMPLES,
            "ground_flag": FL_ONGROUND}


def finite_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def finite_vector(value):
    return isinstance(value, list) and len(value) == 3 and all(finite_number(v) for v in value)


def normalize_flags(value):
    """SourcePawn prints int32 as signed; preserve the engine's uint32 bits."""
    if type(value) is not int or not -(1 << 31) <= value <= (1 << 32) - 1:
        return None
    return value & 0xffffffff


def measure(node, sample):
    radius = node["radius"]
    result = {"raw_radius": radius, "effective_radius": max(radius, MIN_XY_RADIUS),
              "xy_distance": None, "vertical_error": None, "stopped_distance": None,
              "within_radius": False, "within_vertical": False, "grounded": False,
              "sample_speed": None}
    if not isinstance(sample, dict):
        return result
    if finite_vector(sample.get("position")):
        delta = [sample["position"][i] - node[key] for i, key in enumerate(("x", "y", "z"))]
        result.update(xy_distance=math.hypot(delta[0], delta[1]), vertical_error=abs(delta[2]),
                      stopped_distance=math.sqrt(sum(v * v for v in delta)))
        result["within_radius"] = result["xy_distance"] <= result["effective_radius"]
        result["within_vertical"] = result["vertical_error"] <= MAX_VERTICAL_ERROR
    flags = normalize_flags(sample.get("flags"))
    result["grounded"] = flags is not None and bool(flags & FL_ONGROUND)
    if finite_vector(sample.get("velocity")):
        result["sample_speed"] = math.sqrt(sum(v * v for v in sample["velocity"]))
    return result


def evaluate(node, samples, after):
    """Recompute a final, same-client low-speed and precise sample window.

    Missing velocity/ground/identity/time evidence cannot pass. A broken time
    sequence, excessive sample gap, movement or tolerance violation resets the
    relevant window. An earlier good window cannot certify a different after.
    """
    result = measure(node, after)
    result.update(stop_policy_version=VERSION, stop_scope="sampled_endpoint_only",
                  settled=False, settled_seconds=0.0, precise_stop_passed=False,
                  precise_seconds=0.0, stop_status="unsettled", precision_reasons=[])
    if not isinstance(samples, list) or not samples:
        result["precision_reasons"].append("missing endpoint sample sequence")
        return result
    previous, identity = None, None
    low_start, good_start, low_count, good_count = None, None, 0, 0
    for sample in samples:
        if not isinstance(sample, dict):
            previous, low_start, good_start, low_count, good_count = None, None, None, 0, 0
            continue
        stamp, slot, userid = sample.get("time"), sample.get("slot"), sample.get("userid")
        valid = (finite_number(stamp) and type(slot) is int and slot > 0 and
                 type(userid) is int and userid > 0)
        if valid and identity is None:
            identity = slot, userid
        valid = valid and identity == (slot, userid)
        measured = measure(node, sample)
        dt = stamp - previous["time"] if valid and previous is not None else None
        linked = previous is None or (dt is not None and 0 < dt <= MAX_SAMPLE_GAP)
        if not linked:
            low_start, good_start, low_count, good_count = None, None, 0, 0
        low = (valid and measured["grounded"] and measured["sample_speed"] is not None and
               measured["sample_speed"] <= MAX_SPEED and finite_vector(sample.get("position")))
        if low and previous is not None and linked:
            if math.dist(previous["position"], sample["position"]) > MAX_SPEED * dt + 0.1:
                low = False
        good = low and measured["within_radius"] and measured["within_vertical"]
        if low:
            low_start = stamp if low_start is None else low_start
            low_count += 1
        else:
            low_start, low_count = None, 0
        if good:
            good_start = stamp if good_start is None else good_start
            good_count += 1
        else:
            good_start, good_count = None, 0
        previous = sample if valid and finite_vector(sample.get("position")) else None
    if low_start is not None and previous is not None:
        result["settled_seconds"] = previous["time"] - low_start
        result["settled"] = low_count >= MIN_SAMPLES and result["settled_seconds"] >= MIN_DURATION
    if good_start is not None and previous is not None:
        result["precise_seconds"] = previous["time"] - good_start
        result["precise_stop_passed"] = good_count >= MIN_SAMPLES and result["precise_seconds"] >= MIN_DURATION
    fields = ("position", "velocity", "flags", "time", "slot", "userid")
    after_matches = isinstance(after, dict) and isinstance(samples[-1], dict) and all(
        key in after and key in samples[-1] and after[key] == samples[-1][key] for key in fields)
    after_matches = (after_matches and all(type(after.get(key)) is int for key in ("flags", "slot", "userid"))
                     and finite_number(after.get("time")))
    final_valid = (result["within_radius"] and result["within_vertical"] and result["grounded"] and
                   result["sample_speed"] is not None and result["sample_speed"] <= MAX_SPEED)
    result["precise_stop_passed"] = result["precise_stop_passed"] and after_matches and final_valid
    if not after_matches:
        result["precise_stop_passed"] = False
        result["precision_reasons"].append("after differs from final endpoint sample or lacks evidence")
    if not result["within_radius"]:
        result["precision_reasons"].append("XY error exceeds effective radius")
    if not result["within_vertical"]:
        result["precision_reasons"].append("vertical error exceeds 4 units")
    if not result["grounded"]:
        result["precision_reasons"].append("missing grounded evidence")
    if result["sample_speed"] is None or result["sample_speed"] > MAX_SPEED:
        result["precision_reasons"].append("missing or excessive velocity")
    if not result["precise_stop_passed"]:
        result["precision_reasons"].append("final continuous precision window not established")
    if result["precise_stop_passed"]:
        result["stop_status"] = "settled_in_radius"
    elif result["settled"] and (not result["within_radius"] or not result["within_vertical"]):
        # Retain the historical status name; XY and Z failure fields are separate.
        result["stop_status"] = "out_of_radius"
    return result
