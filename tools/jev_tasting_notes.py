#!/usr/bin/env python3
"""Read a real-shot capture's free-text tasting notes with Jev, TypeSafe AI's
System One decision model, and compare that reading with the flavour overlay.

The sensory overlay (`docs/model.md`, "Sensory overlay") predicts seven axis
intensities and an under/balanced/over verdict, but nothing in it has ever been
checked against tasting (`assets/beans/README.md`). Capture records already
carry what a taster wrote -- `observations.taste`, `channeling`,
`puck_condition`, `anomalies`, `notes` -- as free text the comparison rules
cannot use. This tool turns that text into typed, confidence-gated values in
one Jev request, then lines them up against a simulated shot's `flavor` block.

What this is NOT, deliberately:

- Not part of the simulator. Nothing in the build, solver, server, dashboard,
  `calibrate`, artifacts or any hash reads its output. EspressoLab stays
  deterministic; this is an optional, networked side tool like
  `tools/import_shot_telemetry.py`, and stdlib only for the same reason (the
  repo must build offline from a clean clone).
- Not a measurement. A model's reading of a taster's words is written to a
  separate `<capture-id>.tasting.jev.json` sidecar, never into the capture,
  whose rule is that anything not measured stays null. Taste is never a
  calibration target, and neither is this.
- Not a magnitude. Jev's Score answers are not numerically calibrated between
  levels (TypeSafe's "Jev 1.13 jaggedness", "Math using score"), so each axis
  is reported as its most probable named level, never the expectation scaled
  onto the overlay's 0-10. The comparison buckets the overlay's 0-10 into the
  same five named bands in code instead.

Gating, all in code: an axis the notes never mention stays null
(`not_mentioned`) rather than reading as "absent"; an answer whose confidence is
under `--min-confidence` stays null (`low_confidence`) with its probabilities
kept for inspection.

The model is pinned (`jev-1.14.0`) rather than `jev-latest` so a reading names
what produced it; the sidecar also records the model the API says answered,
the SHA-256 of the capture file and of the canonical request, and token usage.
Jev is not deterministic in the result-hash sense, so a reading identifies one
API call, not a reproducible run.

Local backend (`--backend local`): the same questions answered by a local LLM
behind any OpenAI-compatible chat endpoint -- Ollama (the default,
`http://localhost:11434/v1`), llama.cpp's `llama-server`, LM Studio -- for when
there is no Jev key or the notes must not leave the machine. It emulates the
System One answer shape, so validation, gating, the sidecar and `compare` are
unchanged. Each question is one completion of a single label token (a letter
per choice, a digit per score level, yes/no per noul) at temperature 0, and the
probabilities are the model's own token log-probabilities over the allowed
labels, renormalised -- never numbers the model was asked to write, which
small models make up. `confidence` is the top label's probability, a looser
quantity than Jev's own. The server must return `logprobs`; one that does not
is an error, not a silent fallback. A local reading is written to
`<capture-id>.tasting.local.json`, its `model` is prefixed `local:` and its
`backend` is `local`, so it can never pass for a Jev reading.

Usage:
    # Inspect the exact request without a key or any network access:
    python3 tools/jev_tasting_notes.py read \\
        --capture assets/real_shots/<id>.capture.json --dry-run

    # Call Jev (needs TYPESAFE_API_KEY; TYPESAFE_BASE_URL overrides the host):
    python3 tools/jev_tasting_notes.py read \\
        --capture assets/real_shots/<id>.capture.json

    # Or a local model (LOCAL_LLM_BASE_URL overrides the Ollama default):
    ollama pull qwen2.5:7b-instruct
    python3 tools/jev_tasting_notes.py read --backend local \\
        --capture assets/real_shots/<id>.capture.json

    # Compare the reading with a simulated shot that carries a bean:
    python3 tools/jev_tasting_notes.py compare \\
        --reading assets/real_shots/<id>.tasting.jev.json \\
        --summary <artifacts-dir>/summary.json

Exit codes: 0 success, 1 the API call or its response failed, 2 bad input or
usage (missing key, no tasting notes, unreadable file, refusing to overwrite).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from datetime import datetime, timezone
from pathlib import Path

API_KEY_ENV = "TYPESAFE_API_KEY"
BASE_URL_ENV = "TYPESAFE_BASE_URL"
DEFAULT_BASE_URL = "https://api.typesafe.ai"
ENDPOINT = "/v1/systemone"
DEFAULT_MODEL = "jev-1.14.0"
DEFAULT_MIN_CONFIDENCE = 0.5
REQUEST_TIMEOUT_S = 30.0
# 429 Too Many Requests and 529 Overloaded are the two retryable statuses
# (docs.typesafe.ai/api/errors); everything else fails on the first attempt.
RETRYABLE_STATUSES = frozenset({429, 529})
MAX_ATTEMPTS = 4

LOCAL_BASE_URL_ENV = "LOCAL_LLM_BASE_URL"
DEFAULT_LOCAL_BASE_URL = "http://localhost:11434/v1"
DEFAULT_LOCAL_MODEL = "qwen2.5:7b-instruct"
LOCAL_MODEL_PREFIX = "local:"
# Generous: the first request also loads the model into memory.
LOCAL_TIMEOUT_S = 300.0
# Enough alternatives that every allowed label shows up even when the model's
# favourite first token is a variant (" A", "a", "Yes").
LOCAL_TOP_LOGPROBS = 20
LOCAL_SYSTEM_PROMPT = (
    "You judge text for a computer program. Read the state, then reply with exactly "
    "one of the allowed labels and nothing else."
)

BACKENDS = ("typesafe", "local")
# The reading keeps Jev's kind for both backends: the sidecar format is the same,
# and `backend` says which one produced it.
READING_KIND = "jev_tasting_reading"
READING_SCHEMA_VERSION = "1.1"
READING_SUFFIXES = {"typesafe": ".tasting.jev.json", "local": ".tasting.local.json"}

EXIT_OK = 0
EXIT_API_FAILURE = 1
EXIT_BAD_INPUT = 2

# Same names and order as kSensoryAxisNames (engine/espresso_core/bean.cpp).
AXES = (
    "fruit",
    "acidity",
    "sweetness",
    "chocolate",
    "body",
    "bitterness",
    "astringency",
)

# What a taster's words have to be about for each axis. Jev reads literally, so
# each description names the vocabulary a note would actually use.
AXIS_DESCRIPTIONS = {
    "fruit": "fruit flavour (berry, citrus, stone fruit, tropical, winey or jammy notes)",
    "acidity": "acidity (brightness, tartness, sourness, sharpness)",
    "sweetness": "sweetness (sugary, caramel, honey or syrupy sweetness)",
    "chocolate": "chocolate and roast-sweet flavour (cocoa, chocolate, nutty, toasty, brown sugar)",
    "body": "body (weight and texture in the mouth: thin or watery is low, heavy or syrupy is high)",
    "bitterness": "bitterness (bitter, burnt, ashy or harsh roast flavour)",
    "astringency": "astringency (a drying, chalky, puckering, tannic or woody finish)",
}

# Five ordered levels shared by the Jev rubric and the overlay banding.
LEVELS = ("absent", "faint", "moderate", "pronounced", "dominant")
LEVEL_RUBRIC = (
    "Absent: the notes say it is missing or lacking",
    "Faint: the notes call it slight, subtle or a hint",
    "Moderate: the notes call it present, noticeable or balanced",
    "Pronounced: the notes call it strong, bold or forward",
    "Dominant: the notes say it overwhelms or defines the shot",
)
# The overlay's 0-10 axis scale cut into len(LEVELS) equal bands, top band closed.
OVERLAY_SCALE_MAX = 10.0
OVERLAY_BAND_WIDTH = OVERLAY_SCALE_MAX / len(LEVELS)

# Same names as FlavorVerdict (include/espressolab/flavor_result.hpp). The
# overlay's verdict is relative to the bean's declared target; a taster's is
# relative to their own expectation, so the comparison is indicative only.
VERDICT_CRITERIA = {
    "under_extracted_sour": "The taster describes the shot as sour, sharp, thin, salty or under-extracted",
    "balanced": "The taster describes the shot as balanced, sweet or well extracted",
    "over_extracted_bitter": "The taster describes the shot as bitter, harsh, drying, hollow or over-extracted",
}

OBSERVATION_FIELDS = ("taste", "channeling", "puck_condition", "anomalies", "notes")

Transport = Callable[[dict], dict]


class InputError(ValueError):
    """Bad input or usage. Maps to EXIT_BAD_INPUT."""


class JevApiError(RuntimeError):
    """The API call failed or answered with something unusable. Maps to EXIT_API_FAILURE."""

    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise InputError(f"cannot read {path}: {error}") from error


def build_state(capture: dict) -> dict:
    observations = capture.get("observations") or {}
    taste = observations.get("taste")
    if not isinstance(taste, str) or not taste.strip():
        raise InputError(
            f"capture {capture.get('id')!r} has no observations.taste to read "
            f"(status {capture.get('status')!r}); a pending capture never does"
        )
    # Only the taster's own words. Jev loses accuracy on large states full of
    # irrelevant detail, and the setup fields are numbers code already owns.
    state = {}
    for field in OBSERVATION_FIELDS:
        value = observations.get(field)
        if value in (None, "", []):
            continue
        state[field] = value
    return state


def build_questions() -> dict:
    questions: dict[str, dict] = {}
    for axis in AXES:
        description = AXIS_DESCRIPTIONS[axis]
        questions[f"{axis}_mentioned"] = {
            "type": "noul",
            # Not "describe it, including saying it is missing": read literally,
            # that asks whether the notes call the axis missing.
            "instructions": (
                f"The `taste` notes mention the shot's {description}, "
                "either as present or as missing."
            ),
            "criteria": {
                "true": f"The notes talk about the {axis}, whether they say it is there or that there is none.",
                "false": f"The notes do not talk about the {axis} at all.",
            },
        }
        questions[f"{axis}_level"] = {
            "type": "score",
            "instructions": f"How strong is the shot's {description}, according to the `taste` notes?",
            "criteria": list(LEVEL_RUBRIC),
        }
    questions["verdict"] = {
        "type": "choice",
        "instructions": "Which description matches how the `taste` notes judge the shot's extraction?",
        "criteria": dict(VERDICT_CRITERIA),
    }
    questions["channeling_reported"] = {
        "type": "noul",
        # Names no fields: build_state drops empty ones, and a question about a
        # field that is not there reads as false.
        "instructions": (
            "The observations report channeling: a spurt, uneven or off-centre flow, "
            "or holes or cracks in the spent puck."
        ),
        "criteria": {
            "true": "A spurt, uneven flow, or a holed or cracked puck is reported.",
            "false": "Nothing like that is reported, or the flow and puck are described as even.",
        },
    }
    return questions


def build_request(capture: dict, model: str) -> dict:
    return {
        "model": model,
        "state": build_state(capture),
        "questions": build_questions(),
    }


def canonical_sha256(document: dict) -> str:
    text = json.dumps(
        document, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def send_with_retry(
    send_once: Transport,
    request: dict,
    sleep: Callable[[float], None] = time.sleep,
) -> dict:
    """Exponential backoff on 429/529 only, as TypeSafe's error docs ask."""
    for attempt in range(1, MAX_ATTEMPTS + 1):
        try:
            return send_once(request)
        except JevApiError as error:
            if error.status not in RETRYABLE_STATUSES or attempt == MAX_ATTEMPTS:
                raise
            sleep(2.0 ** (attempt - 1))
    raise AssertionError("unreachable")


