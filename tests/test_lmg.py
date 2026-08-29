#!/usr/bin/env python3
import http.server
import json
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest


BIN = pathlib.Path(os.environ.get("LMG_BIN", pathlib.Path(__file__).parents[1] / "lmg"))
REPO = pathlib.Path(__file__).parents[1].resolve()
DISCONNECT = object()


class MockAPI:
    def __init__(self, responses):
        self.responses = iter(responses)
        self.requests = []
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                owner.requests.append((self.headers, json.loads(body)))
                response = next(owner.responses)
                if response is DISCONNECT:
                    self.close_connection = True
                    try:
                        self.connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    self.connection.close()
                    return
                status = 200
                if isinstance(response, tuple):
                    status, response = response
                encoded = json.dumps(response).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def endpoint(self):
        return f"http://127.0.0.1:{self.server.server_port}/v1/chat/completions"

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


def response(message, usage=None):
    result = {"choices": [{"message": message}]}
    if usage is not None:
        result["usage"] = usage
    return result


def codemap(summary="compact answer"):
    return {
        "summary": summary,
        "nodes": [{
            "id": "agent-loop",
            "location": "lmg.c:1495-1603",
            "symbol": "run_agent",
            "description": "Runs tool calls and emits the completed result.",
        }],
        "edges": [],
    }


def codemap_output(value, usage=None):
    result = dict(value)
    result["usage"] = usage if usage is not None else {
        "input": 0,
        "output": 0,
        "cache_read": 0,
        "cache_create": 0,
    }
    return result


def finish_message(value=None, call_id="finish-call"):
    return {
        "role": "assistant",
        "content": None,
        "tool_calls": [{
            "id": call_id,
            "type": "function",
            "function": {
                "name": "finish",
                "arguments": json.dumps({
                    "codemap": codemap() if value is None else value,
                }),
            },
        }],
    }


