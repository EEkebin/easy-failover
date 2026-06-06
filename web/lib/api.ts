export type StatusResponse = {
  node: {
    id: string;
    priority: number;
    state: string;
    healthy: boolean;
  };
  vip: {
    address: string;
    interface: string;
    local_owner: boolean;
  };
  lifecycle: {
    initial_state: string;
    final_state: string;
    detail: string;
    dry_run: boolean;
    started: boolean;
    iteration_ran: boolean;
    stopped: boolean;
  };
  heartbeat: {
    bind: string;
    interval_ms: number;
    timeout_ms: number;
    peers_observed: number;
  };
  health: {
    status: string;
    detail: string;
  };
};

export type ConfigResponse = {
  node_id: string;
  priority: number;
  vip: {
    address: string;
    interface: string;
  };
  heartbeat: {
    bind: string;
    interval_ms: number;
    timeout_ms: number;
  };
  health: {
    command_redacted: boolean;
    interval_ms: number;
    timeout_ms: number;
  };
  election: {
    require_quorum: boolean;
    preempt: boolean;
  };
  api: {
    enabled: boolean;
    bind: string;
    read_only: boolean;
  };
  mutation_safety: {
    allow_network_mutation: boolean;
  };
  peers: Array<{
    id: string;
    address: string;
  }>;
};

export type EventItem = {
  sequence: number;
  event: string;
  level: string;
  message: string;
  fields: Record<string, string | number | boolean>;
};

export type EventsResponse = {
  events: EventItem[];
};

export type PeerStatus = {
  id: string;
  address: string;
  state: string;
  healthy: boolean;
  last_seen: string;
};

export type DashboardData = {
  sample: boolean;
  api_base: string;
  status: StatusResponse;
  config: ConfigResponse;
  peers: PeerStatus[];
  events: EventItem[];
};

const defaultApiBase = "http://127.0.0.1:8743";

export const sampleDashboardData: DashboardData = {
  sample: true,
  api_base: defaultApiBase,
  status: {
    node: {
      id: "node-a",
      priority: 100,
      state: "master",
      healthy: true
    },
    vip: {
      address: "10.0.0.50/24",
      interface: "eth0",
      local_owner: true
    },
    lifecycle: {
      initial_state: "stopped",
      final_state: "stopped",
      detail: "max iterations completed",
      dry_run: true,
      started: true,
      iteration_ran: true,
      stopped: true
    },
    heartbeat: {
      bind: "0.0.0.0:7432",
      interval_ms: 1000,
      timeout_ms: 3000,
      peers_observed: 1
    },
    health: {
      status: "healthy",
      detail: "health command detail redacted"
    }
  },
  config: {
    node_id: "node-a",
    priority: 100,
    vip: {
      address: "10.0.0.50/24",
      interface: "eth0"
    },
    heartbeat: {
      bind: "0.0.0.0:7432",
      interval_ms: 1000,
      timeout_ms: 3000
    },
    health: {
      command_redacted: true,
      interval_ms: 1000,
      timeout_ms: 2000
    },
    election: {
      require_quorum: false,
      preempt: true
    },
    api: {
      enabled: false,
      bind: "127.0.0.1:8743",
      read_only: true
    },
    mutation_safety: {
      allow_network_mutation: false
    },
    peers: [
      {
        id: "node-b",
        address: "10.0.0.12:7432"
      },
      {
        id: "node-c",
        address: "10.0.0.13:7432"
      }
    ]
  },
  peers: [
    {
      id: "node-b",
      address: "10.0.0.12:7432",
      state: "backup",
      healthy: true,
      last_seen: "2s ago"
    },
    {
      id: "node-c",
      address: "10.0.0.13:7432",
      state: "unknown",
      healthy: false,
      last_seen: "expired"
    }
  ],
  events: [
    {
      sequence: 41,
      event: "daemon_loop_result",
      level: "info",
      message:
        "event=daemon_loop_result node_id=\"node-a\" final_state=stopped stop_reason=max_iterations iterations_ran=1",
      fields: {
        node_id: "node-a",
        final_state: "stopped",
        stop_reason: "max_iterations",
        iterations_ran: 1
      }
    },
    {
      sequence: 42,
      event: "vip_operation",
      level: "info",
      message:
        "event=vip_operation operation=announce address=\"10.0.0.50/24\" interface=\"eth0\" result_dry_run=true",
      fields: {
        operation: "announce",
        address: "10.0.0.50/24",
        interface: "eth0",
        result_dry_run: true
      }
    }
  ]
};

// ---------------------------------------------------------------------------
// Multi-node client API.
//
// The browser no longer talks to node daemons directly. All fetches go through
// the dashboard's OWN server-side proxy routes (/api/nodes and /api/nodes/[id]),
// which probe each node, flag reachability, and keep write tokens server-side.
// ---------------------------------------------------------------------------

import type { NodeSummary, NodeView } from "./nodes/types";

export type { NodeSummary, NodeView } from "./nodes/types";

async function getJson<T>(path: string): Promise<T> {
  const response = await fetch(path, {
    headers: { Accept: "application/json" },
    cache: "no-store"
  });
  if (!response.ok) {
    throw new Error(`request failed for ${path}: ${response.status}`);
  }
  return (await response.json()) as T;
}

/**
 * Fetch the fleet list from the dashboard's own proxy. Each entry carries a
 * reachable flag; unreachable nodes are included (not dropped) so the UI can
 * show them. Throws only if the dashboard's own API is unavailable.
 */
export async function fetchNodeSummaries(): Promise<NodeSummary[]> {
  const data = await getJson<{ nodes: NodeSummary[] }>("/api/nodes");
  return data.nodes;
}

/**
 * Fetch one node's detail view (status/config/events + reachability) from the
 * dashboard's own proxy. The returned view may have `reachable: false` for an
 * unreachable node; that is a successful response, not an error.
 */
export async function fetchNodeView(id: string): Promise<NodeView> {
  return getJson<NodeView>(`/api/nodes/${encodeURIComponent(id)}`);
}

/**
 * Derive the peer list rendered by the dashboard from a node's config + the
 * number of peers its heartbeat currently observes. Mirrors the previous
 * single-node behavior so the existing PeerRow markup is reused unchanged.
 */
export function peersFromConfig(config: ConfigResponse, observed: number): PeerStatus[] {
  return config.peers.map((peer, index) => ({
    id: peer.id,
    address: peer.address,
    state: index < observed ? "observed" : "unknown",
    healthy: index < observed,
    last_seen: index < observed ? "recent" : "not observed"
  }));
}

/**
 * Adapt a reachable NodeView into the DashboardData shape the existing panels
 * consume. Returns null when the view lacks the status/config needed to render
 * (e.g. an unreachable node), letting the caller show an "unreachable" state.
 */
export function dashboardDataFromView(view: NodeView): DashboardData | null {
  if (!view.reachable || !view.status || !view.config) {
    return null;
  }
  return {
    sample: false,
    api_base: "/api/nodes",
    status: view.status,
    config: view.config,
    peers: peersFromConfig(view.config, view.status.heartbeat.peers_observed),
    events: view.events ?? []
  };
}