def post_json(url: str, body: dict, headers: dict[str, str], timeout_s: float) -> dict:
    http_request = urllib.request.Request(
        url,
        data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
            **headers,
        },
    )
    try:
        with urllib.request.urlopen(http_request, timeout=timeout_s) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")[:500]
        raise JevApiError(
            f"HTTP {error.code} from {url}: {detail}", status=error.code
        ) from error
    except (urllib.error.URLError, TimeoutError) as error:
        raise JevApiError(f"cannot reach {url}: {error}") from error
    except json.JSONDecodeError as error:
        raise JevApiError(f"response from {url} is not JSON: {error}") from error


def https_transport(api_key: str, base_url: str) -> Transport:
    url = base_url.rstrip("/") + ENDPOINT
    headers = {"Authorization": f"Bearer {api_key}"}
    return lambda request: post_json(url, request, headers, REQUEST_TIMEOUT_S)


def _as_text(value: object) -> str:
    return value if isinstance(value, str) else json.dumps(value, ensure_ascii=False)


def local_prompt(question: dict) -> tuple[list[str], str]:
    """The allowed labels and the question text for one System One question."""
    instructions = _as_text(question.get("instructions") or "")
    if question["type"] == "noul":
        lines = [f"Statement: {instructions}"]
        criteria = question.get("criteria") or {}
        if criteria.get("true"):
            lines.append(f"Answer yes if: {_as_text(criteria['true'])}")
        if criteria.get("false"):
            lines.append(f"Answer no if: {_as_text(criteria['false'])}")
        lines.append("Is the statement true? Answer yes or no.")
        return ["yes", "no"], "\n".join(lines)
    if question["type"] == "choice":
        names = list(question["criteria"])
        if len(names) > 26:
            raise InputError(
                f"the local backend labels choices A-Z; got {len(names)} choices"
            )
        labels = [chr(ord("A") + index) for index in range(len(names))]
        lines = [f"Question: {instructions}", "Options:"]
        for label, name in zip(labels, names):
            description = question["criteria"][name]
            lines.append(
                f"{label}. {name}: {_as_text(description)}"
                if description
                else f"{label}. {name}"
            )
        lines.append(f"Answer with one letter: {', '.join(labels)}.")
        return labels, "\n".join(lines)
    if question["type"] == "score":
        levels = question["criteria"]
        labels = [str(level) for level in range(len(levels))]
        lines = [f"Question: {instructions}", "Levels:"]
        lines += [f"{level}. {_as_text(text)}" for level, text in enumerate(levels)]
        lines.append(f"Answer with one digit from 0 to {len(levels) - 1}.")
        return labels, "\n".join(lines)
    raise InputError(f"the local backend cannot answer a {question['type']} question")


