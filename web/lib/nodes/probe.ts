// Server-only probing of individual easy-failover nodes.
//
// SERVER-ONLY: these functions perform the server-side fetches that the browser
// is no longer allowed to make directly. They translate a roster NodeEntry into
// the browser-safe NodeSummary / NodeView shapes, flagging reachability and
// tolerating per-endpoint failures. They redact nothing beyond what the daemon
// already redacts, and never surface `tokenEnv` or token contents.

import "server-only";

import type {
  ConfigResponse,
  EventsResponse,
  StatusResponse
} from "../api";
import type { NodeEntry, NodeSummary, NodeView } from "./types";

/** Short per-request timeout so one dead node can't stall the fleet list. */
const PROBE_TIMEOUT_MS = 3000;

function trimBase(apiBase: string): string {
  return apiBase.replace(/\/$/, "");
}

/**
 * Fetch one JSON endpoint on a node with a hard timeout. Throws on timeout,
 * network failure, non-2xx, or malformed JSON — callers decide how to degrade.
 */
async function fetchNodeJson<T>(apiBase: string, path: string): Promise<T> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), PROBE_TIMEOUT_MS);
  try {
    const response = await fetch(`${trimBase(apiBase)}${path}`, {
      headers: { Accept: "application/json" },
      signal: controller.signal,
      cache: "no-store"
    });
    if (!response.ok) {
      throw new Error(`status ${response.status}`);
    }
    return (await response.json()) as T;
  } finally {
    clearTimeout(timer);
  }
}

/** Non-sensitive, short reason string from an unknown thrown value. */
function reason(err: unknown): string {
  if (err instanceof Error) {
    if (err.name === "AbortError") {
      return "timed out";
    }
    return err.message;
  }
  return "unreachable";
}

/**
 * Probe a node's status and build a compact NodeSummary. Never throws: an
 * unreachable or erroring node is returned with `reachable: false`.
 */
export async function probeSummary(entry: NodeEntry): Promise<NodeSummary> {
  const base: NodeSummary = {
    id: entry.id,
    label: entry.label,
    apiBase: trimBase(entry.apiBase),
    reachable: false
  };
  try {
    const status = await fetchNodeJson<StatusResponse>(entry.apiBase, "/api/v1/status");
    return {
      ...base,
      reachable: true,
      status: {
        role: status.node.state,
        healthy: status.node.healthy,
        vipOwner: status.vip.local_owner,
        vipAddress: status.vip.address
      }
    };
  } catch (err) {
    return { ...base, error: reason(err) };
  }
}

/**
 * Probe a node's full status/config/events and build a NodeView. Status drives
 * reachability; config and events are best-effort (tolerated individually).
 * Never throws.
 */
export async function probeView(entry: NodeEntry): Promise<NodeView> {
  const view: NodeView = {
    id: entry.id,
    label: entry.label,
    reachable: false
  };

  let status: StatusResponse;
  try {
    status = await fetchNodeJson<StatusResponse>(entry.apiBase, "/api/v1/status");
  } catch (err) {
    return { ...view, error: reason(err) };
  }
  view.reachable = true;
  view.status = status;

  // Config and events are best-effort: a failure on either leaves it absent but
  // keeps the node reachable so the UI can still render the status panels.
  const [config, events] = await Promise.all([
    fetchNodeJson<ConfigResponse>(entry.apiBase, "/api/v1/config").catch(() => null),
    fetchNodeJson<EventsResponse>(entry.apiBase, "/api/v1/events").catch(() => null)
  ]);

  if (config) {
    view.config = config;
  }
  if (events) {
    view.events = events.events;
  }
  return view;
}
