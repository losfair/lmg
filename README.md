# lmg

`lmg` is a small native CLI that answers questions about a local codebase. It
runs a short tool-calling agent loop, lets the model inspect the repository with
normal Unix commands, and prints only the final evidence-rich answer.

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
  "model": "qwen/qwen3-coder",
  "max_steps": 8
}
```

Then ask a question from a repository:

```sh
lmg "Where is session expiry handled?"
lmg -C ~/src/project "Trace request authentication"
```

Configuration may also be supplied with `LMG_ENDPOINT`, `LMG_API_KEY`,
`LMG_MODEL`, `LMG_MAX_STEPS`, and `LMG_EXTRA_JSON`. CLI options take precedence:

```text
-C DIR          repository root
-m MODEL        model name
-e URL          /chat/completions endpoint
-k N            maximum agent rounds
--verbose       print tool activity to stderr
--yolo          execute bash without OS sandboxing
```

`--yolo` disables only the OS sandbox. Command timeouts, output limits, agent
round limits, and environment scrubbing remain active.

Coding agents can load the bundled usage skill without locating project files:

```sh
lmg --skill
```

## Test

```sh
make test
```

See [DESIGN.md](DESIGN.md) for the complete behavior and security model.