def label_distribution(completion: dict, labels: list[str], name: str) -> list[float]:
    """The completion's first-token probabilities over `labels`, renormalised.

    Variants of a label (" A", "a", "Yes") count towards it; everything else the
    model might have said is dropped before renormalising.
    """
    try:
        first = completion["choices"][0]["logprobs"]["content"][0]
        alternatives = first.get("top_logprobs") or [first]
    except (KeyError, IndexError, TypeError) as error:
        raise JevApiError(
            f"the local server returned no logprobs for {name!r}; the local backend "
            "needs an OpenAI-compatible server that supports logprobs"
        ) from error
    index = {label.lower(): position for position, label in enumerate(labels)}
    mass = [0.0] * len(labels)
    for alternative in alternatives:
        position = index.get(str(alternative.get("token", "")).strip().lower())
        logprob = alternative.get("logprob")
        if position is not None and isinstance(logprob, (int, float)):
            mass[position] += math.exp(logprob)
    total = sum(mass)
    if not total > 0.0:
        said = first.get("token")
        raise JevApiError(
            f"the local model answered {said!r} to {name!r}, none of {labels}"
        )
    return [value / total for value in mass]


def local_answer(question: dict, distribution: list[float]) -> dict:
    """A System One answer built from a label distribution."""
    if question["type"] == "noul":
        return {"type": "noul", "noul": distribution[0]}
    if question["type"] == "choice":
        names = list(question["criteria"])
        probabilities = dict(zip(names, distribution))
        return {
            "type": "choice",
            "choice": _most_probable(probabilities),
            "confidence": max(distribution),
            "probabilities": probabilities,
        }
    levels = question["criteria"]
    return {
        "type": "score",
        "score": sum(level * p for level, p in enumerate(distribution)),
        "confidence": max(distribution),
        "legend": {str(level): text for level, text in enumerate(levels)},
        "probabilities": {str(level): p for level, p in enumerate(distribution)},
    }


