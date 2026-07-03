# tgcurl — Design

`tgcurl` is a terminal-only utility for working with a **personal Telegram account** (not a
bot) over Telegram's MTProto protocol. Its distinguishing purpose: be a **deterministic,
Unix-way CLI that an AI agent can drive** to perform Telegram tasks — *"`curl` for Telegram."*

It exposes small, composable, **JSON-only** primitives; a calling agent combines them (e.g.
dump contacts, then decide which chat to message). Every invocation is one shot: connect, do
one thing, print JSON, exit.

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
- No local message cache / database beyond what the session needs.
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

---

## Authentication & session lifecycle

Auth is a **state machine** driven by `updateAuthorizationState`. TDLib emits states in order;
we react to each:

1. `authorizationStateWaitTdlibParameters` → `setTdlibParameters` with `api_id`, `api_hash`,
   the **`database_directory`** (where the session lives), `use_message_database=false`
   (one-shot tool needs no message cache), `use_secret_chats=false`, plus system/app version.
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
  - purely numeric → a `chat_id`, used directly (fast path, always works);
  - starts with `@` → `searchPublicChat` (public usernames / channels / supergroups);
  - anything else → `{"error":"unresolvable","hint":"use chat_id from 'contacts list' /
    'chats list', or a public @username"}`.
- **Phone is not a direct input** to `chat`/`send`. Instead `contacts list` exposes the
  `phone → chat_id` mapping and the agent does the matching itself.

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
| `tgcurl contacts list`                    | `getContacts` → `[{user_id, chat_id, username, phone, first_name, last_name}]`. `chat_id` is the key field. |
| `tgcurl chats list [--limit N]`           | `getChats` → `[{chat_id, title, type, username}]` for recent dialogs (groups/channels too).             |
| `tgcurl contacts new <phone> <first> [last]` | `importContacts`.                                                                                    |
| `tgcurl contacts block <id>`              | resolve → `setMessageSenderBlockList` (block).                                                          |
| `tgcurl chat "<id>" --last N`             | resolve → `getChatHistory(limit=N)` → `[{id, date, is_outgoing, sender_id, text}]`, newest-first.       |
| `tgcurl send "<id>" "<text>"`             | resolve → `sendMessage` (`inputMessageText`). `{"ok":true,"message_id":…}`.                             |

---

## Project structure

```
CMakeLists.txt              // links Td::TdStatic (find_package(Td) or add_subdirectory(td))
src/main.cpp                // subcommand dispatch, top-level arg parsing, JSON error wrapper
src/tdclient.h/.cpp         // ClientManager wrapper: create, send/receive, sync send_query()
src/auth.h/.cpp             // authorization state-machine; interactive vs headless modes
src/config.h/.cpp           // load/save config.json; resolve config dir (XDG + TGCURL_CONFIG_DIR)
src/resolve.h/.cpp          // resolveId(arg) -> chat_id: numeric passthrough / @username / error
src/commands/contacts.cpp   // list / new / block
src/commands/chats.cpp      // chats list (getChats)
src/commands/chat.cpp       // history -> JSON
src/commands/send.cpp       // send message
src/json_out.h              // emit(...) to stdout; emit_error(...) to stderr + exit code
README.md                   // build steps, my.telegram.org registration, agent-usage examples
```

## Build

- **CMake ≥ 3.10, C++17.** Links `Td::TdStatic`.
- **TDLib build prerequisites:** C++17 compiler (GCC ≥ 7 / Clang ≥ 5), OpenSSL, zlib, gperf,
  CMake.
- **TDLib provisioning:** deferred. Either a system-installed TDLib (`find_package(Td)`) or a
  vendored / from-source build (Dockerfile with static linking for a self-contained binary).
  Documented in the README once chosen.
