#!/usr/bin/env python3
"""Offline check for tools/jev_tasting_notes.py, the optional Jev reader for
real-shot tasting notes.

Never touches the network and needs no TYPESAFE_API_KEY: every API call goes
through a fake transport that answers with the documented /v1/systemone
response shape (docs.typesafe.ai/api/system-one). What it pins down:

  - the request carries only the taster's own observation fields, one
    mention Noul + one level Score per sensory axis, the verdict Choice and
    the channeling Noul, and the pinned model id
  - a capture with no tasting notes (every pending_capture, and the
    template) is refused before any request is built
  - an axis the notes never mention stays null rather than reading "absent"
  - a low-confidence answer stays null with its probabilities kept
  - an axis level is the most probable rubric level, never the Score
    expectation interpolated onto a scale
  - a response missing an answer, or with the wrong type or a non-finite
    probability, is rejected
  - only 429/529 are retried, with backoff; 401 fails on the first attempt
  - the overlay's 0-10 intensities band into the same five named levels
  - `read` writes a sidecar beside the capture, never overwrites one without
    --force, and refuses assets/measured_shots/; `--dry-run` needs no key
  - `compare` lines a reading up against a real `simulate` summary shape
  - the local backend asks one single-token, temperature-0 question per
    System One question with the state leading every prompt, merges label
    variants (" A", "Yes") from the server's top logprobs, refuses a server
    without logprobs or an answer outside the labels, and produces a response
    that passes the same validation, labelled `local:` / backend `local` and
    written to `.tasting.local.json`, never the Jev filename

Stdlib only, like tests/schemas/real_shot_capture_check.py:

    python3 tests/tools/jev_tasting_notes_check.py
"""

from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import math
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location(
    "jev_tasting_notes", ROOT / "tools" / "jev_tasting_notes.py"
)
jev = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(jev)

TASTE = "Bright and juicy, lots of blueberry, but a little sour at the front. Thin body, no bitterness."


def sample_capture() -> dict:
    capture = json.loads(
        (ROOT / "assets" / "real_shots" / "TEMPLATE.capture.json").read_text()
    )
    capture["id"] = "2026-10-20-test-machine-01"
    capture["status"] = "measured"
    capture["observations"] = {
        "taste": TASTE,
        "channeling": "One spurt from the left side at 8 s",
        "puck_condition": None,
        "anomalies": [],
        "notes": "",
    }
    return capture


def score_answer(probabilities: list[float], confidence: float) -> dict:
    return {
        "type": "score",
        "score": sum(level * p for level, p in enumerate(probabilities)),
        "confidence": confidence,
        "legend": {str(level): text for level, text in enumerate(jev.LEVEL_RUBRIC)},
        "probabilities": {str(level): p for level, p in enumerate(probabilities)},
    }


def fake_response(request: dict) -> dict:
    answers = {}
    for axis in jev.AXES:
        answers[f"{axis}_mentioned"] = {"type": "noul", "noul": 0.9}
        answers[f"{axis}_level"] = score_answer([0.05, 0.1, 0.7, 0.1, 0.05], 0.8)
    # Chocolate never comes up in TASTE: mentioned, but its level is a guess.
    answers["chocolate_mentioned"] = {"type": "noul", "noul": 0.1}
    # Expectation 2.2 ("moderate") but the most probable level is "dominant".
    answers["fruit_level"] = score_answer([0.45, 0.0, 0.0, 0.0, 0.55], 0.7)
    # Mentioned, but too unsure to report.
    answers["body_level"] = score_answer([0.3, 0.3, 0.2, 0.1, 0.1], 0.3)
    answers["verdict"] = {
        "type": "choice",
        "choice": "under_extracted_sour",
        "confidence": 0.75,
        "probabilities": {
            "under_extracted_sour": 0.8,
            "balanced": 0.15,
            "over_extracted_bitter": 0.05,
        },
    }
    answers["channeling_reported"] = {"type": "noul", "noul": 0.93}
    assert set(answers) == set(request["questions"]), (
        "fake response out of step with build_questions()"
    )
    return {
        "model": "jev-1.14.0",
        "answers": answers,
        "usage": {"input_tokens": 1400, "output_tokens": 60},
    }


