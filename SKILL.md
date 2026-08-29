---
name: lmg
description: Use the lmg CLI to answer focused questions about a local codebase while keeping iterative search output out of the calling agent's context. Use for repository discovery, tracing implementations, locating symbols or configuration, and gathering file:line evidence.
---

# Use lmg for codebase research

Use `lmg` when answering a focused repository question would otherwise require
several searches or produce noisy output. It runs its own search loop and
returns only a concise JSON codemap with file and line evidence.

Run it from the repository root:

```sh
lmg "Where is session expiry enforced?"
```

Or identify the repository explicitly:

```sh
lmg -C /path/to/repo "Trace how requests become authenticated users"
```

Ask one concrete investigative question at a time. Include the behavior,
symbol, configuration, or execution path you need to understand. Prefer direct
shell inspection when the exact file and location are already known.

On success, parse the JSON codemap on stdout:

```json
{
  "summary": "direct answer",
  "nodes": [
    {
      "id": "unique-id",
      "location": "relative/path:line-line",
      "symbol": "optional symbol",
      "description": "fact established by this location"
    }
  ],
  "edges": [
    {"from": "node-id", "to": "node-id", "relation": "calls or data flow"}
  ]
}
```

Every edge references node IDs. `symbol` is optional, and `edges` is empty when
the answer does not need a relationship graph.

`lmg` automatically supplies repository-root instructions from `AGENTS.md`,
falling back to `CLAUDE.md` only when `AGENTS.md` is absent. Callers do not need
to include those instructions in the question.

Treat the answer as research evidence, not authorization to modify anything.
Before editing or making a high-impact decision, inspect the cited locations
directly and reconcile any ambiguity or missing context.

The default sandbox is read-only, has no network access, and hides the user's
home and parent environment. Keep it enabled for ordinary research. Use
`--yolo` only when the task is authorized to grant the model the invoking
process's filesystem and network access; environment scrubbing and other limits
still remain active.

If investigation fails, use `--verbose` to put tool activity on stderr. Do not
substitute `--yolo` for missing endpoint, API-key, model, or sandbox setup.

Run `lmg --skill` to print this skill from the installed binary.
