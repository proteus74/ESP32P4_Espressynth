// MCP host inside the web editor.
//
// The device (synth_web_api.hpp, mcp_post_handler) answers no MCP request itself: it forwards each
// JSON-RPC message to the editor tab that registered here ("mcpHello") and sends the editor's answer
// back to the client. So this module speaks MCP (initialize, tools/list, tools/call) and carries the
// tools out with the editor's own functions - the same cable rules, the same widget definitions.
//
// main.js hands in `api` (see startMcpHost call at the end of main.js).

const PROTOCOL_VERSIONS = ["2025-06-18", "2025-03-26", "2024-11-05"];

const INSTRUCTIONS = `Espressynth is a modular synthesizer. A patch is a graph of widgets (modules) joined by cables.
Typical order: new_preset with template "default" (a playable voice: midi-in, oscillator, adsr, outputs), then add_widget (for example a filter), connect, set_params, play_note, then save_preset.
The "empty" template has only the To Master widget and the FX page input; you must add a midi-in and an output yourself.
Use describe_widget before wiring a type you do not know: it lists the exact port ids.
Cables run from an output port to an input port. Control voltages are 0-10 V, audio is -1..1.
Most sound goes through the "To Master" widget (type audio-output-link, always present) into the FX page's output widget.
Changes are saved into the loaded preset automatically; new_preset starts a fresh unsaved one, so build there and call save_preset at the end.`;

const text = (value) => ({ content: [{ type: "text", text: typeof value === "string" ? value : JSON.stringify(value, null, 2) }] });

class ToolError extends Error {}