def expect_raises(error_type: type, call) -> str | None:
    try:
        call()
    except error_type:
        return None
    except Exception as error:  # noqa: BLE001 - any other failure is the finding
        return f"raised {type(error).__name__}: {error}"
    return f"did not raise {error_type.__name__}"


def run_main(argv: list[str], transport=None) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = jev.main(argv, transport=transport)
    return code, out.getvalue(), err.getvalue()


def check_request_shape() -> list[str]:
    errors = []
    request = jev.build_request(sample_capture(), jev.DEFAULT_MODEL)
    if request["model"] != "jev-1.14.0":
        errors.append(f"model is {request['model']!r}, expected the pinned jev-1.14.0")
    if set(request["state"]) != {"taste", "channeling"}:
        errors.append(
            f"state carries {sorted(request['state'])}, expected only the non-empty observations"
        )
    expected = {
        f"{axis}_{suffix}" for axis in jev.AXES for suffix in ("mentioned", "level")
    }
    expected |= {"verdict", "channeling_reported"}
    if set(request["questions"]) != expected:
        errors.append(f"questions are {sorted(request['questions'])}")
    for name, question in request["questions"].items():
        if question["type"] == "score" and not 1 <= len(question["criteria"]) <= 10:
            errors.append(
                f"{name}: the API allows 1-10 score levels, got {len(question['criteria'])}"
            )
    if list(request["questions"]["verdict"]["criteria"]) != [
        "under_extracted_sour",
        "balanced",
        "over_extracted_bitter",
    ]:
        errors.append("verdict choices no longer mirror FlavorVerdict")
    return errors


def check_refuses_captures_without_notes() -> list[str]:
    errors = []
    for path in sorted((ROOT / "assets" / "real_shots").glob("*.capture.json")):
        capture = json.loads(path.read_text())
        if capture["observations"]["taste"] is not None:
            continue
        problem = expect_raises(
            jev.InputError, lambda c=capture: jev.build_request(c, jev.DEFAULT_MODEL)
        )
        if problem:
            errors.append(f"{path.name} has no taste notes but build_request {problem}")
    blank = sample_capture()
    blank["observations"]["taste"] = "   "
    problem = expect_raises(
        jev.InputError, lambda: jev.build_request(blank, jev.DEFAULT_MODEL)
    )
    if problem:
        errors.append(f"whitespace-only taste notes: build_request {problem}")
    return errors


def check_interpretation() -> list[str]:
    errors = []
    capture = sample_capture()
    request = jev.build_request(capture, jev.DEFAULT_MODEL)
    reading = jev.build_reading(
        capture, b"{}", request, fake_response(request), 0.5, "2026-10-20T00:00:00Z"
    )
    axes = reading["axes"]
    if (
        axes["chocolate"]["status"] != "not_mentioned"
        or axes["chocolate"]["level"] is not None
    ):
        errors.append(f"unmentioned chocolate read as {axes['chocolate']}")
    if axes["fruit"]["level"] != "dominant":
        errors.append(
            f"fruit level is {axes['fruit']['level']!r}; expected the most probable level, not the expectation"
        )
    if axes["body"]["status"] != "low_confidence" or axes["body"]["level"] is not None:
        errors.append(f"low-confidence body read as {axes['body']}")
    if axes["body"]["level_probabilities"]["absent"] != 0.3:
        errors.append("low-confidence answer dropped its probabilities")
    if axes["acidity"]["level"] != "moderate":
        errors.append(f"acidity level is {axes['acidity']['level']!r}")
    if reading["verdict"]["value"] != "under_extracted_sour":
        errors.append(f"verdict is {reading['verdict']}")
    strict = jev.build_reading(
        capture, b"{}", request, fake_response(request), 0.9, "2026-10-20T00:00:00Z"
    )
    if (
        strict["verdict"]["value"] is not None
        or strict["verdict"]["status"] != "low_confidence"
    ):
        errors.append("verdict under --min-confidence was still reported")
    if reading["request_sha256"] != jev.canonical_sha256(copy.deepcopy(request)):
        errors.append("request hash is not a function of the request alone")
    if reading["model"] != "jev-1.14.0" or reading["model_requested"] != "jev-1.14.0":
        errors.append("reading does not record the requested and answering model")
    return errors


