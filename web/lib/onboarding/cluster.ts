// Server-only: derive the peer list a freshly onboarded node should inherit.
//
// When onboarding a new node B from the dashboard running alongside node A, B
// should join the existing cluster automatically: its peers = A's current peers
// PLUS A itself. A's own reachable address is not in its config (heartbeat.bind
// is typically 0.0.0.0), so we discover the local IP that routes toward B with a
// UDP `connect()` — which assigns a source address WITHOUT sending any packet.

import "server-only";

import { createSocket } from "node:dgram";

import { localNode } from "../nodes/registry";
import type { ConfigResponse } from "../api";
import type { PeerInput } from "./types";

const DEFAULT_HEARTBEAT_PORT = 7432;
const PROBE_TIMEOUT_MS = 1500;

/** Parse the port out of a `host:port` bind string, falling back to the default. */
function portOf(bind: string | undefined): number {
  if (typeof bind !== "string") {
    return DEFAULT_HEARTBEAT_PORT;
  }
  const colon = bind.lastIndexOf(":");
  if (colon < 0) {
    return DEFAULT_HEARTBEAT_PORT;
  }
  const port = Number.parseInt(bind.slice(colon + 1).trim(), 10);
  return Number.isInteger(port) && port > 0 && port <= 65535 ? port : DEFAULT_HEARTBEAT_PORT;
}

/**
 * The local IP the OS would use to reach `host`. A UDP socket `connect()` only
 * sets the default destination and binds a local address per the routing table —
 * no datagram is sent — so this is a cheap, side-effect-free way to learn "our"
 * address on the path to the target. Resolves null on any error/timeout.
 */
function localAddressToward(host: string, port: number): Promise<string | null> {
  return new Promise((resolve) => {
    let settled = false;
    const socket = createSocket("udp4");
    const finish = (value: string | null) => {
      if (settled) {
        return;
      }
      settled = true;
      try {
        socket.close();
      } catch {
        /* already closed */
      }
      resolve(value);
    };
    const timer = setTimeout(() => finish(null), PROBE_TIMEOUT_MS);
    socket.on("error", () => {
      clearTimeout(timer);
      finish(null);
    });
    try {
      socket.connect(port, host, () => {
        clearTimeout(timer);
        try {
          finish(socket.address().address);
        } catch {
          finish(null);
        }
      });
    } catch {
      clearTimeout(timer);
      finish(null);
    }
  });
}

/**
 * Build the peers a node being onboarded at `targetHost` should inherit: the
 * current node's configured peers plus the current node itself (addressed at the
 * IP that routes to the target). Best-effort: returns `[]` if the current node's
 * config can't be read, so onboarding still proceeds with operator-entered peers.
 */
export async function inheritedClusterPeers(targetHost: string): Promise<PeerInput[]> {
  const current = localNode();
  if (!current) {
    return [];
  }

  let config: ConfigResponse;
  try {
    const base = current.apiBase.replace(/\/$/, "");
    const response = await fetch(`${base}/api/v1/config`, { cache: "no-store" });
    if (!response.ok) {
      return [];
    }
    config = (await response.json()) as ConfigResponse;
  } catch {
    return [];
  }

  const peers: PeerInput[] = Array.isArray(config.peers)
    ? config.peers
        .filter((p) => typeof p?.id === "string" && typeof p?.address === "string")
        .map((p) => ({ id: p.id, address: p.address }))
    : [];

  // Add the current node itself, addressed at the IP that routes toward the target.
  const port = portOf(config.heartbeat?.bind);
  const selfAddress = await localAddressToward(targetHost, port);
  if (selfAddress && typeof config.node_id === "string" && config.node_id.length > 0) {
    peers.push({ id: config.node_id, address: `${selfAddress}:${port}` });
  }

  return peers;
}

/**
 * Merge operator-entered peers with the inherited cluster peers, de-duplicated by
 * address, excluding the node being onboarded itself (by node id or host). The
 * operator's entries win on ordering; inherited peers fill in the rest.
 */
export function mergePeers(
  operatorPeers: PeerInput[],
  inherited: PeerInput[],
  selfNodeId: string | undefined,
  selfHost: string
): PeerInput[] {
  const out: PeerInput[] = [];
  const seenAddress = new Set<string>();
  for (const peer of [...operatorPeers, ...inherited]) {
    const id = peer.id?.trim();
    const address = peer.address?.trim();
    if (!id || !address) {
      continue;
    }
    // Never list the node being onboarded as its own peer.
    if (selfNodeId && id === selfNodeId) {
      continue;
    }
    if (address === selfHost || address.startsWith(`${selfHost}:`)) {
      continue;
    }
    if (seenAddress.has(address)) {
      continue;
    }
    seenAddress.add(address);
    out.push({ id, address });
  }
  return out;
}
