# CLAUDE.md

Operational guide for working in this repo. Architecture and rationale live in
[`DESIGN.md`](./DESIGN.md) — read it before changing behavior; don't duplicate it here.

## What this is

`tgcurl` — a terminal-only, **JSON-only** CLI for a personal Telegram account (not a bot),
**C++17** on Telegram's official **TDLib** (`td::ClientManager`). One-shot per command:
connect, do one thing, print JSON, exit. `tgcurl -mcp` serves the same commands as MCP tools
over stdio.

## Commands

```
make deps         # install build + dev tooling (dnf/apt; needs sudo). TDLib separately:
                  #   Fedora: sudo dnf copr enable stevenlin/tdlib-master \
                  #        && sudo dnf install tdlib-devel tdlib-static   # >= 1.8.63
                  #   (base-repo 1.8.0 can't log in: 406 UPDATE_APP_TO_LOGIN)
make build        # configure + compile -> build/tgcurl  (BUILD_TYPE=Debug for debug)
make test         # ctest
make format       # clang-format -i    (format-check = dry-run)
make tidy         # clang-tidy         (needs a configure first)
make lint         # cppcheck
make static       # self-contained binary
make release      # static binary -> .rpm + .deb (needs nfpm)
```

TDLib must be available to the build (system `find_package(Td)` or vendored) — see
DESIGN.md → *Build*. `make build` exports `compile_commands.json` for clangd/clang-tidy.

## Layout

`src/main.cpp` dispatch (derived from the registry) · `src/tdclient.*` sync wrapper over TDLib
(central reuse point) · `src/auth.*` login state machine · `src/config.*` · `src/resolve.*`
id→chat_id · `src/commands/registry.*` THE command table (CLI + MCP, one entry per command) ·
`src/commands/*` one file per command · `src/mcp.*` MCP stdio server · `src/send_confirm.h`
server-ack wait for sends · `src/json_out.h` / `src/json_in.*` JSON out/in. Build in `build/`,
packages in `dist/`.

## Conventions

- **Output is JSON, always.** Success on stdout; errors as JSON on stderr with a non-zero exit.
  Never prompt on stdin outside `login` — other commands run head-less.
- **New command = one handler + one `CommandSpec` in `src/commands/registry.cpp`.** Both
  front-ends (CLI dispatch and MCP tools) derive from that table — never wire a command into
  only one of them. Handlers write to the `std::ostream&` they're given, not `std::cout`.
- **TDLib is asynchronous — a response is not a server ack.** If the effect arrives via an
  `update*` (like `sendMessage` → `updateMessageSendSucceeded/Failed`), the command must wait
  for that terminal update before reporting success (see DESIGN.md → *Asynchrony discipline*;
  reuse `src/send_confirm.h`'s pattern).
- **`chat_id` is the primary identifier.** `@username` resolves via `searchPublicChat`; anything
  non-numeric / non-`@` is an `unresolvable` error. No fuzzy name matching.
- Secrets never enter the repo. Session lives in `~/.config/tgcurl/` (or `TGCURL_CONFIG_DIR`).
- C++17; formatting and checks are pinned in `.clang-format` / `.clang-tidy` — don't restate
  style rules here.

## Definition of done (every change) — in order

1. Implement the change.
2. Write tests covering it (pure logic → a `ctest` unit test; network paths → note the manual
   check). `make build` and `make test` pass — never break the build or existing tests.
3. `make format` clean; no new `make lint` / `make tidy` warnings in changed code.
4. Only then commit.

## Workflow

- Do tasks **strictly sequentially** in issue dependency order (#1 → #9); one at a time.
- Work tracks GitHub issues **#1–#9** (`dblokhin/tgcurl`).
- Branch off `main`; commit/push only when asked. Commit message must include **`closes #<id>`**
  when an issue covers the change, and end with the `Co-Authored-By` trailer.
- Tools available: GitHub MCP (issues/PRs), Context7 MCP (library docs incl. TDLib `td_api`).