def check_rejects_bad_responses() -> list[str]:
    errors = []
    request = jev.build_request(sample_capture(), jev.DEFAULT_MODEL)
    cases = {}
    missing = fake_response(request)
    del missing["answers"]["verdict"]
    cases["missing answer"] = missing
    wrong_type = fake_response(request)
    wrong_type["answers"]["body_mentioned"] = {
        "type": "choice",
        "choice": "x",
        "confidence": 1,
        "probabilities": {},
    }
    cases["wrong answer type"] = wrong_type
    nan = fake_response(request)
    nan["answers"]["acidity_level"]["probabilities"]["2"] = float("nan")
    cases["non-finite probability"] = nan
    unknown_choice = fake_response(request)
    unknown_choice["answers"]["verdict"]["choice"] = "delicious"
    cases["choice outside criteria"] = unknown_choice
    missing_level = fake_response(request)
    del missing_level["answers"]["sweetness_level"]["probabilities"]["4"]
    cases["score level missing"] = missing_level
    for label, response in cases.items():
        problem = expect_raises(
            jev.JevApiError, lambda r=response: jev.validate_response(request, r)
        )
        if problem:
            errors.append(f"{label}: validate_response {problem}")
    return errors


def check_retry_policy() -> list[str]:
    errors = []
    sleeps: list[float] = []

    def flaky(statuses):
        remaining = list(statuses)

        def send_once(request):
            if remaining:
                raise jev.JevApiError("busy", status=remaining.pop(0))
            return {"ok": True}

        return send_once

    if jev.send_with_retry(flaky([429, 529]), {}, sleep=sleeps.append) != {"ok": True}:
        errors.append("429 then 529 was not retried to success")
    if sleeps != [1.0, 2.0]:
        errors.append(f"backoff was {sleeps}, expected [1.0, 2.0]")
    sleeps.clear()
    problem = expect_raises(
        jev.JevApiError,
        lambda: jev.send_with_retry(flaky([401]), {}, sleep=sleeps.append),
    )
    if problem or sleeps:
        errors.append(f"401 should fail at once without sleeping ({problem or sleeps})")
    sleeps.clear()
    problem = expect_raises(
        jev.JevApiError,
        lambda: jev.send_with_retry(
            flaky([429] * jev.MAX_ATTEMPTS), {}, sleep=sleeps.append
        ),
    )
    if problem or len(sleeps) != jev.MAX_ATTEMPTS - 1:
        errors.append(
            f"retries were not capped at {jev.MAX_ATTEMPTS} attempts ({problem or sleeps})"
        )
    return errors


def check_overlay_banding() -> list[str]:
    errors = []
    for intensity, level in [
        (0.0, "absent"),
        (1.99, "absent"),
        (2.0, "faint"),
        (5.8, "moderate"),
        (7.61, "pronounced"),
        (9.35, "dominant"),
        (10.0, "dominant"),
    ]:
        if jev.overlay_level(intensity) != level:
            errors.append(
                f"overlay {intensity} banded as {jev.overlay_level(intensity)!r}, expected {level!r}"
            )
    for bad in (-0.1, 10.1, float("nan"), float("inf"), None):
        problem = expect_raises(jev.InputError, lambda b=bad: jev.overlay_level(b))
        if problem:
            errors.append(f"overlay_level({bad!r}) {problem}")
    return errors


