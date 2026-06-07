"use client";

import {
  Activity,
  CheckCircle2,
  CircleDashed,
  Clock3,
  Cpu,
  FileCog,
  HeartPulse,
  Loader2,
  Network,
  Plus,
  Radio,
  ServerOff,
  ShieldCheck,
  Signal,
  TriangleAlert,
  X,
  XCircle
} from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  DashboardData,
  EventItem,
  NodeConfigApplyResult,
  NodeStateSummary,
  NodeSummary,
  NodeView,
  OnboardRequest,
  PeerStatus,
  StepProgress,
  applyNodeConfig,
  dashboardDataFromView,
  fetchNodeConfig,
  fetchNodeSummaries,
  fetchNodeView,
  sampleDashboardData,
  streamOnboard
} from "../lib/api";
import {
  ConfigFormValue,
  formFromConfig,
  serializeConfigToml
} from "../lib/config/serialize";

function stateClass(value: string) {
  if (value === "master" || value === "healthy" || value === "ready") {
    return "good";
  }
  if (value === "fault" || value === "unhealthy" || value === "rejected") {
    return "bad";
  }
  return "neutral";
}

function Pill({ value }: { value: string }) {
  return <span className={`pill ${stateClass(value)}`}>{value}</span>;
}

function Metric({
  label,
  value,
  icon: Icon,
  tone = "neutral"
}: {
  label: string;
  value: string;
  icon: typeof Activity;
  tone?: "good" | "neutral" | "bad";
}) {
  return (
    <section className="metric">
      <Icon aria-hidden="true" className={`metricIcon ${tone}`} />
      <div>
        <span>{label}</span>
        <strong>{value}</strong>
      </div>
    </section>
  );
}

function PeerRow({ peer }: { peer: PeerStatus }) {
  return (
    <li className="peerRow">
      <div className="peerMain">
        <span className="peerName">{peer.id}</span>
        <span className="peerAddress">{peer.address}</span>
      </div>
      <div className="peerMeta">
        <Pill value={peer.state} />
        <span>{peer.healthy ? "healthy" : "unhealthy"}</span>
        <span>{peer.last_seen}</span>
      </div>
    </li>
  );
}

function EventRow({ event }: { event: EventItem }) {
  return (
    <li className="eventRow">
      <div className="eventSequence">#{event.sequence}</div>
      <div className="eventBody">
        <div>
          <strong>{event.event}</strong>
          <span>{event.level}</span>
        </div>
        <p>{event.message}</p>
      </div>
    </li>
  );
}

/** One node in the fleet selector strip. */
function NodeCard({
  node,
  selected,
  onSelect
}: {
  node: NodeSummary;
  selected: boolean;
  onSelect: () => void;
}) {
  const label = node.label ?? node.id;
  const role = node.reachable && node.status ? node.status.role : "—";
  const vip = node.reachable && node.status?.vipOwner ? "VIP owner" : "—";
  return (
    <button
      type="button"
      className={`nodeCard${selected ? " selected" : ""}`}
      aria-pressed={selected}
      onClick={onSelect}
    >
      <div className="nodeCardMain">
        <span className="peerName">{label}</span>
        <span className="peerAddress">{node.apiBase}</span>
      </div>
      <div className="nodeCardMeta">
        <span className={`pill ${node.reachable ? "good" : "bad"}`}>
          {node.reachable ? "reachable" : "unreachable"}
        </span>
        <Pill value={role} />
        <span>{vip}</span>
      </div>
    </button>
  );
}

/** Panel shown in place of the dashboard when the selected node is unreachable. */
function UnreachablePanel({ node }: { node: NodeSummary | undefined }) {
  return (
    <section className="panel unreachablePanel" aria-label="Node unreachable">
      <div className="panelHeader">
        <div>
          <span className="eyebrow">Node</span>
          <h2>Node unreachable</h2>
        </div>
        <ServerOff aria-hidden="true" className="panelIcon bad" />
      </div>
      <p className="unreachableBody">
        The dashboard could not reach{" "}
        <strong>{node ? node.label ?? node.id : "this node"}</strong>
        {node ? ` (${node.apiBase})` : ""}.
        {node?.error ? ` Reason: ${node.error}.` : ""}
      </p>
      <p className="unreachableBody">
        Other nodes in the fleet are unaffected — pick a reachable node from the
        list above.
      </p>
    </section>
  );
}

/** Small labelled text/number input bound into the config form state. */
function TextField({
  label,
  value,
  onChange,
  type = "text",
  disabled = false,
  placeholder
}: {
  label: string;
  value: string | number;
  onChange: (next: string) => void;
  type?: "text" | "number";
  disabled?: boolean;
  placeholder?: string;
}) {
  return (
    <label className="field">
      <span>{label}</span>
      <input
        type={type}
        value={value}
        disabled={disabled}
        placeholder={placeholder}
        onChange={(e) => onChange(e.target.value)}
      />
    </label>
  );
}

