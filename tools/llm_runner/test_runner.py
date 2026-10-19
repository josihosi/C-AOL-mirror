"""Final-response contract at the real Ollama runner boundary; no inference."""
import argparse
import asyncio
import importlib
import socket
import io
import hashlib
import http.server
import os
import subprocess
import threading
import json
from types import SimpleNamespace
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runner


class RunnerFinalResponseTest(unittest.TestCase):
    def test_api_stdio_forwards_controls_and_refuses_empty_or_unsupported_content(self):
        captured = []
        replies = iter([
            SimpleNamespace(choices=[SimpleNamespace(message=SimpleNamespace(content=None))]),
            {"choices": [{"message": {"content": ""}}]},
            SimpleNamespace(choices=[SimpleNamespace(message=SimpleNamespace(content="Ready.|idle"))]),
        ])

        def complete(**kwargs):
            captured.append(kwargs)
            return next(replies)

        requests = [{"request_id": str(i), "prompt": "unchanged prompt", "max_tokens": 256,
                     "temperature": 0, "top_p": 0.75, "repetition_penalty": 1.0}
                    for i in range(3)]
        requests.append(dict(requests[0], request_id="unsupported", repetition_penalty=1.2))
        output = io.StringIO()
        args = argparse.Namespace(api_provider="openai", api_model="fixture", api_key_env="")
        with patch.dict(sys.modules, {"any_llm": SimpleNamespace(completion=complete)}), \
             patch.object(sys, "stdin", io.StringIO("\n".join(map(json.dumps, requests)))), \
             patch.object(sys, "stdout", output):
            self.assertEqual(runner.run_api_mode(args, None), 0)
        rows = list(map(json.loads, output.getvalue().splitlines()))
        self.assertEqual([r["ok"] for r in rows], [False, False, True, False])
        self.assertEqual([r["text"] for r in rows[:3]], ["", "", "Ready.|idle"])
        self.assertTrue(all("no usable text" in r["error"] for r in rows[:2]))
        self.assertIn("unsupported", rows[3]["error"])
        self.assertEqual(len(captured), 3)
        for kwargs in captured:
            self.assertEqual(kwargs["messages"], [{"role": "user", "content": "unchanged prompt"}])
            self.assertNotIn("max_tokens", kwargs)
            self.assertNotIn("temperature", kwargs)
            self.assertNotIn("top_p", kwargs)
            self.assertNotIn("repetition_penalty", kwargs)
            self.assertNotIn("frequency_penalty", kwargs)
            self.assertNotIn("reasoning_effort", kwargs)
        for row in rows[:3]:
            self.assertEqual(row["metrics"]["generation_settings"],
                             {})
            self.assertEqual(row["metrics"]["sampling"], "provider_default")
            self.assertEqual(row["metrics"]["ignored_sampling_settings"],
                             {"temperature": 0, "top_p": 0.75})
            self.assertEqual(row["metrics"]["unsupported_settings"]["repetition_penalty"],
                             {"requested": 1.0, "supported": False, "applied": False, "neutral": True})

    def test_installed_openai_adapter_omits_sampling_and_output_caps_without_network(self):
        try:
            from openai import AsyncOpenAI
            import any_llm
        except ImportError:
            self.skipTest("optional any-llm API dependency is not installed")
        captured = []

        probe = AsyncOpenAI(api_key="fixture-no-network")
        client_type = type(probe._client)
        asyncio.run(probe.close())

        async def send(client, request, *args, **kwargs):
            captured.append(json.loads(request.content))
            transport = importlib.import_module(type(request).__module__.split(".")[0])
            return transport.Response(200, request=request, json={
                "id": "fixture", "object": "chat.completion", "created": 0, "model": "fixture",
                "choices": [{"index": 0, "finish_reason": "length" if len(captured) == 1 else "stop",
                             "message": {"role": "assistant", "content":
                                         "" if len(captured) == 1 else "Ready.|idle"}}],
                "usage": {"prompt_tokens": 100, "completion_tokens": 256, "total_tokens": 356,
                          "completion_tokens_details": {"reasoning_tokens": 250}}})

        request = {"request_id": "wire", "prompt": "unchanged prompt", "max_tokens": 256,
                   "temperature": 0.6, "top_p": 0.9, "repetition_penalty": 1.0}
        args = argparse.Namespace(api_provider="openai", api_model="gpt-6-luna",
                                  api_key_env="COMPANION_FIXTURE_API_KEY")
        output = io.StringIO()
        # Intercept the actual SDK client (recent OpenAI versions use httpx2).
        # Forbid network independently so a changed transport cannot escape the fixture.
        with patch.object(client_type, "send", send), \
             patch.object(socket, "getaddrinfo", side_effect=AssertionError("network forbidden")), \
             patch.object(socket.socket, "connect", side_effect=AssertionError("network forbidden")), \
             patch.object(socket.socket, "connect_ex", side_effect=AssertionError("network forbidden")), \
             patch.dict(os.environ, {"COMPANION_FIXTURE_API_KEY": "fixture-no-network"}), \
             patch.object(sys, "stdin", io.StringIO(json.dumps(request) + "\n" +
                                                  json.dumps(dict(request, request_id="after", max_tokens=128)))), \
             patch.object(sys, "stdout", output):
            self.assertEqual(runner.run_api_mode(args, None), 0)
        empty, reply = map(json.loads, output.getvalue().splitlines())
        self.assertFalse(empty["ok"])
        self.assertEqual(empty["text"], "")
        self.assertIn("no usable text", empty["error"])
        self.assertTrue(reply["ok"], reply)
        self.assertEqual(reply["text"], "Ready.|idle")
        self.assertEqual(empty["metrics"]["completion"]["finish_reason"], "length")
        self.assertEqual(empty["metrics"]["completion"]["usage"]["completion_tokens_details"]["reasoning_tokens"], 250)
        self.assertEqual(len(captured), 2)
        self.assertTrue(all("max_completion_tokens" not in body and "max_tokens" not in body
                            for body in captured))
        for body in captured:
            self.assertEqual(body["model"], "gpt-6-luna")
            self.assertEqual(body["messages"], [{"role": "user", "content": "unchanged prompt"}])
            for name in ("temperature", "top_p", "repetition_penalty", "frequency_penalty", "reasoning_effort"):
                self.assertNotIn(name, body)

    def test_api_defaults_do_not_depend_on_model_or_provider(self):
        for provider, model in (("openai", "gpt-4.1-mini"), ("mistral", "fixture")):
            with self.subTest(provider=provider, model=model):
                request = {"request_id": "defaults", "prompt": "unchanged prompt", "max_tokens": 128}
                output = io.StringIO()
                args = argparse.Namespace(api_provider=provider, api_model=model, api_key_env="")
                complete = unittest.mock.Mock(return_value={"choices": [{"message": {"content": "Ready.|idle"}}]})
                with patch.dict(sys.modules, {"any_llm": SimpleNamespace(completion=complete)}), \
                     patch.object(sys, "stdin", io.StringIO(json.dumps(request))), \
                     patch.object(sys, "stdout", output):
                    self.assertEqual(runner.run_api_mode(args, None), 0)
                complete.assert_called_once_with(model=model, provider=provider,
                                                messages=[{"role": "user", "content": "unchanged prompt"}],
                                                api_key="")
                row = json.loads(output.getvalue())
                self.assertTrue(row["ok"])
                self.assertEqual(row["metrics"]["sampling"], "provider_default")
                self.assertEqual(row["metrics"]["ignored_sampling_settings"], {})

    def test_stdio_reaches_existing_sanitizer_and_recovers_next_request(self):
        captured = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                data = self.rfile.read(int(self.headers["Content-Length"]))
                captured.append(json.loads(data))
                body = json.dumps({"response": "Ready.|idle", "thinking": ""}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        # Cross the text decoder's buffer boundary with valid multibyte text.
        valid_prompt = "inventory " + "é界🙂" * 1200 + " replacement: \ufffd"
        valid = json.dumps({"request_id": "valid", "prompt": valid_prompt,
                            "max_tokens": 9}, ensure_ascii=False).encode() + b"\n"
        malformed = b'{"request_id":"bytes","prompt":"bag\xe2 \xba","max_tokens":7}\n'
        invalid_json = b'{"request_id":"json",broken}\n'
        after = b'{"request_id":"after","prompt":"after"}\n'
        expected = runner.read_request(malformed.decode("utf-8", "surrogateescape"))
        with http.server.HTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever,
                                      kwargs={"poll_interval": 0.01})
            thread.start()
            try:
                result = subprocess.run(
                    [sys.executable, runner.__file__, "--backend", "ollama",
                     "--ollama-url", f"http://127.0.0.1:{server.server_port}",
                     "--ollama-model", "transport-fixture"],
                    input=valid + malformed + invalid_json + after,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    env=dict(os.environ, PYTHONIOENCODING="utf-8"), timeout=15)
            finally:
                server.shutdown()
                thread.join(timeout=5)
                self.assertFalse(thread.is_alive())
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        replies = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([r["request_id"] for r in replies],
                         ["valid", "bytes", "unknown", "after"])
        self.assertEqual([r["ok"] for r in replies], [True, True, False, True])
        self.assertEqual(len(captured), 3)
        self.assertEqual(hashlib.sha256(captured[0]["prompt"].encode()).hexdigest(),
                         hashlib.sha256(valid_prompt.encode()).hexdigest())
        self.assertEqual(captured[1]["prompt"], expected["prompt"])
        self.assertEqual(captured[1]["prompt"], "bag\ufffd \ufffd")
        self.assertEqual(captured[2]["prompt"], "after")
        self.assertEqual([r["options"]["num_predict"] for r in captured], [-1, -1, -1])
        self.assertTrue(all(r["options"]["temperature"] == 0.6 for r in captured))
        self.assertTrue(all(r["think"] is False for r in captured))
        self.assertTrue(all(r["model"] == "transport-fixture" for r in captured))

    def test_debug_events_bind_only_matching_run_ids_and_stay_opt_in(self):
        with patch.dict(runner.os.environ, {"OPENCLAW_HARNESS_RUN_ID": "run-a",
                                          "OPENCLAW_HARNESS_SEMANTIC_RUN_ID": "run-a"}):
            before = runner.EVENT_SEQUENCE
            runner.request_event(None, "llm_request_started", {"request_id": "req_0"})
            self.assertEqual(runner.EVENT_SEQUENCE, before)
            output = io.StringIO()
            runner.request_event(output, "llm_request_started", {"request_id": "req_0"})
            self.assertEqual(json.loads(output.getvalue())["run_id"], "run-a")
        with patch.dict(runner.os.environ, {"OPENCLAW_HARNESS_RUN_ID": "run-a",
                                          "OPENCLAW_HARNESS_SEMANTIC_RUN_ID": "run-b"}):
            output = io.StringIO()
            runner.request_event(output, "llm_request_started", {"request_id": "req_0"})
            self.assertIsNone(json.loads(output.getvalue())["run_id"])

    def test_ollama_request_explicitly_disables_thinking(self):
        raw = {"response": "Holding.|wait_here", "thinking": "diagnostic only"}
        with patch.object(runner.urllib.request, "urlopen", return_value=io.BytesIO(json.dumps(raw).encode())) as send:
            self.assertEqual(runner.ollama_generate("http://127.0.0.1:11434", "installed-model", "game snapshot", 0.6), raw)
        request = send.call_args.args[0]
        self.assertEqual(request.full_url, "http://127.0.0.1:11434/api/generate")
        self.assertEqual(json.loads(request.data), {
            "model": "installed-model", "prompt": "game snapshot", "stream": False,
            "think": False, "options": {"temperature": 0.6, "num_predict": -1},
        })

    def run_response(self, raw):
        args = argparse.Namespace(ollama_model="installed-model", ollama_url="http://127.0.0.1:11434")
        request = {"request_id": "native-request", "prompt": "game snapshot", "max_tokens": 256}
        incoming = io.StringIO(json.dumps(request) + "\n")
        outgoing, log = io.StringIO(), io.StringIO()
        with patch.object(runner.sys, "stdin", incoming), patch.object(runner.sys, "stdout", outgoing), patch.object(runner, "ollama_generate", return_value=raw):
            self.assertEqual(runner.run_ollama_mode(args, log), 0)
        result = json.loads(outgoing.getvalue())
        self.assertEqual(result["request_id"], "native-request")
        # Backend stop/count metadata survives; reasoning bodies do not.
        self.assertIn("ollama response metadata:", log.getvalue())
        reasoning = raw.get("thinking", "")
        if reasoning:
            self.assertNotIn(reasoning, log.getvalue())
        return result

    def test_lifecycle_separates_pending_emission_and_native_application(self):
        args = argparse.Namespace(ollama_model="installed-model", ollama_url="http://127.0.0.1:11434")
        request = {"request_id": "req_42", "prompt": "unique private utterance", "max_tokens": 256}
        incoming, outgoing, log = io.StringIO(json.dumps(request) + "\n"), io.StringIO(), io.StringIO()
        def events():
            return [json.loads(line) for line in log.getvalue().splitlines() if line.startswith("{")]
        def calculate(*args):
            self.assertEqual([event["event"] for event in events()], ["llm_request_started"])
            self.assertEqual(outgoing.getvalue(), "")
            return {"response": "Ready.|wait_here", "thinking": ""}
        with patch.object(runner.sys, "stdin", incoming), patch.object(runner.sys, "stdout", outgoing), patch.object(runner, "ollama_generate", side_effect=calculate):
            self.assertEqual(runner.run_ollama_mode(args, log), 0)
        started, completed = events()
        self.assertEqual(completed["event"], "llm_response_emitted")
        self.assertEqual(completed["request_id"], started["request_id"])
        self.assertEqual(completed["runner_pid"], started["runner_pid"])
        self.assertEqual(started["prompt_sha256"], hashlib.sha256(request["prompt"].encode()).hexdigest())
        self.assertEqual(completed["response_sha256"], hashlib.sha256(outgoing.getvalue().strip().encode()).hexdigest())
        self.assertNotIn("applied", completed)
        # Detailed game evidence is retained for exact utterance queries; it is
        # still scoped to this runner process and proves emission, not application.
        self.assertEqual(started["payload"], request)
        self.assertEqual(completed["payload"], json.loads(outgoing.getvalue()))
        self.assertEqual(started["process_instance"], completed["process_instance"])
        self.assertGreater(completed["sequence"], started["sequence"])

    def test_failed_emission_does_not_log_completion_and_error_is_not_success(self):
        log = io.StringIO()
        class Broken(io.StringIO):
            def flush(self):
                raise BrokenPipeError("native consumer exited")
        with patch.object(runner.sys, "stdout", Broken()):
            with self.assertRaises(BrokenPipeError):
                runner.write_response({"request_id": "req_1", "ok": True}, log_fp=log)
        self.assertEqual(log.getvalue(), "")
        with patch.object(runner.sys, "stdout", io.StringIO()):
            runner.write_response({"request_id": "req_2", "ok": False, "error": "backend failed"}, log_fp=log)
        self.assertIs(json.loads(log.getvalue())["ok"], False)

    def test_runner_rejects_thought_only_without_logging_reasoning(self):
        for raw in (
            {"response": "<|channel>thought\nThinking Process: follow_close", "done_reason": "length"},
            {"response": "<think>Thinking Process: follow_close", "done_reason": "length"},
            {"response": "", "thinking": "Reasoning without an answer", "done_reason": "length"},
            {"response": "<|channel>thought\nOnly reasoning<channel|>"},
        ):
            with self.subTest(raw=raw):
                result = self.run_response(raw)
                self.assertFalse(result["ok"])
                self.assertEqual(result["text"], "")
                self.assertIn("no acceptable final text", result["error"])

    def test_runner_keeps_final_field_and_supported_plain_formats(self):
        for text in ("Holding here.|hold_position", "Speech|bogus follow_close", "Moving+move=-20,+20 wait_here"):
            with self.subTest(text=text):
                result = self.run_response({"response": text, "thinking": ""})
                self.assertTrue(result["ok"])
                self.assertEqual(result["text"], text)

    def test_self_test_uses_same_nonthinking_final_text_contract(self):
        args = argparse.Namespace(ollama_model="installed-model", ollama_url="http://127.0.0.1:11434", self_test_prompt="fixture", max_tokens=256)
        for raw, expected in (({"response": "Ready"}, 0),
                              ({"response": "Ready", "thinking": "leak"}, 0),
                              ({"response": "<|channel>thought\nunfinished"}, 1)):
            with self.subTest(raw=raw), patch.object(runner, "ollama_generate", return_value=raw), patch.object(runner.sys, "stdout", io.StringIO()), patch.object(runner.sys, "stderr", io.StringIO()):
                self.assertEqual(runner.run_ollama_self_test(args, io.StringIO()), expected)

    def test_reasoning_prefix_and_structured_reasoning_keep_only_final(self):
        for raw in (
            {"response": "<think>SECRET_THOUGHT|attack=a|drop:item_1</think>Holding.|wait_here"},
            {"response": "<|channel>thought\nSECRET_THOUGHT|panic_on<channel|>Holding.|wait_here"},
            {"response": "Holding.|wait_here", "thinking": "SECRET_THOUGHT|attack=a"},
        ):
            with self.subTest(raw=raw):
                result = self.run_response(raw)
                self.assertTrue(result["ok"], result)
                self.assertEqual(result["text"], "Holding.|wait_here")

    def test_api_metadata_keeps_only_nonsecret_stop_usage_and_presence(self):
        request = {"request_id": "metadata", "prompt": "unchanged", "max_tokens": 1}
        replies = [
            {"choices": [{"finish_reason": "length", "message": {
                "content": None, "refusal": "PRIVATE_REFUSAL", "tool_calls": [{"arguments": "PRIVATE_TOOL"}]
            }}], "usage": {"completion_tokens": 477, "prompt_tokens": 100, "total_tokens": 577,
                           "completion_tokens_details": {"reasoning_tokens": 468, "private": "PRIVATE_USAGE"}}},
            {"choices": [{"finish_reason": "stop", "message": {"content": [
                {"type": "reasoning", "text": "PRIVATE_REASONING|attack=a"},
                {"type": "output_text", "text": "Ready.|idle"}
            ]}}]},
        ]
        output, log = io.StringIO(), io.StringIO()
        args = argparse.Namespace(api_provider="openai", api_model="fixture", api_key_env="")
        with patch.dict(sys.modules, {"any_llm": SimpleNamespace(completion=unittest.mock.Mock(side_effect=replies))}), \
             patch.object(sys, "stdin", io.StringIO("\n".join(json.dumps(request) for _ in replies))), \
             patch.object(sys, "stdout", output):
            self.assertEqual(runner.run_api_mode(args, log), 0)
        empty, final = map(json.loads, output.getvalue().splitlines())
        self.assertFalse(empty["ok"])
        meta = empty["metrics"]["completion"]
        self.assertEqual(meta["finish_reason"], "length")
        self.assertEqual(meta["usage"]["completion_tokens_details"], {"reasoning_tokens": 468})
        self.assertIsNone(meta["content_chars"])
        self.assertEqual(meta["content_type"], "NoneType")
        self.assertTrue(meta["content_present"])
        self.assertTrue(meta["refusal_present"])
        self.assertTrue(meta["tool_calls_present"])
        self.assertEqual(final["text"], "Ready.|idle")
        self.assertIsNone(final["metrics"]["completion"]["usage"])
        for secret in ("PRIVATE_REFUSAL", "PRIVATE_TOOL", "PRIVATE_USAGE", "PRIVATE_REASONING"):
            self.assertNotIn(secret, log.getvalue() + output.getvalue())

    def test_openvino_caller_uses_sdk_unlimited_sentinels_and_final_suffix(self):
        size_max = (1 << (8 * __import__("struct").calcsize("P"))) - 1
        config = SimpleNamespace(max_new_tokens=size_max, max_length=size_max)
        sdk = SimpleNamespace(GenerationConfig=lambda: config)
        for raw, expected in (
            ("<think>PRIVATE|panic_on</think>Ready.|wait_here", "Ready.|wait_here"),
            ("<think>PRIVATE|panic_on", ""),
            (" ".join(["word"] * 300), " ".join(["word"] * 300)),
        ):
            with self.subTest(raw=raw):
                pipe = SimpleNamespace(generate=unittest.mock.Mock(return_value=raw))
                with patch.dict(sys.modules, {"openvino_genai": sdk}):
                    row = runner.handle_request(pipe, None, {"request_id": "local", "prompt": "unchanged", "max_tokens": 1},
                                                build_time_ms=0, total_load_time_ms=0, log_fp=None)
                self.assertEqual(row["text"], expected)
                self.assertEqual(row["ok"], bool(expected))
                kwargs = pipe.generate.call_args.kwargs
                self.assertEqual(kwargs["max_new_tokens"], size_max)
                self.assertEqual(kwargs["max_length"], size_max)
                self.assertEqual(kwargs["temperature"], .6)

    def test_summary_backends_have_no_application_cap_or_reasoning_recovery(self):
        import background_summarizer as summary
        size_max = (1 << (8 * __import__("struct").calcsize("P"))) - 1
        sdk = SimpleNamespace(GenerationConfig=lambda: SimpleNamespace(max_new_tokens=size_max, max_length=size_max))
        args = argparse.Namespace(retry_invalid=0, debug_io=True, debug_invalid=True,
                                  temperature=.4, top_p=.9, repetition_penalty=1., max_tokens=1)
        final = "brave, loyal, resourceful, calm, clever | We should keep moving."
        for backend in ("openvino", "ollama"):
            for raw, expected in (("<think>PRIVATE_REASONING|attack=a</think>" + final, True),
                                  ("<think>PRIVATE_REASONING|attack=a", False)):
                with self.subTest(backend=backend, valid=expected):
                    output = io.StringIO()
                    pipe = SimpleNamespace(generate=unittest.mock.Mock(return_value=raw))
                    captured = []
                    def send(request, **kwargs):
                        captured.append(json.loads(request.data))
                        return io.BytesIO(json.dumps({"response": raw, "thinking": "PRIVATE_REASONING"}).encode())
                    args.ollama_url, args.ollama_model = "http://fixture", "unchanged-model"
                    with patch.dict(sys.modules, {"openvino_genai": sdk}), \
                         patch.object(summary, "build_prompt_from_text", return_value="unchanged prompt"), \
                         patch.object(summary.urllib.request, "urlopen", side_effect=send), \
                         patch.object(sys, "stdout", output):
                        result = summary.generate_summary_text("unchanged", "fixture", args, backend, pipe)
                    self.assertEqual(bool(result[0]), expected)
                    self.assertNotIn("PRIVATE_REASONING", output.getvalue())
                    if backend == "openvino":
                        for call in pipe.generate.call_args_list:
                            self.assertEqual(call.kwargs["max_length"], size_max)
                            self.assertEqual(call.kwargs["max_new_tokens"], size_max)
                    else:
                        self.assertTrue(captured)
                        self.assertTrue(all(body["options"]["num_predict"] == -1 for body in captured))

    def test_wrapper_commands_and_payloads_ignore_retired_output_caps(self):
        import npc_harness as harness
        import prompt_playground as playground
        args = argparse.Namespace(python_path="fixture-python", runner_path="fixture-runner",
                                  backend="api", max_tokens=1, max_prompt_len=4096,
                                  log_file="", api_provider="openai", api_model="unchanged-model",
                                  api_key_env="", temperature=.6, top_p=.9, repetition_penalty=1.)
        for module, operation, request_id in (
            (harness, lambda: harness.run_runner_request(args, "unchanged", "snapshot", "wrapper"), "wrapper"),
            (playground, lambda: playground.run_runner("fixture-python", "fixture-runner", "", "CPU", 4096,
                                                     False, "", "", "unchanged", "snapshot", 1,
                                                     True, "", "openai", "unchanged-model"), "playground_1"),
        ):
            with self.subTest(wrapper=module.__name__):
                incoming = io.StringIO()
                proc = SimpleNamespace(stdin=incoming, stdout=io.StringIO(json.dumps({"request_id": request_id, "ok": True, "text": "Ready.|idle"}) + "\n"),
                                       stderr=io.StringIO(), poll=lambda: None, wait=unittest.mock.Mock(return_value=0), kill=unittest.mock.Mock())
                with patch.object(module.subprocess, "Popen", return_value=proc) as launch:
                    self.assertTrue(operation()["ok"])
                self.assertNotIn("--max-tokens", launch.call_args.args[0])
                sent = json.loads(incoming.getvalue().splitlines()[0])
                self.assertNotIn("max_tokens", sent)
                self.assertEqual(sent["prompt"], "unchanged")

    def test_empty_reasoning_delimiters_preserve_final_text(self):
        result = self.run_response({"response": "<think></think><|channel>thought\n<channel|>Holding.|wait_here"})
        self.assertTrue(result["ok"])
        self.assertEqual(result["text"], "Holding.|wait_here")
        self.assertEqual(runner.strip_think_tags("I think we should wait.|wait_here"), "I think we should wait.|wait_here")
        self.assertEqual(runner.strip_think_tags("A <thinker> survived.|idle"), "A <thinker> survived.|idle")
        self.assertEqual(runner.strip_think_tags("<channel|>secret|attack=a"), "")


if __name__ == "__main__":
    unittest.main()