class LmgTests(unittest.TestCase):
    def test_help_has_no_round_limit_option(self):
        with tempfile.TemporaryDirectory() as home:
            proc = subprocess.run(
                [BIN, "--help"], env={**os.environ, "HOME": home},
                text=True, capture_output=True,
            )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("-k", proc.stdout)
        self.assertNotIn("round", proc.stdout)

    def test_embedded_skill_matches_skill_file(self):
        with tempfile.TemporaryDirectory() as home:
            pathlib.Path(home, ".lmg.json").write_text("{")
            proc = subprocess.run(
                [BIN, "--skill"], env={**os.environ, "HOME": home},
                capture_output=True,
            )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, (REPO / "SKILL.md").read_bytes())
        self.assertEqual(proc.stderr, b"")

    def test_process_lifetime_allocation_has_no_release_calls(self):
        source = (REPO / "lmg.c").read_text()
        forbidden = [
            r"\bfree\s*\(",
            r"\brealloc\s*\(",
            r"\bjson_object_put\s*\(",
            r"\bjson_tokener_free\s*\(",
            r"\bcurl_easy_cleanup\s*\(",
            r"\bcurl_slist_free_all\s*\(",
            r"\bcurl_global_cleanup\s*\(",
        ]
        for pattern in forbidden:
            self.assertIsNone(re.search(pattern, source), pattern)

    def test_macos_profile_is_not_stored_in_writable_temp(self):
        source = (REPO / "lmg.c").read_text()
        self.assertIn('(char *)"sandbox-exec", (char *)"-p", ctx->profile', source)
        self.assertIn('"(allow file-write* (subpath %s))\\n"', source)
        self.assertNotIn("profile_path", source)
        self.assertNotIn("sandbox.sb", source)

    def test_agent_loop_config_precedence_reasoning_usage_and_output_limits(self):
        assistant = {
            "role": "assistant",
            "content": None,
            "reasoning_details": [{"type": "opaque", "data": "keep-me"}],
            "tool_calls": [
                {
                    "id": "env-call",
                    "type": "function",
                    "function": {
                        "name": "bash",
                        "arguments": json.dumps({
                            "command": "printf 'key=%s home=%s cwd=%s\\n' \"$LMG_API_KEY\" \"$HOME\" \"$PWD\""
                        }),
                    },
                },
                {
                    "id": "large-call",
                    "type": "function",
                    "function": {
                        "name": "bash",
                        "arguments": json.dumps({
                            "command": "awk 'BEGIN { for (i = 0; i < 35000; i++) print \"x\" }'"
                        }),
                    },
                },
            ],
        }
        final_map = codemap()
        final_map["nodes"][0]["location"] = "systems/README.md:1-40"
        first_usage = {
            "prompt_tokens": 100,
            "completion_tokens": 5,
            "prompt_tokens_details": {"cached_tokens": 10, "cache_write_tokens": 20},
        }
        second_usage = {
            "prompt_tokens": 120,
            "completion_tokens": 6,
            "prompt_tokens_details": {"cached_tokens": 30, "cache_write_tokens": 0},
        }
        with MockAPI([
            response(assistant, first_usage),
            response(finish_message(final_map), second_usage),
        ]) as api:
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                    "api_key": "top-secret",
                    "model": "file-model",
                    "extra": {"temperature": 0.9, "stream": True, "model": "not-owned"},
                }))
                env = {
                    **os.environ,
                    "HOME": home,
                    "LMG_MODEL": "env-model",
                    "LMG_EXTRA_JSON": json.dumps({"temperature": 0.2, "parallel_tool_calls": False}),
                }
                proc = subprocess.run(
                    [BIN, "--yolo", "-C", REPO, "-m", "cli-model", "find it"],
                    env=env, text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout), codemap_output(final_map, {
            "input": 220,
            "output": 11,
            "cache_read": 40,
            "cache_create": 20,
        }))
        self.assertIn('"location":"systems/README.md:1-40"', proc.stdout.replace(" ", ""))
        self.assertNotIn(r"\/", proc.stdout)
        self.assertEqual(proc.stderr, "")
        self.assertEqual(len(api.requests), 2)
        headers, first = api.requests[0]
        self.assertEqual(headers["Authorization"], "Bearer top-secret")
        self.assertEqual(first["model"], "cli-model")
        self.assertEqual(first["temperature"], 0.2)
        self.assertTrue(first["parallel_tool_calls"])
        self.assertFalse(first["stream"])
        self.assertEqual(first["tool_choice"], "auto")
        self.assertEqual(
            [tool["function"]["name"] for tool in first["tools"]],
            ["bash", "finish"],
        )
        finish_schema = first["tools"][1]["function"]["parameters"]
        map_schema = finish_schema["properties"]["codemap"]
        self.assertEqual(map_schema["required"], ["summary", "nodes", "edges"])
        self.assertEqual(
            map_schema["properties"]["nodes"]["items"]["required"],
            ["id", "location", "description"],
        )
        second = api.requests[1][1]
        self.assertIn(assistant, second["messages"])
        env_result = next(
            message["content"] for message in second["messages"]
            if message.get("tool_call_id") == "env-call"
        )
        self.assertIn("key= home=", env_result)
        self.assertNotIn("top-secret", env_result)
        self.assertIn(f"cwd={REPO}", env_result)
        large_result = next(
            message["content"] for message in second["messages"]
            if message.get("tool_call_id") == "large-call"
        )
        self.assertIn("[output truncated]", large_result)
        self.assertLess(len(large_result), 66_000)

    def test_repository_instructions_are_synthesized_with_fallback(self):
        cases = [
            (
                {"AGENTS.md": "agents instructions", "CLAUDE.md": "claude instructions"},
                "AGENTS.md",
                "agents instructions",
            ),
            ({"CLAUDE.md": "fallback instructions"}, "CLAUDE.md", "fallback instructions"),
        ]
        for files, expected_path, expected_content in cases:
            with self.subTest(expected_path=expected_path):
                with tempfile.TemporaryDirectory() as repo, tempfile.TemporaryDirectory() as home:
                    for name, content in files.items():
                        pathlib.Path(repo, name).write_text(content)
                    with MockAPI([response(finish_message())]) as api:
                        pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                            "endpoint": api.endpoint,
                        }))
                        proc = subprocess.run(
                            [BIN, "--yolo", "-C", repo, "question"],
                            env={**os.environ, "HOME": home},
                            text=True, capture_output=True, timeout=10,
                        )
                self.assertEqual(proc.returncode, 0, proc.stderr)
                request = api.requests[0][1]
                self.assertEqual(
                    [tool["function"]["name"] for tool in request["tools"]],
                    ["bash", "finish"],
                )
                self.assertEqual(
                    [message["role"] for message in request["messages"]],
                    ["system", "assistant", "tool", "user"],
                )
                synthetic = request["messages"][1]
                call = synthetic["tool_calls"][0]
                self.assertEqual(call["function"]["name"], "load_agents_md")
                self.assertEqual(
                    json.loads(call["function"]["arguments"]),
                    {"path": expected_path},
                )
                self.assertEqual(request["messages"][2]["tool_call_id"], call["id"])
                self.assertEqual(
                    request["messages"][2]["content"],
                    f"Repository instructions loaded from {expected_path}:\n\n{expected_content}",
                )

    def test_missing_repository_instructions_injects_nothing(self):
        with tempfile.TemporaryDirectory() as repo, tempfile.TemporaryDirectory() as home:
            with MockAPI([response(finish_message())]) as api:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                }))
                proc = subprocess.run(
                    [BIN, "--yolo", "-C", repo, "question"],
                    env={**os.environ, "HOME": home},
                    text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        request = api.requests[0][1]
        self.assertEqual(
            [message["role"] for message in request["messages"]],
            ["system", "user"],
        )
        self.assertNotIn(
            "load_agents_md",
            [tool["function"]["name"] for tool in request["tools"]],
        )

    def test_malformed_config_is_local_error(self):
        with tempfile.TemporaryDirectory() as home:
            pathlib.Path(home, ".lmg.json").write_text("{")
            proc = subprocess.run(
                [BIN, "--yolo", "question"],
                cwd=REPO, env={**os.environ, "HOME": home},
                text=True, capture_output=True,
            )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(proc.stdout, "")
        self.assertEqual(proc.stderr, "lmg: invalid ~/.lmg.json\n")

    @unittest.skipUnless(
        (sys.platform.startswith("linux") and shutil.which("bwrap"))
        or (sys.platform == "darwin" and pathlib.Path("/usr/bin/sandbox-exec").exists()),
        "platform sandbox is not installed",
    )
    def test_default_sandbox_isolated_and_read_only(self):
        with MockAPI([]) as api:
            command = (
                "printf 'cwd=%s home=%s key=%s\\n' \"$PWD\" \"$HOME\" \"$LMG_API_KEY\"; "
                "if p=$(mktemp \"$TMPDIR/lmg-write-test.XXXXXX\" 2>/dev/null); "
                "then rm -f \"$p\"; echo temp-write=yes; else echo temp-write=no; fi; "
                "if touch sandbox-write-test 2>/dev/null; then echo write=yes; else echo write=no; fi; "
                f"if bash -c 'echo x >/dev/tcp/127.0.0.1/{api.server.server_port}' 2>/dev/null; "
                "then echo network=yes; else echo network=no; fi"
            )
            assistant = {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": "sandbox-call",
                    "type": "function",
                    "function": {"name": "bash", "arguments": json.dumps({"command": command})},
                }],
            }
            api.responses = iter([
                response(assistant),
                response(finish_message(codemap("sandbox answer"))),
            ])
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                    "api_key": "sandbox-secret",
                }))
                proc = subprocess.run(
                    [BIN, "--verbose", "-C", REPO, "question"],
                    env={**os.environ, "HOME": home},
                    text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["summary"], "sandbox answer")
        tool_result = next(
            message["content"] for message in api.requests[1][1]["messages"]
            if message.get("tool_call_id") == "sandbox-call"
        )
        if sys.platform.startswith("linux"):
            self.assertIn("cwd=/repo home=/tmp key=", tool_result)
        else:
            self.assertIn(f"cwd={REPO} home=/tmp/lmg.", tool_result)
            self.assertIn(" key=", tool_result)
        self.assertNotIn("sandbox-secret", tool_result)
        self.assertIn("temp-write=yes", tool_result)
        self.assertIn("write=no", tool_result)
        self.assertIn("network=no", tool_result)
        self.assertFalse((REPO / "sandbox-write-test").exists())

    def test_plain_assistant_answer_is_nudged_until_finish(self):
        final_map = codemap("answer after nudge")
        unfinished = {"role": "assistant", "content": "I found the answer."}
        with MockAPI([response(unfinished), response(finish_message(final_map))]) as api:
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                }))
                proc = subprocess.run(
                    [BIN, "--yolo", "-C", REPO, "question"],
                    env={**os.environ, "HOME": home},
                    text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout), codemap_output(final_map))
        self.assertEqual(len(api.requests), 2)
        messages = api.requests[1][1]["messages"]
        unfinished_index = messages.index(unfinished)
        nudge = messages[unfinished_index + 1]
        self.assertEqual(nudge["role"], "user")
        self.assertIn("not called finish", nudge["content"])
        self.assertIn("Continue investigating", nudge["content"])

    def test_verbose_logs_provider_token_usage_per_round(self):
        unfinished = {"role": "assistant", "content": "still working"}
        openrouter_usage = {
            "prompt_tokens": 120,
            "completion_tokens": 8,
            "prompt_tokens_details": {
                "cached_tokens": 80,
                "cache_write_tokens": 32,
            },
        }
        anthropic_usage = {
            "input_tokens": 140,
            "output_tokens": 12,
            "cache_read_input_tokens": 100,
            "cache_creation_input_tokens": 24,
        }
        with MockAPI([
            response(unfinished, openrouter_usage),
            response(finish_message(codemap("done")), anthropic_usage),
        ]) as api:
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                }))
                proc = subprocess.run(
                    [BIN, "--yolo", "--verbose", "-C", REPO, "question"],
                    env={**os.environ, "HOME": home},
                    text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["usage"], {
            "input": 260,
            "output": 20,
            "cache_read": 180,
            "cache_create": 56,
        })
        self.assertEqual(
            proc.stderr,
            "lmg: round 1 usage: input=120 output=8 cache-read=80 cache-create=32\n"
            "lmg: round 2 usage: input=140 output=12 cache-read=100 cache-create=24\n",
        )

    def test_invalid_codemap_gets_tool_error_and_can_retry(self):
        invalid = finish_message({"summary": "missing fields"}, "bad-finish")
        final_map = codemap("corrected")
        with MockAPI([response(invalid), response(finish_message(final_map))]) as api:
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                }))
                proc = subprocess.run(
                    [BIN, "--yolo", "-C", REPO, "question"],
                    env={**os.environ, "HOME": home},
                    text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout), codemap_output(final_map))
        tool_result = next(
            message for message in api.requests[1][1]["messages"]
            if message.get("tool_call_id") == "bad-finish"
        )
        self.assertEqual(tool_result["role"], "tool")
        self.assertEqual(tool_result["tool_call_id"], "bad-finish")
        self.assertIn("Invalid codemap", tool_result["content"])

    def test_http_error_is_api_error(self):
        class ErrorHandler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                self.send_response(401)
                self.end_headers()

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ErrorHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            proc = subprocess.run(
                [BIN, "--yolo", "-e", f"http://127.0.0.1:{server.server_port}", "question"],
                cwd=REPO, env={**os.environ, "HOME": tempfile.gettempdir()},
                text=True, capture_output=True,
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
        self.assertEqual(proc.returncode, 2)
        self.assertEqual(proc.stdout, "")
        self.assertEqual(proc.stderr, "lmg: API returned HTTP 401\n")

    def test_retries_network_and_retryable_http_errors(self):
        usage = {
            "prompt_tokens": 12,
            "completion_tokens": 3,
            "prompt_tokens_details": {"cached_tokens": 4, "cache_write_tokens": 5},
        }
        with MockAPI([
            DISCONNECT,
            (429, {"error": "busy"}),
            response(finish_message(codemap("retried")), usage),
        ]) as api:
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                }))
                proc = subprocess.run(
                    [BIN, "--yolo", "-C", REPO, "question"],
                    env={**os.environ, "HOME": home},
                    text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(api.requests), 3)
        self.assertEqual(api.requests[0][1], api.requests[1][1])
        self.assertEqual(api.requests[1][1], api.requests[2][1])
        self.assertIn("lmg: API request failed:", proc.stderr)
        self.assertIn("retrying in 1s", proc.stderr)
        self.assertIn("lmg: API returned HTTP 429; retrying in 2s", proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["usage"], {
            "input": 12,
            "output": 3,
            "cache_read": 4,
            "cache_create": 5,
        })


if __name__ == "__main__":
    unittest.main()
