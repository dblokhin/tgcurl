# tgcurl

**`curl` for Telegram.** A deterministic, terminal-only, JSON-only command-line client for a
**personal Telegram account** (not a bot), built on Telegram's official
[TDLib](https://core.telegram.org/tdlib).

`tgcurl` is built to be **driven by scripts and AI agents**: every command does one thing,
prints JSON, and exits. There is no REPL, no interactive menus in the hot path, and no fuzzy
guessing — you get composable primitives that a program (or a person with `jq`) can rely on.

```console
$ tgcurl send 123456789 "deploy finished ✅"
{"ok":true,"message_id":184600002560,"chat_id":123456789}
```

> **Status:** all commands implemented (login/logout, contacts list/new/block, chats list,
> chat, send). Pure logic is covered by `ctest`; the network round-trip is verified manually
> against a real account (see *End-to-end verification* below). Design: [`DESIGN.md`](./DESIGN.md).

---

## Goals

- **Personal-account access over MTProto** — a real user session, not the Bot API.
- **Deterministic & Unix-way** — one command, one action, composable in pipelines.
- **Machine-first output** — JSON on stdout for every command; JSON errors on stderr with a
  non-zero exit code. Ideal for AI agents and automation.
- **Login once, reuse forever** — authenticate a single time; later runs never re-prompt and
  never block on stdin, so they are safe to run head-less.
- **Flexibility lives in the caller** — the tool exposes primitives (e.g. a contacts dump with
  stable `chat_id`s); the agent decides who to message.

## Non-goals

- No fuzzy display-name matching ("John Smith" → chat). List the contacts and let the caller
  match.
- No bot mode, no local message database beyond the session.
- No hand-rolled MTProto — we use the official TDLib.

---

## Install

### From a package (recommended)

Download the `.rpm` or `.deb` from the [releases page](https://github.com/dblokhin/tgcurl/releases),
or build them yourself (see *Release* below), then:

```console
# Fedora / RHEL
sudo dnf install ./tgcurl-0.1.0.x86_64.rpm

# Debian / Ubuntu
sudo apt install ./tgcurl_0.1.0_amd64.deb
```

The packaged binary is self-contained (TDLib, OpenSSL, zlib and the C++ runtime are statically
linked); the only runtime dependency is the system glibc, which every supported distro ships.

### Build from source

```console
make deps      # install build prerequisites (dnf or apt)
make build     # compile into build/tgcurl
sudo make install
```

You also need **TDLib** available to the build. See *Building* below.

---

## Quick start

`tgcurl` needs a Telegram **api_id / api_hash**. Register an application once at
<https://my.telegram.org/apps> — these identify your app, not your account.

```console
# 1. Log in (interactive, one time). Prompts for api_id/api_hash on first run,
#    then phone number, the login code, and your 2FA password if enabled.
#    (Prompts are written to stderr; only the JSON result goes to stdout.)
$ tgcurl login
{"ok":true,"user":{"user_id":42,"first_name":"Dmitriy","last_name":"","username":"dblokhin","phone":"15551234567"},"already":false}

# Running it again is a no-op — the session is reused, no prompts:
$ tgcurl login
{"ok":true,"user":{"user_id":42,"first_name":"Dmitriy","last_name":"","username":"dblokhin","phone":"15551234567"},"already":true}
```

Credentials and session live under `~/.config/tgcurl/` (override with `TGCURL_CONFIG_DIR`):
`config.json` (api_id/api_hash, `0600`) and `td.db/` (the TDLib session, `0700`).

---

## Commands

All output is JSON. The `<id>` argument is either a numeric **`chat_id`** (from `contacts list`
or `chats list`) or a public **`@username`**.

### Authentication

```console
$ tgcurl login       # idempotent; see Quick start
$ tgcurl logout      # end the session and clear ~/.config/tgcurl/td.db
{"ok":true}
```

### Contacts

```console
# List saved contacts. chat_id is the field you feed back into chat/send.
$ tgcurl contacts list
[{"user_id":42,"chat_id":42,"username":"alice","phone":"+15551234567","first_name":"Alice","last_name":"A"}]

# Add a contact by phone.
$ tgcurl contacts new "+15557654321" "Bob"
{"ok":true,"user_id":77,"chat_id":77}

# Block a chat/user (by chat_id or @username).
$ tgcurl contacts block @spammer
{"ok":true}
```

### Chats and messages

```console
# List recent dialogs (groups and channels too, not just contacts).
$ tgcurl chats list --limit 20
[{"chat_id":-100123,"title":"Dev Team","type":"supergroup","username":"devteam"}]

# Read the last N messages of a chat, newest first.
$ tgcurl chat 42 --last 3
[{"id":1846,"date":1751600000,"is_outgoing":false,"sender_id":42,"text":"hi"}]

# Send a message.
$ tgcurl send @devteam "build is green"
{"ok":true,"message_id":184600002560,"chat_id":-100123}
```

### For AI agents

Because names are never guessed, an agent resolves a person deterministically in two steps —
dump the list, match locally, then act on the stable `chat_id`:

```console
$ CHAT=$(tgcurl contacts list | jq -r '.[] | select(.username=="alice") | .chat_id')
$ tgcurl send "$CHAT" "your report is ready"
```

Errors are JSON on stderr with a non-zero exit, so they are easy to detect:

```console
$ tgcurl send "John Smith" "hi"; echo "exit=$?"
{"error":"unresolvable","hint":"use chat_id from 'contacts list' / 'chats list', or a public @username"}
exit=1
```

### MCP server mode

`tgcurl -mcp` serves the same commands as **MCP tools** over stdio (JSON-RPC 2.0), so agent
runtimes can call Telegram primitives natively instead of shelling out. Log in once with
`tgcurl login` first; then, e.g. for Claude Code:

```console
$ claude mcp add telegram -- tgcurl -mcp
```

Exposed tools: `contacts_list`, `contacts_new`, `contacts_block`, `chats_list`,
`chat_history`, `send_message`, `logout` (everything except the interactive `login`). Each
tool call is the same one-shot handler as the CLI subcommand; results and errors carry the
same JSON. See DESIGN.md → *MCP mode*.

---

## Testing & verification

Pure logic is covered by an in-tree test suite (no external framework) run via `ctest`:

```console
make test
```

It exercises JSON output/escaping, config path resolution and `config.json`
round-tripping (perms included), identifier classification (`classify`), the
head-less prompter contract, and CLI-level dispatch (unknown/again/usage,
head-less `login` not hanging, bad args and `unresolvable` failing before any
network). Anything that hits Telegram's servers is **not** unit-tested — it's
verified manually.

### End-to-end verification (real account)

The network path needs real credentials, so run this once by hand after a build:

```console
# 0. build links against TDLib
make build

# 1. get an api_id/api_hash at https://my.telegram.org/apps, then:
tgcurl login                       # phone + code (+ 2FA) -> {"ok":true,...,"already":false}
ls -l ~/.config/tgcurl             # config.json is 0600, td.db/ is 0700
tgcurl login                       # -> "already":true, no prompts (session reuse)

# 2. reads
tgcurl contacts list | jq .        # array; every entry has a chat_id
tgcurl chats list --limit 20 | jq. # array of {chat_id,title,type,username}

# 3. write + read back
tgcurl send "<chat_id>" "hello from tgcurl"   # -> {"ok":true,"message_id":...}
tgcurl send "@some_public" "hi"               # @username path via searchPublicChat
tgcurl chat "<chat_id>" --last 5 | jq .        # shows the sent message, newest-first

# 4. negative paths
tgcurl send "John Smith" "x"; echo $?          # {"error":"unresolvable",...}, exit 1
TGCURL_CONFIG_DIR=$(mktemp -d) tgcurl contacts list </dev/null; echo $?
                                               # no session -> error, exit 1, no stdin hang
```

---

## Building

`tgcurl` is C++17 and links against TDLib's native C++ API.

**Prerequisites** (installed by `make deps`): a C++17 compiler (GCC ≥ 7 / Clang ≥ 5), CMake ≥
3.10, gperf, and the OpenSSL and zlib development headers.

**TDLib** must also be available; the project's CMake finds it via `find_package(Td)`.

> **TDLib ≥ 1.8.63 is required.** The Fedora base-repo package is **1.8.0**, and Telegram
> **refuses to log in** on its old MTProto layer (`406: UPDATE_APP_TO_LOGIN`). You need a recent
> build.

- **Fedora (recommended, and what this project builds against):** install the master snapshot
  from the copr `stevenlin/tdlib-master` (unofficial):
  `sudo dnf copr enable stevenlin/tdlib-master && sudo dnf install tdlib-devel tdlib-static`
  (currently `1.8.63`). No source build needed; `make static` links `Td::TdStatic` from
  `tdlib-static`. Do **not** use the base-repo `tdlib` 1.8.0 — it cannot log in.
- **Other distros / newer TDLib:** build [tdlib/td](https://github.com/tdlib/td) from source
  (a recent `master`) and `make install` it once. Compiling TDLib from source is heavy (a long
  C++ build).

> Built and tested against **TDLib 1.8.63** (copr `stevenlin/tdlib-master`, MTProto layer 227).
> Between 1.8.0 and 1.8.63 several `td_api` types changed (flat `setTdlibParameters`, `usernames`
> object, `setMessageSenderBlockList`, `linkPreviewOptions`), so older TDLib will not build.

### Make targets

| Target          | What it does                                                              |
|-----------------|---------------------------------------------------------------------------|
| `make deps`     | Install build + dev tooling (clang-tools, cppcheck, valgrind, ninja, ccache; auto-detects `dnf` / `apt`). Install TDLib separately (see above). |
| `make build`    | Configure + compile into `build/tgcurl`. `BUILD_TYPE=Debug` for a debug build. |
| `make static`   | Build a self-contained binary (bundled TDLib/OpenSSL/zlib/libstdc++), then print its `ldd`. |
| `make test`     | Run the test suite via `ctest`.                                           |
| `make install`  | Install to `PREFIX` (default `/usr/local`; needs sudo for system prefixes). |
| `make release`  | Build the static binary and produce `.rpm` + `.deb` into `dist/`.         |
| `make clean` / `make distclean` | Remove build artifacts / everything.                     |

Packaging (`make release`) uses [nfpm](https://nfpm.goreleaser.com/) to emit both `.rpm` and
`.deb` from the single static binary — no `rpmbuild`/`dpkg` toolchain required. Override the
version with `make release VERSION=0.2.0`.

---

## License

[MIT](./LICENSE) © 2026 Blokhin Dmitriy. See [`AUTHORS`](./AUTHORS).
