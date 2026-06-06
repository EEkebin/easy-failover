// Controller route: GET /api/nodes/[id] — one node's detail view.
//
// Resolves the node by roster id and returns a NodeView with its full
// status/config/events (each endpoint tolerated individually), flagging
// reachability. 404 when the id isn't in the roster. Like the list route, this
// is the only path the browser uses to reach a node; it redacts nothing beyond
// what the daemon already redacts and never exposes `tokenEnv` / token contents.

import { getNode } from "../../../../lib/nodes/registry";
import { probeView } from "../../../../lib/nodes/probe";

// Roster registry / probe use node:fs / outbound fetch — pin to Node.
export const runtime = "nodejs";
// Always reflect live node state; never cache or statically optimize.
export const dynamic = "force-dynamic";

export async function GET(
  _request: Request,
  context: { params: Promise<{ id: string }> }
): Promise<Response> {
  const { id } = await context.params;
  const entry = getNode(id);
  if (!entry) {
    return Response.json(
      { error: `node not found: ${id}` },
      { status: 404, headers: { "Cache-Control": "no-store" } }
    );
  }
  const view = await probeView(entry);
  return Response.json(view, { headers: { "Cache-Control": "no-store" } });
}
