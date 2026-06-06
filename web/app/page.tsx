"use client";

import {
  Activity,
  CheckCircle2,
  Clock3,
  Cpu,
  FileCog,
  HeartPulse,
  Network,
  Radio,
  ServerOff,
  ShieldCheck,
  Signal,
  TriangleAlert
} from "lucide-react";
import { useCallback, useEffect, useMemo, useState } from "react";
import {
  DashboardData,
  EventItem,
  NodeConfigApplyResult,
  NodeSummary,
  NodeView,
  PeerStatus,
  applyNodeConfig,
  dashboardDataFromView,
  fetchNodeConfig,
  fetchNodeSummaries,
  fetchNodeView,
  sampleDashboardData
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
            </div>
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
            </div>
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

type Source = "sample" | "api";

export default function DashboardPage() {
  const [nodes, setNodes] = useState<NodeSummary[]>([]);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [data, setData] = useState<DashboardData>(sampleDashboardData);
  const [source, setSource] = useState<Source>("sample");

  // Load the fleet list once. On failure (dashboard API down) or an empty/all-
  // unreachable roster, fall back to sample data so dev still renders.
  useEffect(() => {
    let cancelled = false;

    fetchNodeSummaries()
      .then((loaded) => {
        if (cancelled) {
          return;
        }
        if (loaded.length === 0) {
          setSource("sample");
          setData(sampleDashboardData);
          return;
        }
        setNodes(loaded);
        const firstReachable = loaded.find((n) => n.reachable) ?? loaded[0];
        setSelectedId(firstReachable.id);
        if (!loaded.some((n) => n.reachable)) {
          // Roster present but nothing reachable: show sample data so the
          // panels still render, while the strip flags every node unreachable.
          setSource("sample");
          setData(sampleDashboardData);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setNodes([]);
          setSelectedId(null);
          setSource("sample");
          setData(sampleDashboardData);
        }
      });

    return () => {
      cancelled = true;
    };
  }, []);

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
        <div className="sourceBadge">
          <Signal aria-hidden="true" />
          <span>{source === "api" ? "Live node data" : "Sample data"}</span>
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
        <span>Config edits apply on daemon restart. Write tokens stay server-side and never reach the browser.</span>
      </footer>
    </main>
  );
}
