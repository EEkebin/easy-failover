// Roster + fleet view types for the multi-node dashboard.
//
// These types are safe to import from both server and client code: they
// describe the shapes that cross the dashboard's own /api/nodes* boundary.
// The one server-only secret — `NodeEntry.tokenEnv` — names an environment
// variable; its *value* (the write token) is never placed on any of these
// shapes and never leaves the server. See registry.ts and the route handlers.

import type { ConfigResponse, EventItem, StatusResponse } from "../api";

/**
 * A single roster entry: one easy-failover node the dashboard knows about.
 *
 * The roster lives server-side (a JSON file or env var); no individual node
 * owns the pool membership list. See `listNodes` in registry.ts for resolution.
 */
export type NodeEntry = {
  /** Stable, roster-unique id used in URLs (/api/nodes/[id]) and as a key. */
  id: string;
  /** Base URL of the node's read API, e.g. "http://10.0.0.11:8743". */
  apiBase: string;
  /** Optional human-friendly display name. */
  label?: string;
  /**
   * Name of a SERVER-SIDE environment variable holding this node's write
   * token. Used later by the config editor (#98) to authorize writes from the
   * server. SECURITY: only the variable *name* lives here; the token value is
   * read on the server when needed and is NEVER serialized to the browser.
   */
  tokenEnv?: string;
};

/**
 * Compact, browser-safe status for one node, used to render the fleet list.
 * Populated by the server after probing the node's /api/v1/status.
 */
export type NodeStatusSummary = {
  /** Node role/state from status.node.state (e.g. "master", "backup"). */
  role: string;
  /** Whether the node reports itself healthy. */
  healthy: boolean;
  /** True when this node currently owns the VIP. */
  vipOwner: boolean;
  /** The configured/observed VIP address (status.vip.address). */
  vipAddress: string;
};

/**
 * Fleet-list entry: one node's identity + reachability + a compact status.
 * Returned by GET /api/nodes. `tokenEnv` is intentionally absent — it never
 * crosses to the browser.
 */
export type NodeSummary = {
  id: string;
  label?: string;
  apiBase: string;
  /** True when the server could reach and parse this node's status. */
  reachable: boolean;
  /** Compact status, present only when `reachable` is true. */
  status?: NodeStatusSummary;
  /** Short, non-sensitive reason when the node could not be reached/parsed. */
  error?: string;
};

/**
 * Detail view: one node's full status/config/events, reachability-guarded.
 * Returned by GET /api/nodes/[id]. Each sub-resource is present only when the
 * server successfully fetched it; an unreachable node yields `reachable: false`
 * with all three absent.
 */
export type NodeView = {
  id: string;
  label?: string;
  /** True when the node's status endpoint responded successfully. */
  reachable: boolean;
  /** Full runtime status, when available. */
  status?: StatusResponse;
  /** Effective (already daemon-redacted) config, when available. */
  config?: ConfigResponse;
  /** Recent runtime events, when available. */
  events?: EventItem[];
  /** Short, non-sensitive reason when the node could not be reached. */
  error?: string;
};
