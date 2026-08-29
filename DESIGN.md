# `lmg` — LM grep

## Purpose

`lmg` is a small CLI that answers questions about a local codebase.

It is intended to be invoked by another coding agent. Instead of pushing large `grep` results into the parent agent's context, `lmg` runs its own agent loop and returns only a compact answer with file/line evidence.

```text
coding agent
    |
    | lmg "Where is session expiry handled?"
    v
lmg
    |
    +-- bash: rg ...
    +-- bash: sed ...
    +-- bash: git ...
    |
    v
JSON codemap: summary + evidence nodes + relationship edges + token usage
```

The search process, tool output, and model reasoning remain private to `lmg`.

---

## Implementation

`lmg` is one C source file linked against:

```sh
cc -O2 -o lmg lmg.c -lcurl -ljson-c
```

Runtime dependencies:

```text
bash
Linux:  bubblewrap (bwrap)
macOS:  sandbox-exec
```

There is no daemon, index, database, persistent state, SDK, or external language runtime.

`lmg` communicates with an OpenAI-compatible `/chat/completions` endpoint using native function calling.

`SKILL.md` contains concise instructions for coding agents that invoke `lmg`.
The build embeds that file byte-for-byte in the executable; `lmg --skill`
prints the embedded copy to stdout without loading configuration or accessing
the network.

### Memory lifetime

`lmg` deliberately uses process-lifetime allocation. The C program contains no
memory-release operations: it does not free or resize heap allocations,
decrement JSON reference counts, free JSON tokenizers, or tear down libcurl
objects. Configuration strings, JSON conversation trees, HTTP handles, headers,
and bounded I/O buffers remain live until the process exits, at which point the
operating system reclaims them together.

This is an intentional safety tradeoff for an invocation that is expected to
finish promptly: ownership and cleanup ordering cannot introduce use-after-free
bugs. The agent loop itself is not round-limited, so conversation allocations
grow until `finish` succeeds or an error ends the process. File descriptors,
child processes, and temporary filesystem state are still cleaned up promptly
because they affect external system state.

---

## CLI

```sh
lmg "Where is refresh token invalidation implemented?"
```

The current directory is the repository root by default.

```text
-C DIR          repository root
-m MODEL        model name
-e URL          /chat/completions endpoint
--verbose       print tool activity and per-round token usage to stderr
--yolo          execute bash without OS sandboxing
--skill         print the embedded coding-agent skill and exit
```

Example:

```sh
lmg -C ~/src/project \
  "Trace how an HTTP request becomes an authenticated user"
```

Stdout contains only the final JSON codemap.

Diagnostics and tool activity go to stderr.

---

## Configuration

Configuration precedence, highest to lowest:

```text
command-line arguments
environment variables
~/.lmg.json
built-in defaults
```

`lmg` reads `~/.lmg.json` at startup if it exists.

Example:

```json
{
  "endpoint": "https://openrouter.ai/api/v1/chat/completions",
  "api_key": "sk-...",
  "model": "qwen/qwen3-coder",
  "extra": {
    "reasoning": {
      "effort": "high"
    }
  }
}
```

Supported keys:

```text
endpoint
api_key
model
extra
```

Equivalent environment variables:

```text
LMG_ENDPOINT
LMG_API_KEY
LMG_MODEL
LMG_EXTRA_JSON
```

Environment variables override corresponding file values.

For example:

```sh
export LMG_MODEL=deepseek/deepseek-r1
lmg "Where is the parser initialized?"
```

uses the endpoint and API key from `~/.lmg.json`, but the model from `LMG_MODEL`.

Command-line flags override both:

```sh
lmg -m qwen/qwen3-coder "Where is the parser initialized?"
```

### Loading

Conceptually:

```text
config = defaults

if ~/.lmg.json exists:
    merge(config, parse_json(file))

merge(config, environment)

merge(config, command_line)
```

The path is derived from the user's home directory rather than invoking a shell to expand `~`.

A missing config file is not an error.

Malformed JSON is an error:

```text
lmg: invalid ~/.lmg.json
```

Unknown top-level keys may be ignored to permit forward compatibility.

The `extra` object is merged into the top-level Chat Completions request.