def check_cli() -> list[str]:
    errors = []
    saved_key = os.environ.pop(jev.API_KEY_ENV, None)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            capture_path = Path(tmp) / "2026-10-20-test-machine-01.capture.json"
            capture_path.write_text(json.dumps(sample_capture()))

            code, out, _ = run_main(
                ["read", "--capture", str(capture_path), "--dry-run"]
            )
            if code != 0 or json.loads(out)["model"] != jev.DEFAULT_MODEL:
                errors.append(f"--dry-run without a key exited {code}")

            code, _, err = run_main(["read", "--capture", str(capture_path)])
            if code != jev.EXIT_BAD_INPUT or jev.API_KEY_ENV not in err:
                errors.append(
                    f"a real read without {jev.API_KEY_ENV} exited {code}: {err.strip()}"
                )

            code, _, err = run_main(
                ["read", "--capture", str(capture_path)], transport=fake_response
            )
            sidecar = Path(tmp) / "2026-10-20-test-machine-01.tasting.jev.json"
            if code != 0 or not sidecar.exists():
                errors.append(f"read exited {code} and wrote no sidecar: {err.strip()}")
            elif json.loads(sidecar.read_text())["kind"] != jev.READING_KIND:
                errors.append("sidecar has the wrong kind")
            if json.loads(capture_path.read_text()) != sample_capture():
                errors.append("read modified the capture record")

            code, _, _ = run_main(
                ["read", "--capture", str(capture_path)], transport=fake_response
            )
            if code != jev.EXIT_BAD_INPUT:
                errors.append(
                    f"second read overwrote the sidecar without --force (exit {code})"
                )
            code, _, _ = run_main(
                ["read", "--capture", str(capture_path), "--force"],
                transport=fake_response,
            )
            if code != 0:
                errors.append(f"--force did not replace the sidecar (exit {code})")

            measured = Path(tmp) / "measured_shots"
            measured.mkdir()
            code, _, _ = run_main(
                [
                    "read",
                    "--capture",
                    str(capture_path),
                    "--out",
                    str(measured / "x.json"),
                ],
                transport=fake_response,
            )
            if code != jev.EXIT_BAD_INPUT or any(measured.iterdir()):
                errors.append("read wrote into a measured_shots directory")

            def failing(request):
                raise jev.JevApiError("HTTP 401", status=401)

            code, _, _ = run_main(
                ["read", "--capture", str(capture_path), "--force"], transport=failing
            )
            if code != jev.EXIT_API_FAILURE:
                errors.append(
                    f"an API failure exited {code}, expected {jev.EXIT_API_FAILURE}"
                )

            pending = (
                ROOT
                / "assets"
                / "real_shots"
                / "2026-09-01-pending-capture-01.capture.json"
            )
            code, _, _ = run_main(["read", "--capture", str(pending), "--dry-run"])
            if code != jev.EXIT_BAD_INPUT:
                errors.append(f"the pending capture was not refused (exit {code})")

            # The flavor block shape `espressolab_cli simulate` writes to summary.json
            # (flavor_summary_to_json in engine/artifact_io/json_io.cpp).
            intensities = {
                "fruit": 6.96,
                "acidity": 5.80,
                "sweetness": 9.35,
                "chocolate": 7.61,
                "body": 4.41,
                "bitterness": 2.64,
                "astringency": 1.55,
            }
            summary_path = Path(tmp) / "summary.json"
            summary_path.write_text(
                json.dumps(
                    {
                        "flavor": {
                            "bean_id": "counter-culture-hologram",
                            "bean_version": "1.0.0",
                            "flavor_model_version": "flavor-0.1.0",
                            "verdict": "balanced",
                            "axes": {
                                a: {"intensity": v, "target": 5.0, "deviation": v - 5.0}
                                for a, v in intensities.items()
                            },
                        }
                    }
                )
            )
            code, out, err = run_main(
                ["compare", "--reading", str(sidecar), "--summary", str(summary_path)]
            )
            if code != 0:
                errors.append(f"compare exited {code}: {err.strip()}")
            else:
                if (
                    "acidity" not in out
                    or "(not_mentioned)" not in out
                    or "(low_confidence)" not in out
                ):
                    errors.append(f"compare table is missing rows or statuses:\n{out}")
                if "agreement 1/5 axes" not in out:
                    errors.append(f"compare agreement line is wrong:\n{out}")

            summary_path.write_text(json.dumps({"tds_percent": 9.1}))
            code, _, _ = run_main(
                ["compare", "--reading", str(sidecar), "--summary", str(summary_path)]
            )
            if code != jev.EXIT_BAD_INPUT:
                errors.append(f"compare against a beanless summary exited {code}")
    finally:
        if saved_key is not None:
            os.environ[jev.API_KEY_ENV] = saved_key
    return errors


