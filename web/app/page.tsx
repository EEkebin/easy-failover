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
  ShieldCheck,
  Signal,
  TriangleAlert
} from "lucide-react";
import { useEffect, useMemo, useState } from "react";
import {
  DashboardData,
  EventItem,
  PeerStatus,
  fetchDashboardData,
  sampleDashboardData
} from "../lib/api";

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

export default function DashboardPage() {
  const [data, setData] = useState<DashboardData>(sampleDashboardData);
  const [source, setSource] = useState<"sample" | "api">("sample");

  useEffect(() => {
    let cancelled = false;

    fetchDashboardData()
      .then((loaded) => {
        if (!cancelled) {
          setData(loaded);
          setSource(loaded.sample ? "sample" : "api");
        }
      })
      .catch(() => {
        if (!cancelled) {
          setData(sampleDashboardData);
          setSource("sample");
        }
      });

    return () => {
      cancelled = true;
    };
  }, []);

  const healthTone = data.status.node.healthy ? "good" : "bad";
  const vipOwner = data.status.vip.local_owner ? "local owner" : "not local";
  const apiBase = useMemo(() => data.api_base.replace(/\/$/, ""), [data.api_base]);

  return (
    <main>
      <header className="topBar">
        <div>
          <span className="eyebrow">easy-failover</span>
          <h1>Local failover dashboard</h1>
        </div>
        <div className="sourceBadge">
          <Signal aria-hidden="true" />
          <span>{source === "api" ? "Local API" : "Sample data"}</span>
        </div>
      </header>

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
      </section>

      <footer>
        <span>API base: {apiBase}</span>
        <span>Read-only dashboard. No daemon, VIP, or config mutation controls are exposed.</span>
      </footer>
    </main>
  );
}
