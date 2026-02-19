# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires the sibling repo `../d-comms` to be present (CMake pulls it in via `add_subdirectory`). OpenGL and GLFW development libraries must be installed on the system.

```bash
cmake -B build          # configure (defaults to desktop platform)
cmake --build build     # compile; output binary: build/dui
```

Run the binary from its directory so it can resolve `basedir` correctly (it uses `/proc/self/exe` on Linux):

```bash
./build/dui
```

### Running multiple isolated instances

`run_instances.sh` builds the project and launches N independent clients in parallel, each in its own temp directory under `/tmp/dui_instance_*/`. All temp directories are deleted when the last window closes.

```bash
./run_instances.sh        # 2 instances (default)
./run_instances.sh 5      # 5 instances
```

The script copies the binary into each instance directory rather than symlinking it, because `resolve_basedir` reads `/proc/self/exe` which always resolves to the real file path — a symlink would make every instance share `build/` as its basedir. Each instance is also launched with its own directory as the working directory (`cd "$dir" && DCOMMS_HOST=127.0.0.1 exec "$dir/dui"`), so both `messages.db` and `registry.db` are fully isolated per instance. After launch the script cross-seeds each instance's `registry.db` entry into all other instances, so `sync_with_peers` can establish the first connection; gossip propagates further peers automatically.

### Platform variants

Pass `-DDUI_PLATFORM=<value>` at configure time. Supported values: `desktop` (default), `macos`, `ios`, `android`. The macOS/iOS/Android targets are stubs (`TODO` in CMakeLists.txt).

## Architecture

### Layered design

```
main_desktop.cpp          ← platform entry point + render loop
    └── app.cpp / app.h   ← platform-agnostic UI (two screens)
            └── d-comms (sibling repo, static lib: dcomms_core)
                    ├── proto.h / proto.c   — encrypted chat protocol
                    └── sync.h / sync.c     — LAN peer sync
```

**`app.h`** declares the four lifecycle hooks called by every platform entry point: `app_init`, `app_frame`, `app_shutdown`, `app_quit_requested`.

**`app.cpp`** owns all UI state (static globals) and implements two screens via `draw_chat_list()` / `draw_chat_view()`. A background `std::thread` (`g_sync_thread`) calls `sync_with_peers()` every 5 seconds.

**`main_desktop.cpp`** owns the GLFW window and OpenGL 3.3 core profile context, runs the ImGui frame loop, and calls into the `app_*` hooks.

**`src/tui.c`** is a self-contained terminal UI with its own `main()` that uses the same `proto.h`/`sync.h` API. It is **not wired into CMakeLists.txt** and must be compiled separately if needed.

### Data storage

- Chat sessions are saved as `{basedir}/chats/<name>.chat` (binary, loaded/saved via `proto_save_chat` / `proto_load_chat`).
- Message log: `messages.db` (relative path opened by `dcomms_core`, resolves to the instance working directory).
- Peer registry: `registry.db` (relative path, per-instance). Format is `host:port` per line. Set `DCOMMS_HOST` env var to advertise a non-loopback address for internet peers.

### Protocol flow

`proto_initialize` generates keys and returns a `set <key> <id>` credential string to share out-of-band. The other party calls `proto_join` with that string. After joining, both sides use `proto_send` / `proto_list` and messages are synced over LAN via `sync_with_peers`.

### ImGui usage notes

- `imgui.ini` is disabled (`io.IniFilename = nullptr`).
- All screens use `begin_fullscreen()` which pins a borderless window to the display size every frame.
- Per-participant message colouring is handled by `color_for(entity_id)` in `app.cpp`, which assigns colours sequentially (up to 8 unique IDs without collision, hash fallback beyond that).
