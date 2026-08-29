#!/usr/bin/env python3
import http.server
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest


BIN = pathlib.Path(os.environ.get("LMG_BIN", pathlib.Path(__file__).parents[1] / "lmg"))
REPO = pathlib.Path(__file__).parents[1].resolve()


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
                encoded = json.dumps(response).encode()
                self.send_response(200)
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


def response(message):
    return {"choices": [{"message": message}]}


class LmgTests(unittest.TestCase):
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

    def test_agent_loop_config_precedence_reasoning_and_limits(self):
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
                        "arguments": json.dumps({"command": "yes x | head -c 70000"}),
                    },
                },
            ],
        }
        with MockAPI([response(assistant), response({"role": "assistant", "content": "compact answer"})]) as api:
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                    "api_key": "top-secret",
                    "model": "file-model",
                    "max_steps": 7,
                    "extra": {"temperature": 0.9, "stream": True, "model": "not-owned"},
                }))
                env = {
                    **os.environ,
                    "HOME": home,
                    "LMG_MODEL": "env-model",
                    "LMG_MAX_STEPS": "3",
                    "LMG_EXTRA_JSON": json.dumps({"temperature": 0.2, "parallel_tool_calls": True}),
                }
                proc = subprocess.run(
                    [BIN, "--yolo", "-C", REPO, "-m", "cli-model", "-k", "2", "find it"],
                    env=env, text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "compact answer\n")
        self.assertEqual(proc.stderr, "")
        self.assertEqual(len(api.requests), 2)
        headers, first = api.requests[0]
        self.assertEqual(headers["Authorization"], "Bearer top-secret")
        self.assertEqual(first["model"], "cli-model")
        self.assertEqual(first["temperature"], 0.2)
        self.assertTrue(first["parallel_tool_calls"])
        self.assertFalse(first["stream"])
        self.assertEqual(first["tool_choice"], "auto")
        second = api.requests[1][1]
        self.assertEqual(second["messages"][2], assistant)
        env_result = second["messages"][3]["content"]
        self.assertIn("key= home=", env_result)
        self.assertNotIn("top-secret", env_result)
        self.assertIn(f"cwd={REPO}", env_result)
        large_result = second["messages"][4]["content"]
        self.assertIn("[output truncated]", large_result)
        self.assertLess(len(large_result), 66_000)

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
                response({"role": "assistant", "content": "sandbox answer"}),
            ])
            with tempfile.TemporaryDirectory() as home:
                pathlib.Path(home, ".lmg.json").write_text(json.dumps({
                    "endpoint": api.endpoint,
                    "api_key": "sandbox-secret",
                }))
                proc = subprocess.run(
                    [BIN, "-C", REPO, "question"],
                    env={**os.environ, "HOME": home},
                    text=True, capture_output=True, timeout=10,
                )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "sandbox answer\n")
        tool_result = api.requests[1][1]["messages"][3]["content"]
        self.assertIn("cwd=/repo home=/tmp key=", tool_result)
        self.assertNotIn("sandbox-secret", tool_result)
        self.assertIn("write=no", tool_result)
        self.assertIn("network=no", tool_result)
        self.assertFalse((REPO / "sandbox-write-test").exists())

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


if __name__ == "__main__":
    unittest.main()
