# lmg

`lmg` is a small native CLI that answers questions about a local codebase. It
runs a tool-calling agent loop, lets the model inspect the repository with
normal Unix commands, and prints only the final evidence-rich codemap.

## Build

Install the development packages for libcurl and json-c, then run:

```sh
make
```

The build embeds `SKILL.md` before compiling. The equivalent commands are:

```sh
make skill.inc
cc -O2 -o lmg lmg.c -lcurl -ljson-c
```

At runtime, sandboxed execution requires `bwrap` on Linux or `sandbox-exec` on
macOS. Bash is required on both platforms.

## Configure and run

Create `~/.lmg.json`:

```json
{
  "endpoint": "https://openrouter.ai/api/v1/chat/completions",
  "api_key": "sk-...",
  "model": "qwen/qwen3-coder"
}
```

Then ask a question from a repository:

```sh
lmg "Where is session expiry handled?"
lmg -C ~/src/project "Trace request authentication"
```

Successful output is a JSON codemap:

```json
{
  "summary": "Session validation rejects expired sessions before middleware returns 401.",
  "nodes": [
    {
      "id": "session-validation",
      "location": "src/auth/session.c:142-171",
      "symbol": "validate_session",
      "description": "Checks the expiry time and rejects expired sessions."
    }
  ],
  "edges": [],
  "usage": {
    "input": 4210,
    "output": 380,
    "cache_read": 2048,
    "cache_create": 1024
  }
}
```

Nodes are evidence-bearing code locations. Edges connect node IDs to describe
call, data-flow, control-flow, or ownership relationships. `lmg` emits a result
only when the model calls its `finish` tool with a valid codemap. `usage` is
added by `lmg` and aggregates provider-returned token counts across all rounds.

Before the first model request, `lmg` loads repository-root instructions from
`AGENTS.md`, or from `CLAUDE.md` when `AGENTS.md` does not exist. It represents
the loaded file as a synthetic `load_agents_md` tool call and result in the
conversation, without advertising that internal operation in the tool catalog.
If neither file exists, no messages are injected.

Configuration may also be supplied with `LMG_ENDPOINT`, `LMG_API_KEY`,
`LMG_MODEL`, and `LMG_EXTRA_JSON`. CLI options take precedence:

```text
-C DIR          repository root
-m MODEL        model name
-e URL          /chat/completions endpoint
--verbose       print tool activity and per-round token usage to stderr
--yolo          execute bash without OS sandboxing
```

`--yolo` disables only the OS sandbox. Command timeouts, output limits, and
environment scrubbing remain active. The agent loop has no round limit; it
continues until `finish` succeeds or a local/API error occurs.

Parallel tool calling is enabled on every model request. LMG accepts multiple
`bash` calls in one assistant turn and returns a result for each call.

Network failures and retryable API responses (HTTP 408, 409, 425, 429, and
5xx) are retried indefinitely. Retries use exponential backoff from 1 second to
a maximum of 30 seconds between attempts and are reported on stderr. Permanent
HTTP errors still fail immediately.

Coding agents can load the bundled usage skill without locating project files:

```sh
lmg --skill
```

## Test

```sh
make test
```

See [DESIGN.md](DESIGN.md) for the complete behavior and security model.