`LMG_EXTRA_JSON`, when present, must contain a JSON object and replaces or overrides values supplied by the file's `extra` object.

`lmg` owns and overrides these request fields regardless of `extra`:

```text
model
messages
tools
tool_choice
parallel_tool_calls
stream
```

All config parsing and merging uses `json-c`.

---

## Agent Loop

Before the first API request, LMG checks the repository root for `AGENTS.md`. If
it does not exist, LMG checks for `CLAUDE.md`. When one is found, its contents
are represented as a synthetic tool exchange:

```text
assistant + load_agents_md({"path":"AGENTS.md"})
tool + repository instruction contents
```

`load_agents_md` is an internal initialization event and is deliberately not
included in the request's `tools` catalog. The instruction file is limited to
256 KiB. If neither file exists, LMG injects neither the call nor a result.

The private conversation is therefore:

```text
system
[optional assistant + load_agents_md]
[optional tool + repository instructions]
user
assistant + reasoning + tool_calls
tool
assistant + reasoning + tool_calls
tool
...
assistant + finish(codemap)
```

Pseudo-code:

```text
messages = [system_prompt]

if AGENTS.md exists:
    append synthetic load_agents_md call and its file contents
else if CLAUDE.md exists:
    append synthetic load_agents_md call and its file contents

append user_question

for round = 1 .. forever:
    response = chat_completions(messages)
    add response.usage to aggregate token usage
    if verbose, log this round's token usage to stderr

    assistant = response.choices[0].message
    append assistant unchanged enough to preserve reasoning state

    if assistant called finish with a valid codemap:
        add aggregate token usage to the codemap
        print codemap as JSON
        exit 0

    if assistant called bash:
        execute each bash call and append its tool result
        continue

    if assistant did not call a tool:
        append a user message reminding it to continue and call finish
        continue

fail
```

There is no maximum round count. The loop ends only when `finish` succeeds or
a local/API error occurs.

Multiple bash calls in one response are supported and may be executed
sequentially. `finish` must be the only tool call in its response. Normal
assistant content is never accepted as the final result; a turn containing only
content receives a continuation nudge and starts another round.

---

## Tools

The advertised tool catalog contains exactly two functions. The synthetic
`load_agents_md` initialization call described above is not one of them. `bash`
is the repository inspection primitive:

```json
{
  "type": "function",
  "function": {
    "name": "bash",
    "description": "Run a shell command in the repository to search or inspect the codebase.",
    "parameters": {
      "type": "object",
      "properties": {
        "command": {
          "type": "string"
        }
      },
      "required": ["command"]
    }
  }
}
```

Typical calls:

```sh
rg -n "refresh_token" .
```

```sh
sed -n '120,190p' src/auth.c
```

```sh
find . -iname '*session*'
```

```sh
git log -S'refresh_token' --oneline -- src
```

```sh
rg -l 'validate_session' . | head
```

This gives the model normal Unix composition rather than exposing a growing collection of specialized tools.

`finish` is the completion boundary. Its `codemap` argument has this logical
shape (the actual function definition supplies the equivalent JSON Schema):

```json
{
  "summary": "Concise direct answer",
  "nodes": [
    {
      "id": "unique-short-id",
      "location": "relative/path.c:12-34",
      "symbol": "optional_symbol",
      "description": "The fact this location proves"
    }
  ],
  "edges": [
    {
      "from": "source-node-id",
      "to": "destination-node-id",
      "relation": "calls, feeds, owns, or another concise relationship"
    }
  ]
}
```

`summary`, `nodes`, and `edges` are always present. Node IDs are unique. Each
edge endpoint references a node ID. `symbol` is the only optional field. Use an
empty `edges` array when the answer needs no graph. Locations use repository-
relative `file:line` or `file:start-end` references.

LMG validates the codemap again at runtime. A malformed map produces a tool
error in the private conversation so the model can repair it on a later round.
On a valid call, LMG adds aggregate `usage`, pretty-prints the resulting codemap
to stdout, and exits. `usage` is generated by LMG and is not part of the
model-supplied `finish` schema.

---

## Bash Execution

A tool call executes conceptually as:

```sh
bash -lc "$command"
```

Do not use `system()`.