def local_llm_transport(
    base_url: str, model: str, post: Callable[[dict], dict] | None = None
) -> Transport:
    """Answer a System One request with a local OpenAI-compatible chat model."""
    url = base_url.rstrip("/") + "/chat/completions"
    if post is None:

        def post(body: dict) -> dict:
            return post_json(url, body, {}, LOCAL_TIMEOUT_S)

    def send_once(request: dict) -> dict:
        # The state leads every prompt so servers with prefix caching (Ollama,
        # llama-server) only process it once across the questions.
        state = "State:\n" + json.dumps(request["state"], ensure_ascii=False, indent=2)
        answers: dict[str, dict] = {}
        usage = {"input_tokens": 0, "output_tokens": 0}
        answered_by = model
        for name, question in request["questions"].items():
            labels, text = local_prompt(question)
            completion = post(
                {
                    "model": model,
                    "messages": [
                        {"role": "system", "content": LOCAL_SYSTEM_PROMPT},
                        {"role": "user", "content": f"{state}\n\n{text}"},
                    ],
                    "max_tokens": 1,
                    "temperature": 0,
                    "seed": 0,
                    "logprobs": True,
                    "top_logprobs": LOCAL_TOP_LOGPROBS,
                }
            )
            answers[name] = local_answer(
                question, label_distribution(completion, labels, name)
            )
            answered_by = completion.get("model") or answered_by
            reported = completion.get("usage") or {}
            usage["input_tokens"] += int(reported.get("prompt_tokens") or 0)
            usage["output_tokens"] += int(reported.get("completion_tokens") or 0)
        return {
            "model": LOCAL_MODEL_PREFIX + answered_by,
            "answers": answers,
            "usage": usage,
        }

    return send_once