/** Labelled checkbox bound into the config form state. */
function CheckField({
  label,
  checked,
  onChange,
  disabled = false
}: {
  label: string;
  checked: boolean;
  onChange: (next: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <label className="field checkbox">
      <input
        type="checkbox"
        checked={checked}
        disabled={disabled}
        onChange={(e) => onChange(e.target.checked)}
      />
      <span>{label}</span>
    </label>
  );
}

/**
 * The config editor for the selected node. Reads the node's effective (redacted)
 * config via the dashboard's own GET route, edits a form, serializes it to TOML
 * on the client, and POSTs it to the same route for validate + apply. The write
 * token stays server-side; this component only ever sees the result of a write.
 *
 * The component renders a disabled/empty state for unreachable nodes and never
 * throws, so it cannot crash the page.
 */
function ConfigEditorPanel({ nodeId }: { nodeId: string }) {
  const [form, setForm] = useState<ConfigFormValue | null>(null);
  const [writable, setWritable] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState<NodeConfigApplyResult | null>(null);

  // Load config once on mount. The panel is keyed by node id in the parent, so
  // selecting a different node remounts this component with fresh initial state
  // rather than re-running this effect — no synchronous reset needed here.
  useEffect(() => {
    let cancelled = false;
    fetchNodeConfig(nodeId)
      .then((res) => {
        if (cancelled) {
          return;
        }
        setWritable(res.writable);
        if (res.config) {
          setForm(formFromConfig(res.config));
        } else {
          setLoadError(res.error ?? "config unavailable");
        }
      })
      .catch((err: unknown) => {
        if (!cancelled) {
          setLoadError(err instanceof Error ? err.message : "config unavailable");
        }
      });
    return () => {
      cancelled = true;
    };
  }, [nodeId]);

  const update = useCallback((mutate: (draft: ConfigFormValue) => ConfigFormValue) => {
    setForm((prev) => (prev ? mutate(prev) : prev));
  }, []);

  const onApply = useCallback(() => {
    if (!form) {
      return;
    }
    setBusy(true);
    setResult(null);
    const toml = serializeConfigToml(form);
    applyNodeConfig(nodeId, toml)
      .then((res) => setResult(res))
      .catch((err: unknown) =>
        setResult({
          applied: false,
          error: err instanceof Error ? err.message : "apply failed"
        })
      )
      .finally(() => setBusy(false));
  }, [form, nodeId]);

  const validationErrors =
    result && !result.applied && Array.isArray(result.errors) ? result.errors : [];

  return (
    <section className="panel configPanel" aria-label="Config editor">
      <div className="panelHeader">
        <div>
          <span className="eyebrow">Config editor</span>
          <h2>Edit and apply node config</h2>
        </div>
        <FileCog aria-hidden="true" className="panelIcon" />
      </div>

      {!form ? (
        <p className="configHint">
          {loadError
            ? `Config is unavailable for this node: ${loadError}. Reads keep working; editing is disabled until the node returns a config.`
            : "Loading config…"}
        </p>
      ) : (
        <div className="configForm">
          {!writable && (
            <p className="configNotice">
              This node has no write token configured, so Apply is disabled. You
              can review and edit fields, but changes cannot be applied. Set a
              server-side token (the node&apos;s <code>tokenEnv</code>) to enable
              writes.
            </p>
          )}

          <div className="configSection">
            <h3>Node</h3>
            <div className="configGrid">
              <TextField
                label="node_id (blank uses hostname)"
                value={form.nodeId}
                onChange={(v) => update((d) => ({ ...d, nodeId: v }))}
              />
              <TextField
                label="priority"
                type="number"
                value={form.priority}
                onChange={(v) =>
                  update((d) => ({ ...d, priority: Number.parseInt(v, 10) || 0 }))
                }
              />
            </div>
          </div>

          <div className="configSection">
            <h3>VIP</h3>
            <div className="configGrid">
              <TextField
                label="address"
                value={form.vip.address}
                onChange={(v) => update((d) => ({ ...d, vip: { ...d.vip, address: v } }))}
              />
              <TextField
                label="interface"
                value={form.vip.interface}
                onChange={(v) =>
                  update((d) => ({ ...d, vip: { ...d.vip, interface: v } }))
                }
              />
            </div>
          </div>

          <div className="configSection">
            <h3>Peers</h3>
            {form.peers.map((peer, index) => (
              <div className="peerEditRow" key={index}>
                <TextField
                  label="id"
                  value={peer.id}
                  onChange={(v) =>
                    update((d) => {
                      const peers = d.peers.slice();
                      peers[index] = { ...peers[index], id: v };
                      return { ...d, peers };
                    })
                  }
                />
                <TextField
                  label="address"
                  value={peer.address}
                  onChange={(v) =>
                    update((d) => {
                      const peers = d.peers.slice();
                      peers[index] = { ...peers[index], address: v };
                      return { ...d, peers };
                    })
                  }
                />
                <button
                  type="button"
                  className="btn secondary"
                  onClick={() =>
                    update((d) => ({
                      ...d,
                      peers: d.peers.filter((_, i) => i !== index)
                    }))
                  }
                >
                  Remove
                </button>
              </div>
            ))}
            <div>
              <button
                type="button"
                className="btn secondary"
                onClick={() =>
                  update((d) => ({ ...d, peers: [...d.peers, { id: "", address: "" }] }))
                }
              >
                Add peer
              </button>
            </div>
          </div>

          <div className="configSection">
            <h3>Heartbeat</h3>
            <div className="configGrid">
              <TextField
                label="bind"
                value={form.heartbeat.bind}
                onChange={(v) =>
                  update((d) => ({ ...d, heartbeat: { ...d.heartbeat, bind: v } }))
                }
              />
              <TextField
                label="interval_ms"
                type="number"
                value={form.heartbeat.intervalMs}
                onChange={(v) =>
                  update((d) => ({
                    ...d,
                    heartbeat: { ...d.heartbeat, intervalMs: Number.parseInt(v, 10) || 0 }
                  }))
                }
              />
              <TextField
                label="timeout_ms"
                type="number"
                value={form.heartbeat.timeoutMs}
                onChange={(v) =>
                  update((d) => ({
                    ...d,
                    heartbeat: { ...d.heartbeat, timeoutMs: Number.parseInt(v, 10) || 0 }
                  }))
                }
              />
            </div>
          </div>

          <div className="configSection">
            <h3>Health</h3>
            <div className="configGrid">
              <TextField
                label="command (redacted — re-enter to keep)"
                value={form.health.command}
                placeholder="redacted — leave blank to clear on apply"
                onChange={(v) =>
                  update((d) => ({ ...d, health: { ...d.health, command: v } }))
                }
              />
              <TextField
                label="interval_ms"
                type="number"
                value={form.health.intervalMs}
                onChange={(v) =>
                  update((d) => ({
                    ...d,
                    health: { ...d.health, intervalMs: Number.parseInt(v, 10) || 0 }
                  }))
                }
              />
              <TextField
                label="timeout_ms"
                type="number"
                value={form.health.timeoutMs}
                onChange={(v) =>
                  update((d) => ({
                    ...d,
                    health: { ...d.health, timeoutMs: Number.parseInt(v, 10) || 0 }
                  }))
                }
              />
            </div>
            <p className="configHint warn">
              The current health command is redacted and never returned by the
              node. Apply replaces the whole config, so leaving this blank will{" "}
              <strong>clear the health command</strong>. Re-enter it to keep or
              change it.
            </p>
          </div>

          <div className="configSection">
            <h3>Election</h3>
            <div className="configGrid">
              <CheckField
                label="require_quorum"
                checked={form.election.requireQuorum}
                onChange={(v) =>
                  update((d) => ({
                    ...d,
                    election: { ...d.election, requireQuorum: v }
                  }))
                }
              />
              <CheckField
                label="preempt"
                checked={form.election.preempt}
                onChange={(v) =>
                  update((d) => ({ ...d, election: { ...d.election, preempt: v } }))
                }
              />
              <TextField
                label="quorum_size (0 = auto majority)"
                type="number"
                value={form.election.quorumSize}
                onChange={(v) =>
                  update((d) => ({
                    ...d,
                    election: { ...d.election, quorumSize: Number.parseInt(v, 10) || 0 }
                  }))
                }
              />
            </div>
            <p className="configHint">
              Quorum requires a majority of the cluster to be visible before this node owns the VIP;
              a node that loses quorum releases it. For a 2-node cluster, add a witness node or set
              quorum_size to keep failover.
            </p>
          </div>

          <div className="configSection">
            <h3>API</h3>
            <div className="configGrid">
              <CheckField
                label="enabled"
                checked={form.api.enabled}
                onChange={(v) => update((d) => ({ ...d, api: { ...d.api, enabled: v } }))}
              />
              <TextField
                label="bind"
                value={form.api.bind}
                onChange={(v) => update((d) => ({ ...d, api: { ...d.api, bind: v } }))}
              />
              <CheckField
                label="read_only"
                checked={form.api.readOnly}
                onChange={(v) => update((d) => ({ ...d, api: { ...d.api, readOnly: v } }))}
              />
              <TextField
                label="auth_token_file (re-enter to keep)"
                value={form.api.authTokenFile}
                placeholder={
                  form.api.authTokenConfigured
                    ? "configured — leave blank to clear on apply"
                    : "path to the write-API token file"
                }
                onChange={(v) => update((d) => ({ ...d, api: { ...d.api, authTokenFile: v } }))}
              />
            </div>
            {form.api.authTokenConfigured && (
              <p className="configHint warn">
                This node has a write-API token file configured, but its path is
                never returned by the node. Apply replaces the whole config, so
                leaving this blank will <strong>clear the token file</strong> and
                disable write-API authentication. Re-enter the path to keep it.
              </p>
            )}
          </div>

          <div className="configSection">
            <h3>Mutation safety</h3>
            <CheckField
              label="allow_network_mutation"
              checked={form.mutationSafety.allowNetworkMutation}
              onChange={(v) =>
                update((d) => ({
                  ...d,
                  mutationSafety: { allowNetworkMutation: v }
                }))
              }
            />
            <p className="configHint warn">
              Enabling real network mutation permits non-dry-run VIP changes. Keep
              this off until real VIP movement has been tested on the target.
            </p>
          </div>

          {validationErrors.length > 0 && (
            <ul className="configErrors" aria-label="Validation errors">
              {validationErrors.map((err, i) => (
                <li key={i}>{typeof err === "string" ? err : JSON.stringify(err)}</li>
              ))}
            </ul>
          )}

          {result && !result.applied && validationErrors.length === 0 && result.error && (
            <p className="configErrors">{result.error}</p>
          )}

          {result?.applied && (
            <div className="configSuccess">
              <strong>Config applied.</strong>
              {result.backup_path ? ` Backup saved at ${result.backup_path}.` : ""} This
              change takes effect on the next daemon restart (no live reload).
            </div>
          )}

          <div className="configActions">
            <button
              type="button"
              className="btn"
              onClick={onApply}
              disabled={!writable || busy}
            >
              {busy ? "Applying…" : "Apply"}
            </button>
            {!writable && (
              <span className="configHint">Apply is disabled: no write token.</span>
            )}
          </div>
        </div>
      )}
    </section>
  );
}

// ---------------------------------------------------------------------------
// Onboarding wizard (#101).
//
// A modal that collects a full OnboardRequest, POSTs it to /api/onboard, and
// renders the redacted NDJSON progress stream + final summary. Secrets entered
// here (SSH password / private key / passphrase / sudo password) are kept in
// local form state, sent ONCE on submit, and never displayed back. All strings
// shown from the stream are already redacted server-side.
// ---------------------------------------------------------------------------

type AuthKind = "password" | "privateKey";
type SudoKind = "passwordless" | "password" | "already-root";
type SourceKind = "release-tarball" | "build-from-source";

/** Mutable form state mirroring OnboardRequest; secrets are write-only fields. */
type WizardForm = {
  host: string;
  port: string;
  username: string;
  authKind: AuthKind;
  password: string;
  privateKey: string;
  passphrase: string;
  sudoKind: SudoKind;
  sudoPassword: string;
  sourceKind: SourceKind;
  releaseRef: string;
  sha256: string;
  gitRef: string;
  prefix: string;
  sysconfdir: string;
  expectedFingerprint: string;
  overwriteConfig: boolean;
  nodeId: string;
  priority: string;
  vipAddress: string;
  vipInterface: string;
  peers: PeerInput[];
  heartbeatBind: string;
  heartbeatIntervalMs: string;
  heartbeatTimeoutMs: string;
  healthCommand: string;
  healthIntervalMs: string;
  healthTimeoutMs: string;
  electionRequireQuorum: boolean;
  electionPreempt: boolean;
  apiEnabled: boolean;
  apiBind: string;
  apiReadOnly: boolean;
};

type PeerInput = { id: string; address: string };

function emptyWizardForm(): WizardForm {
  return {
    host: "",
    port: "22",
    username: "",
    authKind: "password",
    password: "",
    privateKey: "",
    passphrase: "",
    sudoKind: "passwordless",
    sudoPassword: "",
    sourceKind: "release-tarball",
    releaseRef: "latest",
    sha256: "",
    gitRef: "",
    prefix: "",
    sysconfdir: "",
    expectedFingerprint: "",
    overwriteConfig: false,
    nodeId: "",
    priority: "100",
    vipAddress: "",
    vipInterface: "",
    peers: [{ id: "", address: "" }],
    heartbeatBind: "",
    heartbeatIntervalMs: "",
    heartbeatTimeoutMs: "",
    healthCommand: "",
    healthIntervalMs: "",
    healthTimeoutMs: "",
    electionRequireQuorum: false,
    electionPreempt: true,
    apiEnabled: true,
    apiBind: "",
    apiReadOnly: true
  };
}

/** Parse a positive integer field, returning undefined for blank/invalid. */
function optInt(value: string): number | undefined {
  const trimmed = value.trim();
  if (trimmed.length === 0) {
    return undefined;
  }
  const parsed = Number.parseInt(trimmed, 10);
  return Number.isInteger(parsed) ? parsed : undefined;
}

function optStr(value: string): string | undefined {
  const trimmed = value.trim();
  return trimmed.length > 0 ? trimmed : undefined;
}

/**
 * Client-side validation of the minimum required fields before we even POST.
 * Returns a human-readable error, or null when the form is submittable.
 */
function validateWizard(form: WizardForm): string | null {
  if (form.host.trim().length === 0) {
    return "Host is required.";
  }
  if (form.username.trim().length === 0) {
    return "Username is required.";
  }
  if (form.authKind === "password" && form.password.length === 0) {
    return "SSH password is required for password auth.";
  }
  if (form.authKind === "privateKey" && form.privateKey.trim().length === 0) {
    return "Private key is required for key auth.";
  }
  if (form.sudoKind === "password" && form.sudoPassword.length === 0) {
    return "Sudo password is required for the password sudo method.";
  }
  if (form.sourceKind === "release-tarball" && form.releaseRef.trim().length === 0) {
    return "Release ref is required (use 'latest' for the newest release).";
  }
  if (form.vipAddress.trim().length === 0 || form.vipInterface.trim().length === 0) {
    return "VIP address and interface are required.";
  }
  const peers = form.peers.filter((p) => p.id.trim().length > 0 && p.address.trim().length > 0);
  if (peers.length === 0) {
    return "At least one peer (id + address) is required.";
  }
  return null;
}

/** Assemble the OnboardRequest from form state. Only set optional sections that
 * carry a value, so we don't send empty overrides. */
function buildRequest(form: WizardForm): OnboardRequest {
  const auth: OnboardRequest["connection"]["auth"] =
    form.authKind === "password"
      ? { kind: "password", password: form.password }
      : {
          kind: "privateKey",
          privateKey: form.privateKey,
          ...(form.passphrase.length > 0 ? { passphrase: form.passphrase } : {})
        };

  const sudo: OnboardRequest["connection"]["sudo"] =
    form.sudoKind === "passwordless"
      ? { kind: "passwordless" }
      : form.sudoKind === "already-root"
        ? { kind: "already-root" }
        : { kind: "password", sudoPassword: form.sudoPassword };

  const source: OnboardRequest["source"] =
    form.sourceKind === "release-tarball"
      ? {
          kind: "release-tarball",
          releaseRef: form.releaseRef.trim(),
          ...(optStr(form.sha256) ? { sha256: form.sha256.trim() } : {})
        }
      : { kind: "build-from-source", ...(optStr(form.gitRef) ? { gitRef: form.gitRef.trim() } : {}) };

  const peers = form.peers
    .filter((p) => p.id.trim().length > 0 && p.address.trim().length > 0)
    .map((p) => ({ id: p.id.trim(), address: p.address.trim() }));

  const config: OnboardRequest["config"] = {
    ...(optStr(form.nodeId) ? { nodeId: form.nodeId.trim() } : {}),
    ...(optInt(form.priority) !== undefined ? { priority: optInt(form.priority) } : {}),
    vip: { address: form.vipAddress.trim(), interface: form.vipInterface.trim() },
    peers
  };

  const heartbeat = {
    ...(optStr(form.heartbeatBind) ? { bind: form.heartbeatBind.trim() } : {}),
    ...(optInt(form.heartbeatIntervalMs) !== undefined
      ? { intervalMs: optInt(form.heartbeatIntervalMs) }
      : {}),
    ...(optInt(form.heartbeatTimeoutMs) !== undefined
      ? { timeoutMs: optInt(form.heartbeatTimeoutMs) }
      : {})
  };
  if (Object.keys(heartbeat).length > 0) {
    config.heartbeat = heartbeat;
  }

  const health = {
    ...(optStr(form.healthCommand) ? { command: form.healthCommand.trim() } : {}),
    ...(optInt(form.healthIntervalMs) !== undefined ? { intervalMs: optInt(form.healthIntervalMs) } : {}),
    ...(optInt(form.healthTimeoutMs) !== undefined ? { timeoutMs: optInt(form.healthTimeoutMs) } : {})
  };
  if (Object.keys(health).length > 0) {
    config.health = health;
  }

  config.election = {
    requireQuorum: form.electionRequireQuorum,
    preempt: form.electionPreempt
  };

  if (form.apiEnabled || optStr(form.apiBind) || form.apiReadOnly) {
    config.api = {
      enabled: form.apiEnabled,
      readOnly: form.apiReadOnly,
      ...(optStr(form.apiBind) ? { bind: form.apiBind.trim() } : {})
    };
  }

  const install = {
    ...(optStr(form.prefix) ? { prefix: form.prefix.trim() } : {}),
    ...(optStr(form.sysconfdir) ? { sysconfdir: form.sysconfdir.trim() } : {})
  };

  const request: OnboardRequest = {
    connection: {
      host: form.host.trim(),
      ...(optInt(form.port) !== undefined ? { port: optInt(form.port) } : {}),
      username: form.username.trim(),
      auth,
      sudo,
      ...(optStr(form.expectedFingerprint)
        ? { hostKey: { expectedFingerprint: form.expectedFingerprint.trim() } }
        : {})
    },
    source,
    config,
    ...(Object.keys(install).length > 0 ? { install } : {}),
    ...(form.overwriteConfig ? { overwriteConfig: true } : {})
  };

  return request;
}

/** Render a single step in the live progress list. */
function StepRow({ step }: { step: StepProgress }) {
  const Icon =
    step.status === "ok"
      ? CheckCircle2
      : step.status === "failed"
        ? XCircle
        : step.status === "running"
          ? Loader2
          : CircleDashed;
  const tone =
    step.status === "ok"
      ? "good"
      : step.status === "failed"
        ? "bad"
        : step.status === "running"
          ? "neutral"
          : "neutral";
  return (
    <li className="onboardStep">
      <Icon
        aria-hidden="true"
        className={`onboardStepIcon ${tone}${step.status === "running" ? " spin" : ""}`}
      />
      <div className="onboardStepBody">
        <div className="onboardStepHead">
          <strong>{step.label}</strong>
          <span className={`pill ${tone}`}>{step.status}</span>
        </div>
        {step.detail ? <p className="onboardStepDetail">{step.detail}</p> : null}
        {step.command ? <code className="onboardStepCmd">{step.command}</code> : null}
        {typeof step.exitCode === "number" ? (
          <span className="onboardStepMeta">exit code: {step.exitCode}</span>
        ) : null}
        {step.stderr ? <pre className="onboardStepStderr">{step.stderr}</pre> : null}
      </div>
    </li>
  );
}

function OnboardWizard({
  onClose,
  onSucceeded
}: {
  onClose: () => void;
  onSucceeded: () => void;
}) {
  const [form, setForm] = useState<WizardForm>(emptyWizardForm);
  const [submitting, setSubmitting] = useState(false);
  const [validationError, setValidationError] = useState<string | null>(null);
  const [steps, setSteps] = useState<StepProgress[]>([]);
  const [summary, setSummary] = useState<NodeStateSummary | null>(null);
  const [streamError, setStreamError] = useState<string | null>(null);
  const abortRef = useRef<AbortController | null>(null);

  // Abort an in-flight onboarding stream if the modal unmounts.
  useEffect(() => {
    return () => abortRef.current?.abort();
  }, []);

  const set = useCallback(<K extends keyof WizardForm>(key: K, value: WizardForm[K]) => {
    setForm((prev) => ({ ...prev, [key]: value }));
  }, []);

  const onSubmit = useCallback(async () => {
    const error = validateWizard(form);
    if (error) {
      setValidationError(error);
      return;
    }
    setValidationError(null);
    setSteps([]);
    setSummary(null);
    setStreamError(null);
    setSubmitting(true);

    const controller = new AbortController();
    abortRef.current = controller;
    let succeeded = false;
    try {
      const request = buildRequest(form);
      for await (const evt of streamOnboard(request, controller.signal)) {
        if (evt.type === "progress") {
          setSteps((prev) => {
            // Replace the matching step (same id) so re-emitted statuses update
            // in place; otherwise append.
            const index = prev.findIndex((s) => s.step === evt.event.step);
            if (index >= 0) {
              const next = prev.slice();
              next[index] = evt.event;
              return next;
            }
            return [...prev, evt.event];
          });
        } else if (evt.type === "result") {
          setSummary(evt.summary);
          succeeded = evt.summary.succeeded;
        } else {
          setStreamError(evt.error);
        }
      }
    } catch (err) {
      setStreamError(err instanceof Error ? err.message : "onboarding failed");
    } finally {
      setSubmitting(false);
      abortRef.current = null;
      if (succeeded) {
        // Refresh the fleet list so the new node appears.
        onSucceeded();
      }
    }
  }, [form, onSucceeded]);

  const updatePeer = useCallback((index: number, patch: Partial<PeerInput>) => {
    setForm((prev) => {
      const peers = prev.peers.slice();
      peers[index] = { ...peers[index], ...patch };
      return { ...prev, peers };
    });
  }, []);

  const failedStep = steps.find((s) => s.status === "failed");
  const done = summary !== null || streamError !== null;

  return (
    <div
      className="onboardOverlay"
      role="dialog"
      aria-modal="true"
      aria-label="Onboard node wizard"
    >
      <div className="onboardModal panel">
        <div className="panelHeader">
          <div>
            <span className="eyebrow">Onboarding</span>
            <h2>Onboard a node over SSH</h2>
          </div>
          <button
            type="button"
            className="onboardClose"
            aria-label="Close"
            onClick={onClose}
          >
            <X aria-hidden="true" />
          </button>
        </div>

        <p className="configHint">
          Onboarding connects to the target over SSH, installs easy-failover,
          writes its config, and starts it. It is disabled unless{" "}
          <code>EASY_FAILOVER_ONBOARD_ENABLED=true</code> is set on the dashboard
          server. Secrets you enter here are sent once and never shown back.
        </p>

        <div className="configForm">
          <div className="configSection">
            <h3>Connection</h3>
            <div className="configGrid">
              <TextField label="host" value={form.host} onChange={(v) => set("host", v)} />
              <TextField label="port" type="number" value={form.port} onChange={(v) => set("port", v)} />
              <TextField label="username" value={form.username} onChange={(v) => set("username", v)} />
            </div>
            <div className="configGrid">
              <label className="field">
                <span>auth method</span>
                <select
                  className="onboardSelect"
                  value={form.authKind}
                  onChange={(e) => set("authKind", e.target.value as AuthKind)}
                >
                  <option value="password">password</option>
                  <option value="privateKey">private key</option>
                </select>
              </label>
              {form.authKind === "password" ? (
                <SecretField
                  label="SSH password"
                  value={form.password}
                  onChange={(v) => set("password", v)}
                />
              ) : (
                <>
                  <label className="field">
                    <span>private key (PEM)</span>
                    <textarea
                      className="onboardTextarea"
                      value={form.privateKey}
                      onChange={(e) => set("privateKey", e.target.value)}
                      placeholder="-----BEGIN OPENSSH PRIVATE KEY-----"
                    />
                  </label>
                  <SecretField
                    label="key passphrase (optional)"
                    value={form.passphrase}
                    onChange={(v) => set("passphrase", v)}
                  />
                </>
              )}
            </div>
            <div className="configGrid">
              <label className="field">
                <span>sudo method</span>
                <select
                  className="onboardSelect"
                  value={form.sudoKind}
                  onChange={(e) => set("sudoKind", e.target.value as SudoKind)}
                >
                  <option value="passwordless">passwordless sudo</option>
                  <option value="password">sudo with password</option>
                  <option value="already-root">already root</option>
                </select>
              </label>
              {form.sudoKind === "password" ? (
                <SecretField
                  label="sudo password"
                  value={form.sudoPassword}
                  onChange={(v) => set("sudoPassword", v)}
                />
              ) : null}
            </div>
            <label className="field">
              <span>expected host-key fingerprint (optional, strict)</span>
              <input
                type="text"
                value={form.expectedFingerprint}
                onChange={(e) => set("expectedFingerprint", e.target.value)}
                placeholder="SHA256:… — blank pins the key on first connect (TOFU)"
              />
            </label>
          </div>

          <div className="configSection">
            <h3>Install source</h3>
            <div className="configGrid">
              <label className="field">
                <span>source</span>
                <select
                  className="onboardSelect"
                  value={form.sourceKind}
                  onChange={(e) => set("sourceKind", e.target.value as SourceKind)}
                >
                  <option value="release-tarball">release tarball</option>
                  <option value="build-from-source">build from source</option>
                </select>
              </label>
              {form.sourceKind === "release-tarball" ? (
                <>
                  <TextField
                    label="release ref"
                    value={form.releaseRef}
                    onChange={(v) => set("releaseRef", v)}
                    placeholder="latest or v1.2.3"
                  />
                  <TextField
                    label="sha256 (optional)"
                    value={form.sha256}
                    onChange={(v) => set("sha256", v)}
                  />
                </>
              ) : (
                <TextField
                  label="git ref (optional)"
                  value={form.gitRef}
                  onChange={(v) => set("gitRef", v)}
                  placeholder="branch, tag, or commit"
                />
              )}
            </div>
            <div className="configGrid">
              <TextField
                label="install prefix (optional)"
                value={form.prefix}
                onChange={(v) => set("prefix", v)}
                placeholder="/usr"
              />
              <TextField
                label="sysconfdir (optional)"
                value={form.sysconfdir}
                onChange={(v) => set("sysconfdir", v)}
                placeholder="/etc"
              />
            </div>
          </div>

          <div className="configSection">
            <h3>Config</h3>
            <div className="configGrid">
              <TextField
                label="node_id (blank uses hostname)"
                value={form.nodeId}
                onChange={(v) => set("nodeId", v)}
              />
              <TextField
                label="priority"
                type="number"
                value={form.priority}
                onChange={(v) => set("priority", v)}
              />
            </div>
            <div className="configGrid">
              <TextField
                label="vip address"
                value={form.vipAddress}
                onChange={(v) => set("vipAddress", v)}
                placeholder="10.0.0.50/24"
              />
              <TextField
                label="vip interface"
                value={form.vipInterface}
                onChange={(v) => set("vipInterface", v)}
                placeholder="eth0"
              />
            </div>
          </div>

          <div className="configSection">
            <h3>Peers</h3>
            {form.peers.map((peer, index) => (
              <div className="peerEditRow" key={index}>
                <TextField
                  label="id"
                  value={peer.id}
                  onChange={(v) => updatePeer(index, { id: v })}
                />
                <TextField
                  label="address"
                  value={peer.address}
                  onChange={(v) => updatePeer(index, { address: v })}
                />
                <button
                  type="button"
                  className="btn secondary"
                  onClick={() =>
                    setForm((prev) => ({
                      ...prev,
                      peers: prev.peers.filter((_, i) => i !== index)
                    }))
                  }
                >
                  Remove
                </button>
              </div>
            ))}
            <div>
              <button
                type="button"
                className="btn secondary"
                onClick={() =>
                  setForm((prev) => ({ ...prev, peers: [...prev.peers, { id: "", address: "" }] }))
                }
              >
                Add peer
              </button>
            </div>
          </div>

          <div className="configSection">
            <h3>Heartbeat (optional)</h3>
            <div className="configGrid">
              <TextField label="bind" value={form.heartbeatBind} onChange={(v) => set("heartbeatBind", v)} />
              <TextField
                label="interval_ms"
                type="number"
                value={form.heartbeatIntervalMs}
                onChange={(v) => set("heartbeatIntervalMs", v)}
              />
              <TextField
                label="timeout_ms"
                type="number"
                value={form.heartbeatTimeoutMs}
                onChange={(v) => set("heartbeatTimeoutMs", v)}
              />
            </div>
          </div>

          <div className="configSection">
            <h3>Health (optional)</h3>
            <div className="configGrid">
              <TextField
                label="command"
                value={form.healthCommand}
                onChange={(v) => set("healthCommand", v)}
              />
              <TextField
                label="interval_ms"
                type="number"
                value={form.healthIntervalMs}
                onChange={(v) => set("healthIntervalMs", v)}
              />
              <TextField
                label="timeout_ms"
                type="number"
                value={form.healthTimeoutMs}
                onChange={(v) => set("healthTimeoutMs", v)}
              />
            </div>
          </div>

          <div className="configSection">
            <h3>Election (optional)</h3>
            <div className="configGrid">
              <CheckField
                label="require_quorum"
                checked={form.electionRequireQuorum}
                onChange={(v) => set("electionRequireQuorum", v)}
              />
              <CheckField
                label="preempt"
                checked={form.electionPreempt}
                onChange={(v) => set("electionPreempt", v)}
              />
            </div>
          </div>

          <div className="configSection">
            <h3>API (optional)</h3>
            <div className="configGrid">
              <CheckField
                label="enabled"
                checked={form.apiEnabled}
                onChange={(v) => set("apiEnabled", v)}
              />
              <TextField
                label="bind"
                value={form.apiBind}
                onChange={(v) => set("apiBind", v)}
                placeholder="0.0.0.0:8743"
              />
              <CheckField
                label="read_only"
                checked={form.apiReadOnly}
                onChange={(v) => set("apiReadOnly", v)}
              />
            </div>
            <p className="configHint">
              The <code>[api].bind</code> must be a dashboard-reachable address
              for the node to show as reachable in the fleet view — a
              loopback-only bind (127.0.0.1) won&apos;t be reachable from another
              host.
            </p>
          </div>

          <div className="configSection">
            <h3>Mutation safety</h3>
            <p className="configHint warn">
              <code>allow_network_mutation</code> is always forced{" "}
              <strong>off</strong> by onboarding. Real VIP movement must be
              enabled later via the config editor after it has been tested.
            </p>
            <CheckField
              label="allow overwriting a divergent existing config on the target"
              checked={form.overwriteConfig}
              onChange={(v) => set("overwriteConfig", v)}
            />
          </div>

          {validationError ? <p className="configErrors">{validationError}</p> : null}

          {steps.length > 0 ? (
            <div className="configSection">
              <h3>Progress</h3>
              <ul className="onboardSteps">
                {steps.map((step) => (
                  <StepRow key={step.step} step={step} />
                ))}
              </ul>
            </div>
          ) : null}

          {streamError ? (
            <div className="configErrors">
              <strong>Onboarding failed.</strong> {streamError}
              {failedStep ? (
                <span>
                  {" "}
                  Failing step: <strong>{failedStep.label}</strong>. Fix the
                  cause and re-submit.
                </span>
              ) : null}
            </div>
          ) : null}

          {summary ? (
            summary.succeeded ? (
              <div className="configSuccess">
                <strong>Onboarding succeeded.</strong> Reached step{" "}
                <strong>{summary.reachedStep}</strong>. {summary.resultingState}
                {summary.configPath ? ` Config written at ${summary.configPath}.` : ""}
                {summary.hostKeyFingerprint
                  ? ` Pinned host key: ${summary.hostKeyFingerprint}.`
                  : ""}{" "}
                The node was added to the roster (read-only until a write token
                is wired server-side).
              </div>
            ) : (
              <div className="configErrors">
                <strong>Onboarding did not complete.</strong> Reached step{" "}
                <strong>{summary.reachedStep}</strong>. {summary.resultingState}
                {failedStep ? (
                  <span>
                    {" "}
                    Failing step: <strong>{failedStep.label}</strong>. Fix the
                    cause and re-submit.
                  </span>
                ) : null}
              </div>
            )
          ) : null}

          <div className="configActions">
            <button type="button" className="btn" onClick={onSubmit} disabled={submitting}>
              {submitting ? "Onboarding…" : done ? "Re-submit" : "Onboard"}
            </button>
            <button type="button" className="btn secondary" onClick={onClose}>
              {summary?.succeeded ? "Done" : "Close"}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}

/** A masked, write-only secret field. The value is never re-displayed as text. */
function SecretField({
  label,
  value,
  onChange
}: {
  label: string;
  value: string;
  onChange: (next: string) => void;
}) {
  return (
    <label className="field">
      <span>{label}</span>
      <input
        type="password"
        autoComplete="new-password"
        value={value}
        onChange={(e) => onChange(e.target.value)}
      />
    </label>
  );
}

type Source = "sample" | "api";

export default function DashboardPage() {
  const [nodes, setNodes] = useState<NodeSummary[]>([]);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [data, setData] = useState<DashboardData>(sampleDashboardData);
  const [source, setSource] = useState<Source>("sample");
  const [wizardOpen, setWizardOpen] = useState(false);

  // Load (or reload) the fleet list. On failure (dashboard API down) or an
  // empty/all-unreachable roster, fall back to sample data so dev still renders.
  // `preserveSelection` keeps the current selection on a refresh; the initial
  // load picks the first reachable node.
  const refreshNodes = useCallback((preserveSelection: boolean) => {
    fetchNodeSummaries()
      .then((loaded) => {
        if (loaded.length === 0) {
          setNodes([]);
          if (!preserveSelection) {
            setSelectedId(null);
          }
          setSource("sample");
          setData(sampleDashboardData);
          return;
        }
        setNodes(loaded);
        setSelectedId((prev) => {
          if (preserveSelection && prev && loaded.some((n) => n.id === prev)) {
            return prev;
          }
          return (loaded.find((n) => n.reachable) ?? loaded[0]).id;
        });
        if (!loaded.some((n) => n.reachable)) {
          // Roster present but nothing reachable: show sample data so the
          // panels still render, while the strip flags every node unreachable.
          setSource("sample");
          setData(sampleDashboardData);
        }
      })
      .catch(() => {
        setNodes([]);
        if (!preserveSelection) {
          setSelectedId(null);
        }
        setSource("sample");
        setData(sampleDashboardData);
      });
  }, []);

  // Load the fleet list once on mount.
  useEffect(() => {
    refreshNodes(false);
  }, [refreshNodes]);

  const selectedNode = useMemo(
    () => nodes.find((n) => n.id === selectedId),
    [nodes, selectedId]
  );

  // Load the selected node's detail view whenever the selection changes.
  const loadView = useCallback((id: string) => {
    let cancelled = false;
    fetchNodeView(id)
      .then((view: NodeView) => {
        if (cancelled) {
          return;
        }
        const adapted = dashboardDataFromView(view);
        if (adapted) {
          setData(adapted);
          setSource("api");
        } else {
          // Unreachable node: keep sample data behind the unreachable panel.
          setSource("sample");
          setData(sampleDashboardData);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setSource("sample");
          setData(sampleDashboardData);
        }
      });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    if (!selectedId || !selectedNode?.reachable) {
      return;
    }
    return loadView(selectedId);
  }, [selectedId, selectedNode?.reachable, loadView]);

  const showUnreachable = Boolean(selectedNode && !selectedNode.reachable);
  const healthTone = data.status.node.healthy ? "good" : "bad";
  const vipOwner = data.status.vip.local_owner ? "local owner" : "not local";
  const apiBase = useMemo(() => data.api_base.replace(/\/$/, ""), [data.api_base]);

  return (
    <main>
      <header className="topBar">
        <div>
          <span className="eyebrow">easy-failover</span>
          <h1>Fleet failover dashboard</h1>
        </div>
        <div className="topBarActions">
          <div className="sourceBadge">
            <Signal aria-hidden="true" />
            <span>{source === "api" ? "Live node data" : "Sample data"}</span>
          </div>
          <button
            type="button"
            className="btn onboardLaunch"
            onClick={() => setWizardOpen(true)}
          >
            <Plus aria-hidden="true" />
            <span>Onboard node</span>
          </button>
        </div>
      </header>

      <section className="nodeStrip" aria-label="Fleet nodes">
        {nodes.length === 0 ? (
          <div className="nodeStripEmpty">
            No nodes resolved from the roster — showing sample data.
          </div>
        ) : (
          nodes.map((node) => (
            <NodeCard
              key={node.id}
              node={node}
              selected={node.id === selectedId}
              onSelect={() => setSelectedId(node.id)}
            />
          ))
        )}
      </section>

      {showUnreachable ? (
        <section className="dashboardGrid" aria-label="Selected node">
          <UnreachablePanel node={selectedNode} />
        </section>
      ) : (
        <>
          <section className="summaryGrid" aria-label="Runtime summary">
            <Metric label="Node" value={data.status.node.id} icon={Cpu} tone="neutral" />
            <Metric label="Role" value={data.status.node.state} icon={ShieldCheck} tone={stateClass(data.status.node.state) as "good" | "neutral" | "bad"} />
            <Metric label="Health" value={data.status.health.status} icon={HeartPulse} tone={healthTone} />
            <Metric label="VIP" value={vipOwner} icon={Network} tone={data.status.vip.local_owner ? "good" : "neutral"} />
          </section>

          <section className="dashboardGrid">
            <section className="panel primaryPanel">
              <div className="panelHeader">
                <div>
                  <span className="eyebrow">Runtime</span>
                  <h2>Ownership state</h2>
                </div>
                <Pill value={data.status.lifecycle.final_state} />
              </div>
              <div className="ownershipLayout">
                <div>
                  <span className="label">Virtual IP</span>
                  <strong>{data.status.vip.address}</strong>
                  <span>{data.status.vip.interface}</span>
                </div>
                <div>
                  <span className="label">Decision detail</span>
                  <p>{data.status.lifecycle.detail}</p>
                </div>
              </div>
              <dl className="facts">
                <div>
                  <dt>Dry run</dt>
                  <dd>{data.status.lifecycle.dry_run ? "enabled" : "disabled"}</dd>
                </div>
                <div>
                  <dt>Iteration</dt>
                  <dd>{data.status.lifecycle.iteration_ran ? "ran" : "waiting"}</dd>
                </div>
                <div>
                  <dt>Priority</dt>
                  <dd>{data.status.node.priority}</dd>
                </div>
              </dl>
            </section>

            <section className="panel">
              <div className="panelHeader">
                <div>
                  <span className="eyebrow">Heartbeat</span>
                  <h2>Peer observation</h2>
                </div>
                <Radio aria-hidden="true" className="panelIcon" />
              </div>
              <dl className="facts stacked">
                <div>
                  <dt>Bind</dt>
                  <dd>{data.status.heartbeat.bind}</dd>
                </div>
                <div>
                  <dt>Interval</dt>
                  <dd>{data.status.heartbeat.interval_ms} ms</dd>
                </div>
                <div>
                  <dt>Timeout</dt>
                  <dd>{data.status.heartbeat.timeout_ms} ms</dd>
                </div>
                <div>
                  <dt>Observed peers</dt>
                  <dd>{data.status.heartbeat.peers_observed}</dd>
                </div>
              </dl>
            </section>

            <section className="panel widePanel">
              <div className="panelHeader">
                <div>
                  <span className="eyebrow">Peers</span>
                  <h2>Configured nodes</h2>
                </div>
                <Clock3 aria-hidden="true" className="panelIcon" />
              </div>
              <ul className="peerList">
                {data.peers.map((peer) => (
                  <PeerRow key={peer.id} peer={peer} />
                ))}
              </ul>
            </section>

            <section className="panel">
              <div className="panelHeader">
                <div>
                  <span className="eyebrow">Config</span>
                  <h2>Effective settings</h2>
                </div>
                <FileCog aria-hidden="true" className="panelIcon" />
              </div>
              <dl className="facts stacked">
                <div>
                  <dt>API</dt>
                  <dd>{data.config.api.enabled ? data.config.api.bind : "disabled"}</dd>
                </div>
                <div>
                  <dt>Read only</dt>
                  <dd>{data.config.api.read_only ? "true" : "false"}</dd>
                </div>
                <div>
                  <dt>Health command</dt>
                  <dd>{data.config.health.command_redacted ? "redacted" : "not configured"}</dd>
                </div>
                <div>
                  <dt>Mutation safety</dt>
                  <dd>{data.config.mutation_safety.allow_network_mutation ? "real VIP changes allowed" : "dry-run gated"}</dd>
                </div>
              </dl>
            </section>

            <section className="panel widePanel">
              <div className="panelHeader">
                <div>
                  <span className="eyebrow">Events</span>
                  <h2>Recent runtime activity</h2>
                </div>
                {source === "sample" ? (
                  <TriangleAlert aria-hidden="true" className="panelIcon warn" />
                ) : (
                  <CheckCircle2 aria-hidden="true" className="panelIcon good" />
                )}
              </div>
              <ul className="eventList">
                {data.events.map((event) => (
                  <EventRow key={event.sequence} event={event} />
                ))}
              </ul>
            </section>

            {selectedId && selectedNode?.reachable && source === "api" && (
              <ConfigEditorPanel key={selectedId} nodeId={selectedId} />
            )}
          </section>
        </>
      )}

      <footer>
        <span>API base: {apiBase}</span>
        <span>
          Config edits apply on daemon restart. Write tokens stay server-side
          and never reach the browser. Onboarding is disabled unless{" "}
          <code>EASY_FAILOVER_ONBOARD_ENABLED=true</code> on the dashboard server.
        </span>
      </footer>

      {wizardOpen ? (
        <OnboardWizard
          onClose={() => setWizardOpen(false)}
          onSucceeded={() => refreshNodes(true)}
        />
      ) : null}
    </main>
  );
}
