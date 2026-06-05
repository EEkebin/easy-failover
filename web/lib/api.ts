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

function apiBase() {
  return process.env.NEXT_PUBLIC_EASY_FAILOVER_API_BASE ?? defaultApiBase;
}

async function getJson<T>(base: string, path: string): Promise<T> {
  const response = await fetch(`${base}${path}`, {
    headers: {
      Accept: "application/json"
    }
  });
  if (!response.ok) {
    throw new Error(`request failed for ${path}: ${response.status}`);
  }
  return (await response.json()) as T;
}

function peersFromConfig(config: ConfigResponse, observed: number): PeerStatus[] {
  return config.peers.map((peer, index) => ({
    id: peer.id,
    address: peer.address,
    state: index < observed ? "observed" : "unknown",
    healthy: index < observed,
    last_seen: index < observed ? "recent" : "not observed"
  }));
}

export async function fetchDashboardData(): Promise<DashboardData> {
  const base = apiBase().replace(/\/$/, "");
  const [status, config, events] = await Promise.all([
    getJson<StatusResponse>(base, "/api/v1/status"),
    getJson<ConfigResponse>(base, "/api/v1/config"),
    getJson<EventsResponse>(base, "/api/v1/events")
  ]);

  return {
    sample: false,
    api_base: base,
    status,
    config,
    peers: peersFromConfig(config, status.heartbeat.peers_observed),
    events: events.events
  };
}