export function createMcpHost(api) {
  let session = "";
  let banner = null;
  let bannerTimer = null;
  let chain = Promise.resolve();

  function showBanner(message) {
    if (!banner) {
      banner = document.createElement("div");
      banner.style.cssText = "position:fixed;left:16px;bottom:16px;z-index:100000;padding:8px 14px;border-radius:10px;" +
        "background:rgba(14,165,233,0.92);color:#fff;font:600 13px system-ui,sans-serif;pointer-events:none;transition:opacity .3s";
      document.body.appendChild(banner);
    }
    banner.textContent = `MCP: ${message}`;
    banner.style.opacity = "1";
    clearTimeout(bannerTimer);
    bannerTimer = setTimeout(() => { banner.style.opacity = "0"; }, 2500);
  }

  // ---------------------------------------------------------------- helpers
  function widgetSummary(id, { withParams = false } = {}) {
    const info = api.widgetInfo(id);
    if (!info) throw new ToolError(`There is no widget "${id}". Use get_graph to see the ids.`);
    return withParams ? { ...info, params: api.getParams(id) } : info;
  }

  function requireEditable(args) {
    const name = api.currentPresetName();
    if (name !== null && args.edit_loaded !== true) {
      throw new ToolError(`The saved preset "${name}" is loaded, and every edit would be saved into it. ` +
        "Call new_preset first to work on a fresh preset (or pass edit_loaded: true to edit this one on purpose).");
    }
  }

  function requireString(args, key) {
    if (typeof args[key] !== "string" || args[key] === "") throw new ToolError(`"${key}" is required (a text).`);
    return args[key];
  }

  function checkPort(widgetId, portId, dir) {
    const info = widgetSummary(widgetId);
    const ports = info.ports.filter((port) => port.dir === dir);
    if (!ports.some((port) => port.id === portId)) {
      throw new ToolError(`Widget ${widgetId} (${info.type}) has no ${dir === "out" ? "output" : "input"} port "${portId}". ` +
        `Valid ${dir === "out" ? "outputs" : "inputs"}: ${ports.map((port) => port.id).join(", ") || "none"}.`);
    }
  }

  // ---------------------------------------------------------------- tools
  const tools = [
    {
      name: "list_widget_types",
      description: "All widget types that can be added, grouped by category (oscillators, filters, envelopes, effects, ...).",
      inputSchema: { type: "object", properties: {} },
      run: () => text(api.widgetCategories().map((category) => ({
        category: category.label,
        types: category.types.map((type) => ({ type, label: api.widgetTypes()[type]?.label || type })),
      }))),
    },
    {
      name: "describe_widget",
      description: "Ports (exact ids to use with connect) and a short help text for one widget type.",
      inputSchema: { type: "object", properties: { type: { type: "string" } }, required: ["type"] },
      run: async (args) => {
        const type = requireString(args, "type");
        const def = api.widgetTypes()[type];
        if (!def) throw new ToolError(`Unknown widget type "${type}". See list_widget_types.`);
        let help = "";
        try {
          const res = await fetch(`/help/widgets/${encodeURIComponent(type)}.html`);
          if (res.ok) help = (await res.text()).replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim().slice(0, 1500);
        } catch (err) { /* no help page */ }
        const portList = (ports) => (ports || []).map((port) => ({ id: port.id, label: port.label }));
        return text({ type, label: def.label, inputs: portList(def.inputs), outputs: portList(def.outputs),
          note: "Some widgets add ports when you add them (sliders, outputs per layer); add_widget returns the real list.", help });
      },
    },
    {
      name: "get_graph",
      description: "The patch in the editor: pages, widgets (id, type, position, ports) and cables.",
      inputSchema: { type: "object", properties: {} },
      run: () => text(api.graph()),
    },
    {
      name: "get_widget",
      description: "One widget with its current parameters (names and values as set_params expects them).",
      inputSchema: { type: "object", properties: { id: { type: "string" } }, required: ["id"] },
      run: (args) => text(widgetSummary(requireString(args, "id"), { withParams: true })),
    },
    {
      name: "get_status",
      description: "Loaded preset, widget count, CPU load and memory of the device.",
      inputSchema: { type: "object", properties: {} },
      run: () => text(api.status()),
    },
    {
      name: "new_preset",
      description: "Start a new unsaved preset. template \"default\" is a playable starting voice (MIDI in, oscillator, ADSR, all outputs wired); \"empty\" (the default value) has only the permanent To Master and FX input widgets.",
      inputSchema: { type: "object", properties: { template: { type: "string", enum: ["empty", "default"] } } },
      run: async (args) => {
        await api.newPreset(args.template === "default" ? "default" : "empty");
        return text({ ok: true, graph: api.graph() });
      },
    },
    {
      name: "add_widget",
      description: "Add a widget. Returns its id and ports. Optional page (generator or fx), position x/y, label and params.",
      inputSchema: {
        type: "object",
        properties: {
          type: { type: "string" }, page: { type: "string" }, x: { type: "number" }, y: { type: "number" },
          label: { type: "string" }, params: { type: "object" }, edit_loaded: { type: "boolean" },
        },
        required: ["type"],
      },
      run: (args) => {
        requireEditable(args);
        const type = requireString(args, "type");
        if (!api.widgetTypes()[type]) throw new ToolError(`Unknown widget type "${type}". See list_widget_types.`);
        const id = api.addWidget(type, args.page, args.x, args.y, args.label);
        if (args.params && typeof args.params === "object") api.applyParams(id, args.params);
        api.commit();
        return text({ ok: true, widget: widgetSummary(id, { withParams: true }) });
      },
    },
    {
      name: "remove_widget",
      description: "Delete a widget and its cables.",
      inputSchema: { type: "object", properties: { id: { type: "string" }, edit_loaded: { type: "boolean" } }, required: ["id"] },
      run: (args) => {
        requireEditable(args);
        const id = requireString(args, "id");
        widgetSummary(id);
        if (!api.removeWidget(id)) throw new ToolError(`Widget ${id} cannot be removed (the To Master widget is permanent).`);
        api.commit();
        return text({ ok: true });
      },
    },
    {
      name: "move_widget",
      description: "Move a widget to x/y (pixels on its page).",
      inputSchema: { type: "object", properties: { id: { type: "string" }, x: { type: "number" }, y: { type: "number" }, edit_loaded: { type: "boolean" } }, required: ["id", "x", "y"] },
      run: (args) => {
        requireEditable(args);
        widgetSummary(requireString(args, "id"));
        api.moveWidget(args.id, Number(args.x), Number(args.y));
        api.commit();
        return text({ ok: true });
      },
    },
    {
      name: "connect",
      description: "Cable from an output port to an input port. A control input takes one cable (a new one replaces the old); audio inputs can take several.",
      inputSchema: {
        type: "object",
        properties: { from_widget: { type: "string" }, from_port: { type: "string" }, to_widget: { type: "string" }, to_port: { type: "string" }, edit_loaded: { type: "boolean" } },
        required: ["from_widget", "from_port", "to_widget", "to_port"],
      },
      run: (args) => {
        requireEditable(args);
        const { from_widget: fromWidget, from_port: fromPort, to_widget: toWidget, to_port: toPort } = args;
        checkPort(requireString(args, "from_widget"), requireString(args, "from_port"), "out");
        checkPort(requireString(args, "to_widget"), requireString(args, "to_port"), "in");
        const result = api.connect(fromWidget, fromPort, toWidget, toPort);
        if (!result.ok) throw new ToolError(result.error);
        return text({ ok: true, connection: result.id });
      },
    },
    {
      name: "disconnect",
      description: "Remove a cable by its id (see get_graph).",
      inputSchema: { type: "object", properties: { connection_id: { type: "string" }, edit_loaded: { type: "boolean" } }, required: ["connection_id"] },
      run: (args) => {
        requireEditable(args);
        if (!api.disconnect(requireString(args, "connection_id"))) throw new ToolError(`There is no cable "${args.connection_id}".`);
        return text({ ok: true });
      },
    },
    {
      name: "set_params",
      description: "Set parameters of a widget, for example {\"attack\":0.02} or {\"cutoff\":800}. Names and ranges: call get_widget first.",
      inputSchema: { type: "object", properties: { id: { type: "string" }, params: { type: "object" }, edit_loaded: { type: "boolean" } }, required: ["id", "params"] },
      run: (args) => {
        requireEditable(args);
        const id = requireString(args, "id");
        const info = widgetSummary(id);
        if (!args.params || typeof args.params !== "object") throw new ToolError('"params" is required (an object).');
        const known = Object.keys(api.getParams(id));
        if (known.length === 0) throw new ToolError(`Widget ${id} (${info.type}) has no settable parameters.`);
        const unknown = Object.keys(args.params).filter((key) => !known.includes(key));
        if (unknown.length) throw new ToolError(`Unknown parameter(s) for ${info.type}: ${unknown.join(", ")}. Known: ${known.join(", ")}.`);
        api.applyParams(id, args.params);
        api.commit();
        return text({ ok: true, params: api.getParams(id) });
      },
    },
    {
      name: "play_note",
      description: "Play a note on the device (MIDI note 0-127, velocity 1-127, length in ms). The sound comes from the ESP32.",
      inputSchema: { type: "object", properties: { note: { type: "integer" }, velocity: { type: "integer" }, duration_ms: { type: "integer" }, channel: { type: "integer" } }, required: ["note"] },
      run: async (args) => {
        const note = Math.round(Number(args.note));
        if (!(note >= 0 && note <= 127)) throw new ToolError('"note" must be 0-127.');
        const duration = Math.min(10000, Math.max(20, Math.round(Number(args.duration_ms) || 500)));
        api.sendNote(true, note, Math.min(127, Math.max(1, Math.round(Number(args.velocity) || 100))), args.channel | 0);
        await new Promise((resolve) => setTimeout(resolve, duration));
        api.sendNote(false, note, 0, args.channel | 0);
        return text({ ok: true });
      },
    },
    {
      name: "play_sequence",
      description: "Play notes: [{note, at_ms, duration_ms, velocity}] (at_ms from the start, at most 30 s).",
      inputSchema: { type: "object", properties: { notes: { type: "array", items: { type: "object" } }, channel: { type: "integer" } }, required: ["notes"] },
      run: async (args) => {
        if (!Array.isArray(args.notes) || args.notes.length === 0 || args.notes.length > 256) throw new ToolError('"notes" needs 1-256 entries.');
        let end = 0;
        for (const entry of args.notes) {
          const note = Math.round(Number(entry.note));
          if (!(note >= 0 && note <= 127)) throw new ToolError("Every note must be 0-127.");
          const at = Math.max(0, Math.round(Number(entry.at_ms) || 0));
          const length = Math.min(10000, Math.max(20, Math.round(Number(entry.duration_ms) || 300)));
          const velocity = Math.min(127, Math.max(1, Math.round(Number(entry.velocity) || 100)));
          end = Math.max(end, at + length);
          if (end > 30000) throw new ToolError("The sequence is longer than 30 seconds.");
          setTimeout(() => api.sendNote(true, note, velocity, args.channel | 0), at);
          setTimeout(() => api.sendNote(false, note, 0, args.channel | 0), at + length);
        }
        await new Promise((resolve) => setTimeout(resolve, end + 100));
        return text({ ok: true, played: args.notes.length });
      },
    },
    {
      name: "list_presets",
      description: "Names of the presets stored on the device and the loaded one (null = unsaved).",
      inputSchema: { type: "object", properties: {} },
      run: async () => text({ presets: await api.listPresets(), loaded: api.currentPresetName() }),
    },
    {
      name: "load_preset",
      description: "Load a stored preset (it plays right away). Edits afterwards are saved into it.",
      inputSchema: { type: "object", properties: { name: { type: "string" } }, required: ["name"] },
      run: async (args) => {
        const name = requireString(args, "name");
        if (!(await api.listPresets()).includes(name)) throw new ToolError(`There is no preset "${name}". See list_presets.`);
        await api.loadPreset(name);
        return text({ ok: true, graph: api.graph() });
      },
    },
    {
      name: "save_preset",
      description: "Save the editor's patch under a name. An existing preset is only replaced with overwrite: true.",
      inputSchema: { type: "object", properties: { name: { type: "string" }, overwrite: { type: "boolean" } }, required: ["name"] },
      run: async (args) => {
        const name = requireString(args, "name");
        const problem = api.presetNameProblem(name);
        if (problem) throw new ToolError(`${problem}: "${name}". Use letters, digits, spaces, - and _.`);
        const exists = (await api.listPresets()).includes(name);
        if (exists && args.overwrite !== true) throw new ToolError(`A preset named "${name}" already exists. Pass overwrite: true to replace it, or pick another name.`);
        if (!(await api.saveAs(name))) throw new ToolError("The device could not save the preset.");
        return text({ ok: true, saved_as: name });
      },
    },
    {
      name: "rename_preset",
      description: "Rename a stored preset.",
      inputSchema: { type: "object", properties: { name: { type: "string" }, new_name: { type: "string" } }, required: ["name", "new_name"] },
      run: async (args) => {
        const newName = requireString(args, "new_name");
        const problem = api.presetNameProblem(newName);
        if (problem) throw new ToolError(`${problem}: "${newName}".`);
        const result = await api.renamePreset(requireString(args, "name"), newName);
        if (!result.ok) throw new ToolError(result.error);
        return text({ ok: true });
      },
    },
    {
      name: "delete_preset",
      description: "Delete a stored preset from the device for good. Needs confirm: true.",
      inputSchema: { type: "object", properties: { name: { type: "string" }, confirm: { type: "boolean" } }, required: ["name", "confirm"] },
      run: async (args) => {
        const name = requireString(args, "name");
        if (args.confirm !== true) throw new ToolError("Deleting cannot be undone. Ask the user, then call again with confirm: true.");
        const result = await api.deletePreset(name);
        if (!result.ok) throw new ToolError(result.error);
        return text({ ok: true });
      },
    },
  ];

  // ---------------------------------------------------------------- JSON-RPC
  const rpcError = (id, code, message) => ({ jsonrpc: "2.0", id, error: { code, message } });
  const rpcResult = (id, result) => ({ jsonrpc: "2.0", id, result });

  async function handle(rpc) {
    if (!rpc || rpc.jsonrpc !== "2.0" || typeof rpc.method !== "string") return rpcError(rpc?.id ?? null, -32600, "Invalid Request");
    const { id, method } = rpc;
    const params = rpc.params || {};
    if (id === undefined) return null; // notification (initialized, cancelled, ...)
    switch (method) {
      case "initialize": {
        const asked = params.protocolVersion;
        return rpcResult(id, {
          protocolVersion: PROTOCOL_VERSIONS.includes(asked) ? asked : PROTOCOL_VERSIONS[0],
          capabilities: { tools: { listChanged: false } },
          serverInfo: { name: "espressynth", version: "1.0" },
          instructions: INSTRUCTIONS,
        });
      }
      case "ping":
        return rpcResult(id, {});
      case "tools/list":
        return rpcResult(id, { tools: tools.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })) });
      case "tools/call": {
        const tool = tools.find((candidate) => candidate.name === params.name);
        if (!tool) return rpcError(id, -32602, `Unknown tool: ${params.name}`);
        showBanner(tool.name);
        try {
          return rpcResult(id, await tool.run(params.arguments || {}));
        } catch (err) {
          const message = err?.message || String(err);
          return rpcResult(id, { ...text(message), isError: true });
        }
      }
      case "resources/list":
        return rpcResult(id, { resources: [] });
      case "prompts/list":
        return rpcResult(id, { prompts: [] });
      default:
        return rpcError(id, -32601, `Method not found: ${method}`);
    }
  }

  async function reply(id, response) {
    const body = JSON.stringify(response);
    for (let attempt = 0; attempt < 3; attempt++) {
      try {
        const res = await fetch(`/api/mcp/reply?id=${id}&s=${encodeURIComponent(session)}`, {
          method: "POST", headers: { "Content-Type": "application/json" }, body,
        });
        if (res.ok || res.status === 404 || res.status === 403) return; // 404/403: nobody waits any more
      } catch (err) {
        console.warn("MCP reply failed", err);
      }
      await new Promise((resolve) => setTimeout(resolve, 300));
    }
  }

  return {
    // Offers this tab as the one that carries out MCP requests (the latest tab to ask wins).
    hello() { api.wsSend({ type: "mcpHello" }); },
    onMessage(message) {
      if (message.type === "mcpReady") {
        session = message.session || "";
        return;
      }
      if (message.type !== "mcpRequest") return;
      // One request at a time: tools change the same editor state.
      chain = chain.then(async () => {
        const response = await handle(message.rpc);
        if (message.id && response) await reply(message.id, response);
      }).catch((err) => console.warn("MCP request failed", err));
    },
  };
}