def _probabilities(answer: dict, name: str, keys: list[str]) -> dict[str, float]:
    probabilities = answer.get("probabilities")
    if not isinstance(probabilities, dict) or sorted(probabilities) != sorted(keys):
        raise JevApiError(
            f"answer {name!r} has probabilities for {probabilities!r}, expected {keys}"
        )
    for key, value in probabilities.items():
        if not _is_unit_interval(value):
            raise JevApiError(f"answer {name!r} probability {key!r} is {value!r}")
    return probabilities


def _is_unit_interval(value: object) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and 0.0 <= value <= 1.0
    )


def validate_response(request: dict, response: dict) -> dict:
    """Check every question came back with its own type and sane numbers."""
    answers = response.get("answers") if isinstance(response, dict) else None
    if not isinstance(answers, dict):
        raise JevApiError("response has no answers object")
    if not isinstance(response.get("model"), str):
        raise JevApiError("response has no model name")
    for name, question in request["questions"].items():
        answer = answers.get(name)
        if not isinstance(answer, dict) or answer.get("type") != question["type"]:
            raise JevApiError(
                f"answer {name!r} is missing or is not a {question['type']}"
            )
        if question["type"] == "noul":
            if not _is_unit_interval(answer.get("noul")):
                raise JevApiError(f"answer {name!r} noul is {answer.get('noul')!r}")
            continue
        if not _is_unit_interval(answer.get("confidence")):
            raise JevApiError(
                f"answer {name!r} confidence is {answer.get('confidence')!r}"
            )
        if question["type"] == "score":
            keys = [str(level) for level in range(len(question["criteria"]))]
        else:
            keys = list(question["criteria"])
        _probabilities(answer, name, keys)
        if (
            question["type"] == "choice"
            and answer.get("choice") not in question["criteria"]
        ):
            raise JevApiError(f"answer {name!r} chose {answer.get('choice')!r}")
    return answers


def _most_probable(probabilities: dict[str, float]) -> str:
    # Ties go to the first key in rubric/criteria order, so the pick is stable.
    return max(probabilities, key=lambda key: probabilities[key])


def interpret_axis(mentioned: dict, level: dict, min_confidence: float) -> dict:
    level_probabilities = {
        LEVELS[int(key)]: value
        for key, value in sorted(
            level["probabilities"].items(), key=lambda kv: int(kv[0])
        )
    }
    reading = {
        "status": "read",
        "level": None,
        "mentioned_probability": mentioned["noul"],
        "confidence": level["confidence"],
        "level_probabilities": level_probabilities,
    }
    if mentioned["noul"] < 0.5:
        reading["status"] = "not_mentioned"
    elif level["confidence"] < min_confidence:
        reading["status"] = "low_confidence"
    else:
        reading["level"] = _most_probable(level_probabilities)
    return reading


def interpret_verdict(answer: dict, min_confidence: float) -> dict:
    confident = answer["confidence"] >= min_confidence
    return {
        "status": "read" if confident else "low_confidence",
        "value": answer["choice"] if confident else None,
        "confidence": answer["confidence"],
        "probabilities": answer["probabilities"],
    }


