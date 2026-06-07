// Pure, deterministic config.toml serialization for the dashboard config editor.
//
// Renders a full easy-failover `config.toml` string from the editor's form
// state, covering every editable section per `docs/config-reference.md`. This
// module is intentionally pure (no I/O, no secrets) and BROWSER-SAFE: the editor
// in `web/app/page.tsx` builds the candidate TOML on the client, then POSTs the
// string to the dashboard's own /api/nodes/[id]/config route. The same form
// state always renders byte-for-byte the same string.
//
// Unlike SSH onboarding (`web/lib/onboarding/config.ts`), this editor CAN edit
// `[mutation_safety].allow_network_mutation`, so that flag is operator-controlled
// here and emitted from the form value.

/** Render a string value as a quoted TOML basic string. */
export function tomlString(value: string): string {
  // Escape backslashes, double quotes, and the control characters TOML basic
  // strings forbid unescaped. Keeps generated TOML valid for arbitrary input.
  // Mirrors the escaping approach in `web/lib/onboarding/config.ts`.
  const escaped = value
    .replace(/\\/g, "\\\\")
    .replace(/"/g, '\\"')
    .replace(/\n/g, "\\n")
    .replace(/\r/g, "\\r")
    .replace(/\t/g, "\\t");
  return `"${escaped}"`;
}

/** Render a boolean as a TOML literal. */
export function tomlBool(value: boolean): string {
  return value ? "true" : "false";
}

/** One peer row in the editor form. */
export type PeerFormValue = {
  id: string;
  address: string;
};

/**
 * The full editor form state. Every editable field of the config is represented
 * here as a concrete value (no "undefined means default" indirection): the
 * editor replaces the whole config on apply, so the serializer emits each
 * section explicitly and deterministically.
 *
 * `health.command` is the operator-re-entered health command. Because
 * `GET /api/v1/config` redacts the command (never returns its value), the form
 * starts blank and the operator must re-enter it. An empty string here means
 * "no health command" and clears it on apply.
 */
export type ConfigFormValue = {
  nodeId: string;
  priority: number;
  vip: {
    address: string;
    interface: string;
  };
  peers: PeerFormValue[];
  heartbeat: {
    bind: string;
    intervalMs: number;
    timeoutMs: number;
  };
  health: {
    /** Re-entered health command; empty string clears it on apply. */
    command: string;
    intervalMs: number;
    timeoutMs: number;
  };
  election: {
    requireQuorum: boolean;
    preempt: boolean;
  };
  api: {
    enabled: boolean;
    bind: string;
    readOnly: boolean;
    /**
     * Re-entered token-file path. Like `health.command`, the daemon never
     * returns the path (only whether one is configured), so this starts blank
     * and an empty value clears the write-API auth gate on apply.
     */
    authTokenFile: string;
    /** Whether the node reported a token file configured (for the editor warning). */
    authTokenConfigured: boolean;
  };
  mutationSafety: {
    allowNetworkMutation: boolean;
  };
};

/**
 * Build the editor's initial form state from the daemon's effective config.
 *
 * REDACTION: `GET /api/v1/config` never returns `health.command` (only
 * `command_redacted`). We therefore cannot prefill it — `health.command` always
 * starts blank and the operator must re-enter it to keep/set it. Leaving it
 * blank clears the health command on apply.
 */
export function formFromConfig(config: import("../api").ConfigResponse): ConfigFormValue {
  return {
    nodeId: config.node_id,
    priority: config.priority,
    vip: {
      address: config.vip.address,
      interface: config.vip.interface
    },
    peers: config.peers.map((peer) => ({ id: peer.id, address: peer.address })),
    heartbeat: {
      bind: config.heartbeat.bind,
      intervalMs: config.heartbeat.interval_ms,
      timeoutMs: config.heartbeat.timeout_ms
    },
    health: {
      // Always blank: the command is redacted and never returned by the daemon.
      command: "",
      intervalMs: config.health.interval_ms,
      timeoutMs: config.health.timeout_ms
    },
    election: {
      requireQuorum: config.election.require_quorum,
      preempt: config.election.preempt
    },
    api: {
      enabled: config.api.enabled,
      bind: config.api.bind,
      readOnly: config.api.read_only,
      // Always blank: the daemon reports only whether a token file is configured,
      // never the path. The operator must re-enter it to keep write-API auth.
      authTokenFile: "",
      authTokenConfigured: config.api.auth_token_configured ?? false
    },
    mutationSafety: {
      allowNetworkMutation: config.mutation_safety.allow_network_mutation
    }
  };
}

/**
 * Render a complete `config.toml` string from the editor form state.
 * Deterministic and pure. Emits every section so apply replaces the whole
 * config (matching #97 apply semantics — apply is a full replacement).
 *
 * - `node_id` is only emitted when non-empty so the daemon's hostname default
 *   still applies if the operator clears it.
 * - `[health].command` is only emitted when the operator entered a non-empty
 *   value; leaving it blank omits the key, which clears the health command.
 */
export function serializeConfigToml(form: ConfigFormValue): string {
  const lines: string[] = [];

  lines.push("# Generated by the easy-failover dashboard config editor.");
  lines.push("");

  // Top-level keys.
  const nodeId = form.nodeId.trim();
  if (nodeId) {
    lines.push(`node_id = ${tomlString(nodeId)}`);
  }
  lines.push(`priority = ${form.priority}`);
  lines.push("");

  // VIP.
  lines.push("[vip]");
  lines.push(`address = ${tomlString(form.vip.address)}`);
  lines.push(`interface = ${tomlString(form.vip.interface)}`);
  lines.push("");

  // Peers (at least one expected; emitted in order).
  for (const peer of form.peers) {
    lines.push("[[peers]]");
    lines.push(`id = ${tomlString(peer.id)}`);
    lines.push(`address = ${tomlString(peer.address)}`);
    lines.push("");
  }

  // Heartbeat.
  lines.push("[heartbeat]");
  lines.push(`bind = ${tomlString(form.heartbeat.bind)}`);
  lines.push(`interval_ms = ${form.heartbeat.intervalMs}`);
  lines.push(`timeout_ms = ${form.heartbeat.timeoutMs}`);
  lines.push("");

  // Health. The command is only emitted when the operator re-entered one;
  // a blank command clears the health command on apply.
  lines.push("[health]");
  const command = form.health.command.trim();
  if (command) {
    lines.push(`command = ${tomlString(command)}`);
  }
  lines.push(`interval_ms = ${form.health.intervalMs}`);
  lines.push(`timeout_ms = ${form.health.timeoutMs}`);
  lines.push("");

  // Election.
  lines.push("[election]");
  lines.push(`require_quorum = ${tomlBool(form.election.requireQuorum)}`);
  lines.push(`preempt = ${tomlBool(form.election.preempt)}`);
  lines.push("");

  // API.
  lines.push("[api]");
  lines.push(`enabled = ${tomlBool(form.api.enabled)}`);
  lines.push(`bind = ${tomlString(form.api.bind)}`);
  lines.push(`read_only = ${tomlBool(form.api.readOnly)}`);
  // Emit the token-file path only when set, so a fresh node without one stays
  // unset rather than being pinned to an empty path.
  const authTokenFile = form.api.authTokenFile.trim();
  if (authTokenFile) {
    lines.push(`auth_token_file = ${tomlString(authTokenFile)}`);
  }
  lines.push("");

  // Mutation safety — operator-controllable in this editor.
  lines.push("[mutation_safety]");
  lines.push(`allow_network_mutation = ${tomlBool(form.mutationSafety.allowNetworkMutation)}`);
  lines.push("");

  return lines.join("\n");
}
