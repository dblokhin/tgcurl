# tgcurl — Design

`tgcurl` is a terminal-only utility for working with a **personal Telegram account** (not a
bot) over Telegram's MTProto protocol. Its distinguishing purpose: be a **deterministic,
Unix-way CLI that an AI agent can drive** to perform Telegram tasks — *"`curl` for Telegram."*

It exposes small, composable, **JSON-only** primitives; a calling agent combines them (e.g.
dump contacts, then decide which chat to message). Every invocation is one shot: connect, do
one thing, print JSON, exit. The same primitives are also served as **MCP tools**
(`tgcurl -mcp`, see *MCP mode*) so an agent runtime can call them without shelling out.

---

## Goals

- **Personal-account access**, not a bot — full MTProto user session.
- **Deterministic & Unix-way** — each command does exactly one thing and is composable in
  pipelines. No hidden interactive magic in the hot path.
- **Machine-first output** — every command emits JSON so an AI agent (or `jq`) can parse it
  reliably; errors are JSON too, on stderr, with a non-zero exit code.
- **Login once, reuse forever** — authenticate a single time; subsequent runs never re-prompt.
- **Flexibility lives in the agent, not the tool** — the tool provides primitives (e.g. a
  contacts dump with stable ids); the agent matches intent to identifiers itself.

## Non-goals

- No fuzzy display-name matching (e.g. "John Smith" → chat). The agent does that using list
  output.
- A local TDLib database *is* kept (`use_message_database=true`, which implies chat-info and file
  databases) — required so a `chat_id` stays usable across one-shot runs (see *Peer identification
  → warming*). It lives under `database_directory` (mode 0700) and is cleared on `logout`.
- No hand-rolled MTProto implementation.

---

## Language & dependency decision

