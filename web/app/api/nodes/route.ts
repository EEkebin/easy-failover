// Controller route: GET /api/nodes — the fleet list.
//
// This is the server-side proxy the browser talks to instead of fetching node
// daemons directly. For each roster node it probes /api/v1/status with a short
// timeout and returns a browser-safe NodeSummary, flagging reachability. A node
// that is unreachable or errors is marked unreachable; it never throws and never
// fails the whole list. `tokenEnv` / token contents are never included.

import { listNodes } from "../../../lib/nodes/registry";
import { probeSummary } from "../../../lib/nodes/probe";
import type { NodeSummary } from "../../../lib/nodes/types";

// The roster registry and probe use node:fs / outbound fetch — pin to Node.
export const runtime = "nodejs";
// Always reflect live roster + reachability; never cache or statically optimize.
export const dynamic = "force-dynamic";

export async function GET(): Promise<Response> {
  const entries = listNodes();
  // Probe all nodes concurrently; probeSummary never rejects.
  const nodes: NodeSummary[] = await Promise.all(entries.map(probeSummary));
  return Response.json({ nodes }, { headers: { "Cache-Control": "no-store" } });
}