def fake_chat_completion(body: dict) -> dict:
    """An OpenAI-compatible /chat/completions reply with first-token logprobs,
    shaped like Ollama's and llama-server's. Each label shows up as a variant
    (" A", "yes"/"Yes") next to a token that is no label at all."""
    prompt = body["messages"][-1]["content"]
    if prompt.endswith("Answer yes or no."):
        top = [("yes", -0.2), ("Yes", -2.5), ("no", -2.0), ("The", -4.0)]
    elif "Answer with one letter" in prompt:
        top = [(" A", -0.1), ("B", -2.6), ("c", -4.0), ("**", -3.0)]
    else:
        top = [("2", -0.4), ("1", -1.6), ("3", -2.3), ("0", -5.0), ("4", -5.0)]
    alternatives = [{"token": token, "logprob": logprob} for token, logprob in top]
    return {
        "model": body["model"],
        "choices": [
            {
                "message": {"role": "assistant", "content": top[0][0]},
                "logprobs": {
                    "content": [{**alternatives[0], "top_logprobs": alternatives}]
                },
            }
        ],
        "usage": {"prompt_tokens": 300, "completion_tokens": 1},
    }


def check_local_prompts() -> list[str]:
    errors = []
    questions = jev.build_questions()
    labels, text = jev.local_prompt(questions["fruit_mentioned"])
    if labels != ["yes", "no"] or not text.endswith("Answer yes or no."):
        errors.append(f"noul prompt labels {labels}")
    labels, text = jev.local_prompt(questions["verdict"])
    if labels != ["A", "B", "C"] or "A. under_extracted_sour:" not in text:
        errors.append(f"choice prompt labels {labels} / text:\n{text}")
    labels, text = jev.local_prompt(questions["body_level"])
    if (
        labels != ["0", "1", "2", "3", "4"]
        or "Answer with one digit from 0 to 4." not in text
    ):
        errors.append(f"score prompt labels {labels}")
    return errors


def check_local_distribution() -> list[str]:
    errors = []
    completion = fake_chat_completion(
        {"model": "m", "messages": [{"content": "Answer yes or no."}]}
    )
    yes, no = jev.label_distribution(completion, ["yes", "no"], "q")
    expected_yes = (math.exp(-0.2) + math.exp(-2.5)) / (
        math.exp(-0.2) + math.exp(-2.5) + math.exp(-2.0)
    )
    if abs(yes - expected_yes) > 1e-12 or abs(yes + no - 1.0) > 1e-12:
        errors.append(
            f"yes/no distribution {yes}, {no}; expected yes={expected_yes} with variants merged"
        )
    no_logprobs = {"model": "m", "choices": [{"message": {"content": "yes"}}]}
    problem = expect_raises(
        jev.JevApiError, lambda: jev.label_distribution(no_logprobs, ["yes", "no"], "q")
    )
    if problem:
        errors.append(f"a server without logprobs: label_distribution {problem}")
    off_label = {
        "choices": [
            {
                "logprobs": {
                    "content": [
                        {
                            "token": "Maybe",
                            "logprob": 0.0,
                            "top_logprobs": [{"token": "Maybe", "logprob": 0.0}],
                        }
                    ]
                }
            }
        ]
    }
    problem = expect_raises(
        jev.JevApiError, lambda: jev.label_distribution(off_label, ["yes", "no"], "q")
    )
    if problem:
        errors.append(f"an answer with no allowed label: label_distribution {problem}")
    return errors