Use `fork`/`exec`, pipes, and a process group so the whole command tree can be terminated on timeout.

The returned tool message contains bounded output:

```text
exit_code: 0
stdout:
...

stderr:
...
```

Suggested limits:

```text
tool output       64 KiB
command timeout   30 seconds
```

If output is truncated:

```text
[output truncated]
```

These limits remain active with `--yolo`.

---

## Sandbox

Sandboxing is enabled by default.

The sandbox provides the model with:

```text
repository       read-only
/tmp             private and writable
system binaries  read-only
network          disabled
user home        unavailable
parent env       unavailable
```

The shell receives a minimal environment such as:

```text
PATH
LANG
LC_ALL
HOME
TMPDIR
```

In particular, never pass:

```text
LMG_API_KEY
LMG_ENDPOINT
provider credentials
SSH_AUTH_SOCK
AWS_*
GITHUB_*
other inherited secrets
```

The contents of `~/.lmg.json`, including `api_key`, are never exposed inside the sandbox.

Environment scrubbing is independent of the OS sandbox and remains enabled under `--yolo`.

If the platform sandbox cannot be initialized, `lmg` fails rather than silently running unsandboxed.

---

## Linux Sandbox

Linux uses `bwrap`.

Conceptually:

```text
bwrap
  --unshare-all
  --die-with-parent
  --new-session
  --clearenv
  --ro-bind <system paths>
  --ro-bind <repo> /repo
  --tmpfs /tmp
  --proc /proc
  --dev /dev
  --chdir /repo
  --setenv HOME /tmp
  --setenv TMPDIR /tmp
  --setenv PATH ...
  /bin/bash -lc <command>
```

Do not re-enable networking.

The exact system mounts should be detected from the host rather than assuming one filesystem layout. Typical read-only mounts include `/usr`, `/bin`, `/lib`, and `/lib64` when present.

The repository is exposed as `/repo`.

The system prompt instructs the model to report repository paths relative to `/repo`.

---

## macOS Sandbox

macOS uses `sandbox-exec` with a generated Seatbelt profile.

Conceptually:

```sh
sandbox-exec -f PROFILE /bin/bash -lc "$command"
```

The profile uses default-deny behavior and permits only what the search shell needs:

```text
read repository
read/execute system binaries and libraries
spawn subprocesses
read basic system metadata required by those programs
read/write a private temporary directory
```

It denies:

```text
repository writes
reads from the user's home directory
network access
unrelated filesystem access
```

A fresh temporary directory and sandbox profile are generated for each `lmg` invocation and removed afterwards.

As on Linux, the repository should appear to the shell under a predictable path and commands run with that directory as the working directory.

---

## `--yolo`

`--yolo` disables `bwrap` / `sandbox-exec`.

The tool then runs approximately:

```sh
cd "$repo"
bash -lc "$command"
```

This gives the model the same filesystem and network access as the `lmg` process.

It is intentionally explicit:

```sh
lmg --yolo "Run the test suite and find the failing implementation"
```

`--yolo` does not disable:

```text
environment scrubbing
command timeout
output limits
```

It only removes the operating-system sandbox.

---

## Reasoning Preservation

Thinking state is treated as opaque protocol data.

When present, preserve fields including:

```text
reasoning_content
reasoning
reasoning_details
```

The assistant message containing `tool_calls` is retained and sent back before the corresponding tool results.

Reasoning fields are never interpreted, summarized, truncated, reordered, printed, or translated between provider formats.

This supports Qwen-style, DeepSeek-style, OpenRouter-style, and encrypted reasoning representations without provider-specific reasoning logic.

In particular, structured or encrypted `reasoning_details` objects must be passed through unchanged.

---

## JSON

Use `json-c` for all JSON handling:

```text
configuration parsing
configuration merging
request construction
response parsing
conversation state
tool definitions
tool calls
tool arguments
tool results
reasoning preservation
LMG_EXTRA_JSON
```

Do not construct JSON manually.

The in-memory conversation can simply be a `json_object` array of message objects.

Prefer retaining the provider's assistant message fields rather than rebuilding an assistant message from only `content` and `tool_calls`.

---

## System Prompt

Keep it short:

```text
You are a codebase search agent.

Answer the user's question using evidence from the repository.

You have bash and finish tools. Use bash with normal read-only Unix commands such as rg,
find, sed, awk, git, and cat to search and inspect the codebase.

Search iteratively. Do not speculate when you can inspect the code.
Avoid unnecessarily large outputs.

Once you have enough evidence, call finish exactly once and do not call it alongside bash.
The codemap has a summary, evidence nodes, and relationship edges between node ids.
Do not return the final answer as normal assistant content. All node locations should
use repository-relative file:line references.
```

Safety comes from the sandbox rather than from asking the model to avoid dangerous shell commands.

---

## HTTP

Each agent round performs one request:

```text
POST $LMG_ENDPOINT
Authorization: Bearer $LMG_API_KEY
Content-Type: application/json
```

Conceptually:

```json
{
  "model": "...",
  "messages": [...],
  "tools": [...],
  "tool_choice": "auto",
  "parallel_tool_calls": true,
  "stream": false
}
```

Use `libcurl` directly.

Accumulate the response with a write callback and parse it using `json-c`.

Retry transport failures and HTTP 408, 409, 425, 429, and 5xx responses in
place, without advancing the logical agent round. Back off exponentially from
1 second to at most 30 seconds between attempts. There is no total retry cap;
each retry is reported on stderr. Permanent HTTP failures and local libcurl
errors such as malformed URLs, response-buffer overflow, or unsupported
protocols fail immediately.

For every successful response, normalize token usage from the OpenAI/OpenRouter
Chat Completions fields (`prompt_tokens`, `completion_tokens`, and
`prompt_tokens_details.cached_tokens` / `cache_write_tokens`). Also accept the
Anthropic-compatible `input_tokens`, `output_tokens`,
`cache_read_input_tokens`, and `cache_creation_input_tokens` aliases. In
`--verbose` mode, log the four values for each round. Missing values are shown
as `-` in the trace and contribute zero to aggregate output usage.

Streaming is unnecessary for v1.

---

## Output

The parent agent receives only the final codemap:

```json
{
  "summary": "Session expiry is validated before the HTTP failure is returned.",
  "nodes": [
    {
      "id": "validate-session",
      "location": "src/auth/session.c:142-171",
      "symbol": "validate_session",
      "description": "Rejects expired sessions."
    },
    {
      "id": "auth-middleware",
      "location": "src/http/middleware.c:88-103",
      "symbol": "auth_middleware",
      "description": "Turns session validation failure into HTTP 401."
    }
  ],
  "edges": [
    {
      "from": "auth-middleware",
      "to": "validate-session",
      "relation": "calls before mapping failure to HTTP 401"
    }
  ],
  "usage": {
    "input": 4210,
    "output": 380,
    "cache_read": 2048,
    "cache_create": 1024
  }
}
```

`usage` is added by LMG after the model calls `finish` and aggregates every
provider response, including the final round. It never receives the intermediate
`rg`, `sed`, Git output, or model reasoning.

---

## Errors

```text
lmg: invalid ~/.lmg.json
lmg: endpoint is not configured
lmg: cannot load AGENTS.md
lmg: bwrap not found; use --yolo to run unsandboxed
lmg: sandbox-exec failed; use --yolo to run unsandboxed
lmg: API request failed: ...; retrying in 1s
lmg: API returned HTTP 429; retrying in 2s
lmg: API returned HTTP 401
lmg: malformed arguments for bash
lmg: command timed out
```

Exit status:

```text
0   success
1   local/runtime failure
2   API failure
```

---

## Non-goals

Version 1 does not need embeddings, indexing, AST parsing, MCP, a daemon, caching, editing APIs, provider SDKs, persistent state, or specialized search functions.

There is one repository-inspection primitive:

```text
bash
```

`finish` is a protocol boundary, not an additional inspection capability.

The sandbox determines what that primitive is allowed to do.

---

## Design Principle

`lmg` is an intelligent Unix search subprocess:

```sh
lmg "Where do we retry failed database transactions?"
```

Its boundary is:

```text
large noisy search/reasoning process
              |
              v
small evidence-rich codemap
```

Everything inside that boundary should remain as simple as possible.