**No third-party protocol libraries.** We use Telegram's **officially recommended library,
[TDLib](https://core.telegram.org/tdlib)** — which is C++ — and therefore write the utility
**in C++** to consume it natively (no cgo/wrapper layer).

We use TDLib's native, type-safe C++ API — **`td::ClientManager`** (`td/telegram/Client.h`)
with the auto-generated `td_api::*` request/object types — rather than the JSON
(`td_json_client`) interface, since writing in C++ gives type safety and no serialization glue.
The official example `example/cpp/td_example.cpp` in `tdlib/td` is the primary reference for
the request/response and authorization patterns.

### Interaction model

TDLib is **asynchronous and update-driven**:

- `ClientManager::create_client_id()` → a client id.
- `send(client_id, request_id, request)` — enqueue a request from any thread.
- `receive(timeout)` — pull the next update/response; responses are matched to requests via
  the `request_id` we pass.

Because `tgcurl` is one-shot per command, we wrap this event loop in a small **synchronous
`send_query()` helper**: send a query, pump `receive()` until the matching response (or error)
arrives, return it. This turns TDLib's event loop into blocking calls that fit a CLI, and is
the central reuse point — every command and the auth flow go through it.

### Asynchrony discipline — a response is not an acknowledgement

The rule every command (current and future) must obey: **a command may not report success or
exit until TDLib has confirmed the effect it claims** — because tgcurl is one-shot, process
exit destroys any work TDLib still has queued.

TDLib requests fall into two classes:

1. **Round-trip requests** (`getContacts`, `importContacts`, `searchPublicChat`,
   `setMessageSenderBlockList`, …): the response itself is the server's answer. `send_query()`
   returning a non-error object *is* the confirmation — nothing more to wait for.
2. **Fire-and-forget requests** — **`sendMessage` is the canonical case**: the response comes
   back *immediately* with a **local, pending** message (temporary id); the server hasn't
   accepted anything yet. The real outcome arrives later as an update —
   `updateMessageSendSucceeded` / `updateMessageSendFailed`, carrying the temporary id in
   `old_message_id_`. A process that exits after the response but before that update **silently
   drops the message** (this was a real bug). Such commands must pump updates until the
   terminal update for *their* request arrives (bounded by a timeout), and only then print
   `{"ok":true}`.

How this is kept from regressing:

- The wait logic is not ad-hoc inside the command: it lives in **`src/send_confirm.h`
  (`SendConfirmation`)** — install as the update handler *before* issuing `sendMessage`, feed
  every update, then `set_pending_id()` from the response. It handles both orderings (the
  terminal update may be dispatched *before* the response is processed — it is buffered and
  replayed, not dropped) and is covered by a unit test (`tests/test_send_confirm.cpp`) that
  pins the raced ordering as a regression guard.
- **Checklist for any new command:** find the request in the td_api docs; if the result the
  user cares about is delivered via an `update*` rather than the response (send/forward/edit,
  file upload via `updateFile`, etc.), it is class 2 — reuse `SendConfirmation` or follow its
  pattern (observe-before-send, match by id, bounded wait), and add the terminal-update wait
  before reporting success.
- On timeout the command reports `request_failed` *without* claiming the action didn't happen —
  the server may still accept it later; the error text says so ("timed out waiting for the
  server to accept").

Related but distinct: the graceful `close` in `TdClient`'s destructor (pump until
`authorizationStateClosed`) is what flushes TDLib's databases on exit; skipping it silently
loses this run's peer cache. Both rules are two faces of the same fact — **TDLib is
asynchronous; returning from `send()` proves nothing.**

---

## Authentication & session lifecycle

Auth is a **state machine** driven by `updateAuthorizationState`. TDLib emits states in order;
we react to each:

1. `authorizationStateWaitTdlibParameters` → `setTdlibParameters` with `api_id`, `api_hash`,
   the **`database_directory`** (where the session lives), `use_message_database=true` (persist
   the chat/peer cache so a `chat_id` survives between runs — see *Peer identification*),
   `use_secret_chats=false`, plus system/app version.
2. `authorizationStateWaitPhoneNumber` → `setAuthenticationPhoneNumber(phone)`.
3. `authorizationStateWaitCode` → `checkAuthenticationCode(code)`.
4. `authorizationStateWaitPassword` (only if 2FA enabled) → `checkAuthenticationPassword(pw)`.
5. `authorizationStateReady` → authorized; ordinary requests allowed.

**"Log in once, never re-auth" is native to TDLib.** The session key lives in
`database_directory`. On a subsequent run with a valid database, TDLib transitions **straight
to `authorizationStateReady`** without emitting `WaitPhoneNumber` — so no prompts. The command
wrappers exploit this:

- **`login`** is the only command permitted to *interactively* satisfy
  `WaitPhoneNumber`/`WaitCode`/`WaitPassword` (prompt on a TTY). If TDLib jumps straight to
  `Ready`, it prints `{"ok":true,"user":{…},"already":true}` and exits without prompting —
  **idempotent**.
- **All other commands** drive the same startup, but if any state other than `Ready` requires
  input, they do **not** prompt (an agent runs them head-less; a stdin prompt would hang).
  They emit `{"error":"not_authorized","hint":"run: tgcurl login"}` on stderr, exit non-zero.
- **Session invalidation** (revoked / logged out elsewhere): non-login commands report
  `not_authorized`; the user re-runs `login`, which re-enters the flow. `logout` sends `logOut`
  and clears the database directory.

**Logging & stream discipline.** TDLib is verbose by default and writes its log to **stderr** —
the same stream the interactive `login` prompts use (stdout is reserved for the JSON result, so
it stays pipe-clean for `jq`). Left alone, the log buries the phone/code prompts and the user
never sees them. So on client init we call `setLogVerbosityLevel(0)` (fatal-only) to keep stderr
clean; setting `TGCURL_DEBUG` to any non-empty value restores a verbose trace for debugging.
Rule of thumb: **stdout = JSON only; prompts and logs = stderr.**

---

## Peer identification — `chat_id` is primary

**Why not `<@username|phone>` as the sole input.** TDLib makes that fragile:

- `searchPublicChat` resolves **only public usernames** ("only private chats, supergroups and
  channels can be public"). An ordinary address-book contact **without a public @username
  cannot be resolved this way at all**.
- Phone→chat has no direct method: it requires `getContacts` → find the user with that phone →
  `createPrivateChat(user_id)` → chat_id, and only works if the contact is saved.
- The one universally stable identifier is the **`chat_id`** (int64), which every action method
  (`sendMessage`, `getChatHistory`) consumes directly.

**Decision: `chat_id` is the primary identifier; `@username` is a convenience input.** This
fits the Unix-way philosophy — list commands emit stable ids, the agent operates on them.

- **`contacts list` and `chats list` return `chat_id`** for every entry. The agent fetches the
  list once and addresses everything by the deterministic `chat_id` — no fragile per-call
  resolution.
- **`chat` / `send` / `contacts block` parse their identifier argument as:**
  - purely numeric → a `chat_id`, used directly (works once the chat is *warm* — see below);
  - starts with `@` → `searchPublicChat` (public usernames / channels / supergroups);
  - anything else → `{"error":"unresolvable","hint":"use chat_id from 'contacts list' /
    'chats list', or a public @username"}`.
- **Phone is not a direct input** to `chat`/`send`. Instead `contacts list` exposes the
  `phone → chat_id` mapping and the agent does the matching itself.

### Warming — why a raw `chat_id` needs the chat to be known first

A numeric `chat_id` is **not enough on its own** to address a chat. Telegram's MTProto requires
server-issued peer data (an access hash) for every peer; TDLib deliberately refuses to send using
only a fabricated id — otherwise anyone could message arbitrary users by iterating ids
([tdlib/td#88](https://github.com/tdlib/td/issues/88#issuecomment-640638710)). That peer data
comes **only from the server**: via an incoming update, `searchPublicChat`, or a chat-list fetch.

A chat is **"warm"** for a given `chat_id` once TDLib holds its peer data. Two ways to get there:

- **Same process:** the command itself fetched the chat — e.g. `send @username` goes through
  `searchPublicChat`, which warms the chat before sending.
- **Across processes:** because `use_message_database=true`, the chat/peer cache fetched by *any*
  earlier command persists on disk (flushed by the graceful `close` in `TdClient`'s destructor).
  So `chats list` / `contacts list` warms every chat it returns, and a later `send <chat_id>` in a
  fresh process finds the chat in the cache and works with no extra step (tdlib/td#88).

Practical consequence: a `chat_id` you just got from `chats list` / `contacts list` is warm and
usable. A `chat_id` for a chat this session has *never* fetched (all databases were empty) is
cold — run a `list` first, or address it by `@username`. This is inherent to TDLib's design (it
assumes a persistent client DB), not a tgcurl limitation.

---

## Credentials & storage layout

Single directory `~/.config/tgcurl/` (override via `TGCURL_CONFIG_DIR`), following XDG:

| Path                            | Contents                                                        | Written by            | Perms |
|---------------------------------|-----------------------------------------------------------------|-----------------------|-------|
| `config.json`                   | `{ "api_id", "api_hash" }` from my.telegram.org. Long-lived.    | `login` (once)        | 0600  |
| `td.db/` (= database_directory) | TDLib's encrypted session database + key — the persisted token. | TDLib, automatically  | 0700  |

Neither is recreated if valid. `config.json` is read on every run to feed `setTdlibParameters`;
`td.db/` keeps the account logged in across runs.

---

## CLI surface

All output is JSON. Identifier args (`<id>`) follow the resolution rules above.

| Command                                   | Behaviour                                                                                               |
|-------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `tgcurl login`                            | Idempotent. Prompts for api_id/api_hash if config absent, then phone/code/2FA on a TTY. `{"ok":true,"user":{…}}`. |
| `tgcurl logout`                           | `logOut` + clear `td.db/`. `{"ok":true}`.                                                               |
| `tgcurl status`                           | Session diagnostic, never prompts. `{"authorized":true,"user":{…}}` or `{"authorized":false,…}` — both exit 0: "not logged in" is the answer, not an error. |
| `tgcurl contacts list [--limit N] [--offset N]` | `getContacts` → `[{user_id, chat_id, username, phone, first_name, last_name}]`. `chat_id` is the key field. One page per call (default 100, max 1000); `--offset` skips the first N — the whole address book never lands in one response. |
| `tgcurl chats list [--limit N] [--offset N] [--unread]` | `getChats` → `[{chat_id, title, type, username, unread_count, last_message}]` for recent dialogs (groups/channels too). `--unread` keeps only chats with unread messages — the agent's "what needs attention" view. Paged by recency: `--offset` skips the first N of the raw list (counted before the unread filter, so pages stay stable); `offset + limit` is capped at 1000. |
| `tgcurl contacts new <phone> <first> [last]` | `importContacts`.                                                                                    |
| `tgcurl contacts block <id>`              | resolve → `setMessageSenderBlockList` (block).                                                          |
| `tgcurl chat "<id>" [--last N] [--before <msg_id>] [--all]` | resolve → `getChatHistory(limit=N)` → `[{id, date, is_outgoing, sender_id, type, text, reply_to_message_id}]`, newest-first. `type` tags the content (`text`, `photo`, `voice_note`, …); `text` is the text or media caption. Service/system messages (joins, pins, "X joined Telegram", …) are filtered out unless `--all`; only user-authored content reaches the agent. `--before` pages backwards: only messages older than the given id (pass the smallest id of the previous page). |
| `tgcurl send "<id>" "<text>" [--reply-to <msg_id>]` | resolve → `sendMessage` (`inputMessageText`, optional `inputMessageReplyToMessage`), then wait for the server ack (see *Asynchrony discipline*). `{"ok":true,"message_id":…}`. |
| `tgcurl search "<query>" [--chat <id>] [--limit N] [--offset <cursor>]` | `--chat` → `searchChatMessages`; otherwise `searchMessages` over the main chat list. `{"total_count":…,"next_offset":"…","messages":[…]}` in the shared message shape plus `chat_id`, newest-first. One page per call (default 20, max 100); `next_offset` is the cursor for the next page (`""` = no more) — pass it back via `--offset`. In-chat it is a message id, globally an opaque server string; callers just echo it. |
| `tgcurl sendfile "<id>" "<path>" ["<caption>"]` | resolve → `sendMessage` (`inputMessageDocument` over `inputFileLocal`); the ack wait covers the whole upload (minutes-scale budget). `{"ok":true,"message_id":…}`. |
| `tgcurl read "<id>"`                      | resolve → newest message → `viewMessages(force_read)`: clears the chat's unread counter. `{"ok":true,"read_up_to":…}`. |
| `tgcurl -mcp`                             | Long-running MCP stdio server exposing the same commands as tools (see *MCP mode*).                     |

### One registry, two front-ends

Every command is declared exactly once, in **`src/commands/registry.cpp`** — its CLI shape
(command word + subcommand), its MCP tool name/description/parameter schema, and the handler
(`optional<Error> fn(const Args&, std::ostream&)`; handlers write their JSON result to the
given stream instead of assuming stdout, so any front-end can capture it). `main.cpp` derives
its dispatch table from the registry; the MCP server derives `tools/list` and the
`tools/call` argument mapping from the same entries. **Adding a command = one handler + one
`CommandSpec`; it appears in both the CLI and MCP automatically**, and cannot drift between
them. `tests/test_registry.cpp` checks the invariants (unique tool/CLI names, schema shape,
argument mapping).

---

## MCP mode — `tgcurl -mcp`

The same binary doubles as an **MCP (Model Context Protocol) server** so agent runtimes
(Claude Code, etc.) can call Telegram primitives as first-class tools instead of shelling out.

- **Transport:** MCP's stdio transport — JSON-RPC 2.0, one message per line; requests on
  stdin, responses on stdout. This is why the "stdout = JSON only" rule matters doubly here:
  protocol traffic owns stdout, logs/prompts stay on stderr — including the human-facing
  "MCP server ready" startup notice, so a manual `tgcurl -mcp` isn't a silent terminal but
  the protocol stream stays clean.
- **Surface:** implements `initialize`, `ping`, `tools/list`, `tools/call`. No resources/
  prompts/notifications — tgcurl's capabilities are all verbs. Parsing uses the same in-house
  `json_in.h` the config loader uses.
- **Tools = registry entries.** Each `CommandSpec` with a non-empty tool name becomes a tool
  (`contacts_list`, `contacts_new`, `contacts_block`, `chats_list`, `chat_history`,
  `send_message`); `tools/call` maps the named JSON arguments onto the CLI argv shape and
  runs the *identical* handler, capturing its JSON output as the tool result text.

### CLI-only commands

The **session lifecycle** belongs to the human, not the agent, so these commands have an
empty tool name in the registry and never appear over MCP:

| Command  | Why CLI-only                                                                        |
|----------|-------------------------------------------------------------------------------------|
| `login`  | Must prompt for phone/code/2FA on a TTY; a head-less front-end cannot satisfy that. |
| `logout` | Destroys the session every front-end (including the MCP server itself) depends on; only a human re-running `login` can restore it. An agent must never be able to lock itself — and its owner — out. |
| `status` | The human-side diagnostic for the above ("am I logged in, and as whom") — used when setting the session up for an agent, not by the agent. |

The session is created once by a human (`tgcurl login`), checked with `tgcurl status`, and
then the MCP server just uses it. Everything *else* in the registry is exposed as a tool —
new commands are agent-facing by default.
- **Errors:** protocol-level problems (unknown tool, missing/extra arguments) are JSON-RPC
  errors (`-32602` …); a command failure is a *tool result* with `isError:true` carrying the
  same `{"error","hint"}` JSON the CLI would print on stderr — the agent reads it and adapts,
  the session keeps serving.
- **Sessions stay one-shot.** Each `tools/call` opens and closes its own `TdClient` exactly
  like a CLI invocation (same auth path, same graceful close/flush). Slightly slower per call,
  but the CLI and MCP semantics — including the asynchrony discipline and the not_authorized
  behavior — stay provably identical, and a wedged TDLib client can't poison a long-lived
  server.

---

## Project structure

```
CMakeLists.txt              // links Td::TdStatic (find_package(Td) or add_subdirectory(td))
src/main.cpp                // dispatch derived from the registry; -mcp entry; JSON error wrapper
src/tdclient.h/.cpp         // ClientManager wrapper: create, send/receive, sync send_query()
src/auth.h/.cpp             // authorization state-machine; interactive vs headless modes
src/config.h/.cpp           // load/save config.json; resolve config dir (XDG + TGCURL_CONFIG_DIR)
src/resolve.h/.cpp          // resolveId(arg) -> chat_id: numeric passthrough / @username / error
src/send_confirm.h          // SendConfirmation: wait-for-server-ack rule for sendMessage
src/message_render.h        // shared message->JSON shape (type/text/caption/reply)
src/mcp.h/.cpp              // MCP stdio server: JSON-RPC loop over the registry
src/commands/registry.h/.cpp// THE command table: CLI shape + MCP tool + handler, once per command
src/commands/contacts.cpp   // list / new / block
src/commands/chats.cpp      // chats list (getChats)
src/commands/chat.cpp       // history -> JSON
src/commands/send.cpp       // send message (+ ack wait)
src/commands/sendfile.cpp   // send a local file as a document (+ upload ack)
src/commands/search.cpp     // message search: per-chat / global
src/commands/read.cpp       // mark a chat as read (viewMessages force_read)
src/json_out.h              // JSON writer: emit(...) / emit_error(...) + exit code
src/json_in.h/.cpp          // strict minimal JSON parser (config.json, MCP requests)
README.md                   // build steps, my.telegram.org registration, agent-usage examples
```

## Build

- **CMake ≥ 3.10, C++17.** Links `Td::TdStatic`.
- **TDLib ≥ 1.8.63 required.** Telegram rejects login on TDLib 1.8.0's old MTProto layer with
  `406 UPDATE_APP_TO_LOGIN`; a recent build is mandatory, not optional. On Fedora this means the
  copr `stevenlin/tdlib-master` (a master snapshot), not the base-repo 1.8.0. `td_api` changed
  between the two: `setTdlibParameters` is now flat and carries `database_encryption_key` (so the
  former `WaitEncryptionKey` auth step is gone), `user`/`supergroup` expose a `usernames` object
  instead of a single `username`, `importContacts` takes `importedContact`, `inputMessageText`
  takes `linkPreviewOptions`, and blocking uses `setMessageSenderBlockList(blockListMain)`.
- **TDLib build prerequisites:** C++17 compiler (GCC ≥ 7 / Clang ≥ 5), OpenSSL, zlib, gperf,
  CMake.
- **TDLib provisioning:** always the prebuilt package, never an in-tree compile. Native
  builds: a system-installed TDLib (`find_package(Td)`; Fedora — the copr above; Debian ships
  no TDLib package at all). The `Dockerfile` follows the same rule: a pinned Fedora tag whose
  build stage installs the copr `tdlib-static` RPM at an exact pinned version (NEVR) and
  compiles only tgcurl; the runtime stage is the identical Fedora tag plus the single
  static-TDLib binary, with the session under a `/data` volume (`TGCURL_CONFIG_DIR=/data`).
  See README → *Docker*.