def build_reading(
    capture: dict,
    capture_bytes: bytes,
    request: dict,
    response: dict,
    min_confidence: float,
    retrieved_at: str,
) -> dict:
    answers = validate_response(request, response)
    return {
        "schema_version": READING_SCHEMA_VERSION,
        "kind": READING_KIND,
        "backend": (
            "local" if request["model"].startswith(LOCAL_MODEL_PREFIX) else "typesafe"
        ),
        "capture_id": capture.get("id"),
        "capture_sha256": hashlib.sha256(capture_bytes).hexdigest(),
        "request_sha256": canonical_sha256(request),
        "model_requested": request["model"],
        "model": response["model"],
        "retrieved_at": retrieved_at,
        "min_confidence": min_confidence,
        "usage": response.get("usage"),
        "interpretation": (
            "A model's reading of free-text tasting notes. Not a measurement, never a "
            "calibration target, and not read by the solver, server, dashboard or any hash."
        ),
        "axes": {
            axis: interpret_axis(
                answers[f"{axis}_mentioned"], answers[f"{axis}_level"], min_confidence
            )
            for axis in AXES
        },
        "verdict": interpret_verdict(answers["verdict"], min_confidence),
        "channeling_reported_probability": answers["channeling_reported"]["noul"],
        "answers": answers,
    }