def check_local_transport() -> list[str]:
    errors = []
    bodies: list[dict] = []

    def post(body):
        bodies.append(body)
        return fake_chat_completion(body)

    capture = sample_capture()
    request = jev.build_request(capture, jev.LOCAL_MODEL_PREFIX + "test-model")
    response = jev.local_llm_transport(
        "http://localhost:1/v1", "test-model", post=post
    )(request)
    try:
        reading = jev.build_reading(
            capture, b"{}", request, response, 0.5, "2026-10-20T00:00:00Z"
        )
    except jev.JevApiError as error:
        return [f"local response failed System One validation: {error}"]
    if len(bodies) != len(request["questions"]):
        errors.append(
            f"{len(bodies)} completions for {len(request['questions'])} questions"
        )
    for body in bodies:
        if (
            body["max_tokens"] != 1
            or body["temperature"] != 0
            or body["logprobs"] is not True
        ):
            errors.append(f"completion not a single greedy token with logprobs: {body}")
            break
    prefixes = {body["messages"][-1]["content"].split("\n\n")[0] for body in bodies}
    if len(prefixes) != 1 or TASTE not in next(iter(prefixes)):
        errors.append("the state does not lead every prompt identically")
    if reading["backend"] != "local" or reading["model"] != "local:test-model":
        errors.append(
            f"local reading is labelled backend={reading['backend']!r}, model={reading['model']!r}"
        )
    if (
        reading["axes"]["acidity"]["level"] != "moderate"
        or reading["verdict"]["value"] != "under_extracted_sour"
    ):
        errors.append(
            f"local answers misread: {reading['axes']['acidity']}, {reading['verdict']}"
        )
    if response["usage"] != {
        "input_tokens": 300 * len(bodies),
        "output_tokens": len(bodies),
    }:
        errors.append(f"usage not summed: {response['usage']}")
    return errors


def check_local_cli() -> list[str]:
    errors = []
    with tempfile.TemporaryDirectory() as tmp:
        capture_path = Path(tmp) / "2026-10-20-test-machine-01.capture.json"
        capture_path.write_text(json.dumps(sample_capture()))
        code, out, _ = run_main(
            ["read", "--backend", "local", "--capture", str(capture_path), "--dry-run"]
        )
        if code != 0 or json.loads(out)["model"] != "local:" + jev.DEFAULT_LOCAL_MODEL:
            errors.append(f"local --dry-run exited {code} or named the wrong model")
        transport = jev.local_llm_transport(
            "http://localhost:1/v1", jev.DEFAULT_LOCAL_MODEL, post=fake_chat_completion
        )
        code, _, err = run_main(
            ["read", "--backend", "local", "--capture", str(capture_path)],
            transport=transport,
        )
        local_sidecar = Path(tmp) / "2026-10-20-test-machine-01.tasting.local.json"
        if code != 0 or not local_sidecar.exists():
            errors.append(
                f"local read exited {code} and wrote no .tasting.local.json: {err.strip()}"
            )
        if (Path(tmp) / "2026-10-20-test-machine-01.tasting.jev.json").exists():
            errors.append("a local reading was written under the Jev filename")
    return errors


CHECKS = [
    check_request_shape,
    check_refuses_captures_without_notes,
    check_interpretation,
    check_rejects_bad_responses,
    check_retry_policy,
    check_overlay_banding,
    check_cli,
    check_local_prompts,
    check_local_distribution,
    check_local_transport,
    check_local_cli,
]


def main() -> int:
    failures = 0
    for check in CHECKS:
        errors = check()
        name = check.__name__.removeprefix("check_")
        if errors:
            failures += 1
            for error in errors:
                print(f"FAIL {name}: {error}")
        else:
            print(f"PASS {name}")
    if failures:
        print(f"\n{failures} of {len(CHECKS)} check(s) failed")
        return 1
    print(f"\nOK: {len(CHECKS)} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
