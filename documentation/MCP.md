# MCP: build and play patches with an AI assistant

Espressynth can be driven by a language model (Claude Code, Claude Desktop or any other MCP client):
"Build me a bass with a sawtooth, a 24 dB low-pass and an ADSR, then play a scale."

## How it works

```
AI assistant ── MCP (HTTP) ──▶ ESP32-P4  /mcp ── WebSocket ──▶ web editor (browser tab)
                                    ▲                              │ carries out the tools
                                    └──── POST /api/mcp/reply ◀────┘
```

The device only passes messages along. Every MCP request (`initialize`, `tools/list`, `tools/call`) is
answered by the **web editor** that is open in a browser: it knows all widgets, ports and cable rules, and
it shows what the assistant does while it happens. The sound is made by the ESP32 as usual.

**An editor tab must be open.** Without one the device answers every request with
`Editor not open. Open the Espressynth web editor in a browser ...`. With several tabs, the one that was
shown most recently is used.

## Switch it on

1. Open the editor, then **File > Settings > MCP (AI assistants)** and tick **Allow MCP access**.
2. Copy the command that appears and run it in a terminal, for example for Claude Code:

   ```
   claude mcp add --transport http espressynth http://<device address>/mcp --header "Authorization: Bearer <token>"
   ```

MCP is **off by default**. A request needs the token; **New token** makes another one and locks out the old.

**Security:** the token is the only protection, and the rest of the device's web API has no login. Use it
on a network you trust, and do not share the token. Anyone with it can build, overwrite and delete patches.
Wi-Fi and device settings cannot be changed through MCP.

## Tools

| Tool | What it does |
|------|--------------|
| `list_widget_types`, `describe_widget` | Widget types by category; exact port ids and a help text |
| `get_graph`, `get_widget`, `get_status` | Patch (widgets, cables), one widget with its parameters, device status |
| `new_preset` | New unsaved preset (`default` template = a playable voice, `empty`) |
| `add_widget`, `remove_widget`, `move_widget` | Place, delete and move widgets |
| `connect`, `disconnect` | Cables between ports; the same rules as dragging a cable |
| `set_params` | Set widget parameters (names from `get_widget`; CV-style values are 0 to 10 V) |
| `play_note`, `play_sequence` | Play notes on the device |
| `list_presets`, `load_preset`, `save_preset`, `rename_preset`, `delete_preset` | Preset management |

Safety rules built into the tools:

- Building starts with `new_preset`. While a saved preset is loaded, edit tools refuse to run (every edit
  would be saved into it) unless they get `edit_loaded: true`.
- `save_preset` only replaces an existing preset with `overwrite: true`.
- `delete_preset` needs `confirm: true`.

## Example

> "Load the default template, add a low-pass filter between the oscillator and the output, set the cutoff
> low, play a bass line, and save it as 'Wobble Bass'."

## For developers

- Device: `mcp_post_handler`, `mcp_reply_handler` and the `mcpHello` WebSocket message in
  `main/features/synth/synth_web_api.hpp`. `/mcp` is an async request (`httpd_req_async_handler_begin`)
  because the HTTP server handles one request at a time and the editor's answer arrives as another request.
  An unanswered request ends after 30 seconds with a JSON-RPC error.
- Settings: `mcpEnabled` and `mcpToken` in `DeviceSettings.json` (`main/audio/engine/DeviceSettings.h`).
- Editor: `data/static/js/mcp-tools.js` (protocol and tools); the hooks into the editor are at the end of
  `data/static/js/main.js`.