def overlay_level(intensity: float) -> str:
    if not isinstance(intensity, (int, float)) or not math.isfinite(intensity):
        raise InputError(f"overlay intensity {intensity!r} is not a finite number")
    if not 0.0 <= intensity <= OVERLAY_SCALE_MAX:
        raise InputError(
            f"overlay intensity {intensity} is outside 0-{OVERLAY_SCALE_MAX:g}"
        )
    return LEVELS[min(int(intensity // OVERLAY_BAND_WIDTH), len(LEVELS) - 1)]


def compare(reading: dict, summary: dict) -> list[str]:
    if reading.get("kind") != READING_KIND:
        raise InputError(
            f"reading kind is {reading.get('kind')!r}, expected {READING_KIND!r}"
        )
    flavor = summary.get("flavor")
    if not isinstance(flavor, dict):
        raise InputError(
            "summary has no flavor block: simulate a recipe that carries a bean"
        )
    lines = [
        f"capture   {reading.get('capture_id')}   (model {reading.get('model')})",
        (
            f"overlay   bean {flavor.get('bean_id')} {flavor.get('bean_version')}, "
            f"{flavor.get('flavor_model_version')}"
        ),
        "",
        f"{'axis':<13}{'overlay 0-10':>13}  {'overlay level':<14}{'tasted level':<15}{'steps':>5}",
    ]
    agreed = compared = 0
    for axis in AXES:
        intensity = flavor["axes"][axis]["intensity"]
        predicted = overlay_level(intensity)
        tasted = reading["axes"][axis]
        if tasted["level"] is None:
            tasted_text, steps = f"({tasted['status']})", ""
        else:
            tasted_text = tasted["level"]
            difference = LEVELS.index(predicted) - LEVELS.index(tasted["level"])
            steps = f"{difference:+d}"
            compared += 1
            agreed += difference == 0
        lines.append(
            f"{axis:<13}{intensity:>13.1f}  {predicted:<14}{tasted_text:<15}{steps:>5}"
        )
    tasted_verdict = reading["verdict"]["value"] or f"({reading['verdict']['status']})"
    lines += [
        "",
        f"verdict   overlay {flavor.get('verdict')}   tasted {tasted_verdict}",
        f"agreement {agreed}/{compared} axes on the same level"
        + (f"; {len(AXES) - compared} not compared" if compared < len(AXES) else ""),
        (
            "steps = overlay level minus tasted level. Indicative only: the bands are a coarse "
            "bucketing of the overlay's 0-10 scale, and the overlay's verdict is relative to the "
            "bean's target while the taster's is not."
        ),
    ]
    return lines


def default_reading_path(capture_path: Path, capture: dict, backend: str) -> Path:
    stem = capture.get("id") or capture_path.name.removesuffix(".capture.json")
    return capture_path.with_name(f"{stem}{READING_SUFFIXES[backend]}")


def command_read(args: argparse.Namespace, transport: Transport | None) -> int:
    capture_bytes = _read_bytes(args.capture)
    try:
        capture = json.loads(capture_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise InputError(f"cannot read {args.capture}: {error}") from error
    if not 0.0 <= args.min_confidence <= 1.0:
        raise InputError(
            f"--min-confidence must be within 0-1, got {args.min_confidence}"
        )
    local = args.backend == "local"
    model = args.model or (DEFAULT_LOCAL_MODEL if local else DEFAULT_MODEL)
    request = build_request(capture, LOCAL_MODEL_PREFIX + model if local else model)

    if args.dry_run:
        print(json.dumps(request, indent=2, ensure_ascii=False))
        return EXIT_OK

    out = args.out or default_reading_path(args.capture, capture, args.backend)
    if "measured_shots" in out.resolve().parts:
        # calibrate reads every .json there as a measured shot.
        raise InputError(f"refusing to write a reading into {out.parent}")
    if out.exists() and not args.force:
        raise InputError(f"{out} exists; pass --force to replace it")

    if transport is None and local:
        transport = local_llm_transport(
            os.environ.get(LOCAL_BASE_URL_ENV, DEFAULT_LOCAL_BASE_URL), model
        )
    if transport is None:
        api_key = os.environ.get(API_KEY_ENV)
        if not api_key:
            raise InputError(
                f"{API_KEY_ENV} is not set (use --dry-run to inspect the request without one)"
            )
        transport = https_transport(
            api_key, os.environ.get(BASE_URL_ENV, DEFAULT_BASE_URL)
        )

    response = send_with_retry(transport, request)
    retrieved_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    reading = build_reading(
        capture, capture_bytes, request, response, args.min_confidence, retrieved_at
    )
    out.write_text(
        json.dumps(reading, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )

    print(f"reading   {out}")
    print(f"model     {reading['model']}   tokens {reading['usage']}")
    for axis in AXES:
        entry = reading["axes"][axis]
        print(f"  {axis:<13}{entry['level'] or '(' + entry['status'] + ')'}")
    print(
        f"  {'verdict':<13}{reading['verdict']['value'] or '(' + reading['verdict']['status'] + ')'}"
    )
    print(f"  {'channeling':<13}p={reading['channeling_reported_probability']:.2f}")
    return EXIT_OK


def command_compare(args: argparse.Namespace) -> int:
    for line in compare(load_json(args.reading), load_json(args.summary)):
        print(line)
    return EXIT_OK


def _read_bytes(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise InputError(f"cannot read {path}: {error}") from error


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    read = commands.add_parser(
        "read", help="read a capture's tasting notes with Jev or a local model"
    )
    read.add_argument(
        "--capture", type=Path, required=True, help="a *.capture.json record"
    )
    read.add_argument(
        "--backend",
        choices=BACKENDS,
        default="typesafe",
        help="typesafe: the Jev API; local: an OpenAI-compatible local server "
        "such as Ollama (default: typesafe)",
    )
    read.add_argument(
        "--out",
        type=Path,
        help="reading path (default: <capture-id>"
        f"{READING_SUFFIXES['typesafe']} or {READING_SUFFIXES['local']} beside it)",
    )
    read.add_argument(
        "--model",
        help=f"model id (default: {DEFAULT_MODEL}, or {DEFAULT_LOCAL_MODEL} for local)",
    )
    read.add_argument(
        "--min-confidence",
        type=float,
        default=DEFAULT_MIN_CONFIDENCE,
        help=f"answers below this confidence stay null (default: {DEFAULT_MIN_CONFIDENCE})",
    )
    read.add_argument(
        "--dry-run", action="store_true", help="print the request; no key, no network"
    )
    read.add_argument(
        "--force", action="store_true", help="replace an existing reading"
    )

    comparison = commands.add_parser(
        "compare", help="line a reading up against a simulated shot"
    )
    comparison.add_argument(
        "--reading",
        type=Path,
        required=True,
        help=f"a *{READING_SUFFIXES['typesafe']} or *{READING_SUFFIXES['local']} file",
    )
    comparison.add_argument(
        "--summary",
        type=Path,
        required=True,
        help="summary.json from simulate with a bean",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None, transport: Transport | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "read":
            return command_read(args, transport)
        return command_compare(args)
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_BAD_INPUT
    except JevApiError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_API_FAILURE


if __name__ == "__main__":
    sys.exit(main())
