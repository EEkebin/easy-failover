// Server-side SSH onboarding route handler (POST /api/onboard).
//
// SECURITY / SERVER-ONLY: this handler runs entirely on the server (Node.js
// runtime). It imports the orchestrator, which imports ssh2 — a Node-only
// dependency that must never reach the browser. There is no client component
// here. The handler accepts an OnboardRequest (carrying ephemeral secrets),
// invokes the orchestrator, and streams back ONLY redacted progress + final
// status. Secrets are never echoed back.
//
// AUTHORIZATION (extension point): onboarding is a privileged remote write
// action. Per `docs/ssh-onboarding-design.md` and `docs/write-api-design.md`,
// this route MUST be gated by the authenticated write API before it is exposed:
// it requires an authenticated dashboard session and explicit write authority,
// is disabled by default, and is not reachable from the read-only local API. The
// token plumbing is out of scope for this backend issue; `authorizeWriteRequest`
// below is the single clear extension point where that check must be wired in.

import { onboard, type ProgressSink } from "../../../lib/onboarding/onboard";
import type { NodeStateSummary, OnboardRequest, StepProgress } from "../../../lib/onboarding/types";
import { appendNode, dashboardHasWriteAuthority } from "../../../lib/nodes/registry";
import { inheritedClusterPeers, mergePeers } from "../../../lib/onboarding/cluster";

// ssh2 is a Node-only dependency; pin this route to the Node.js runtime so it is
// never bundled for the edge/browser and so native-ish deps resolve correctly.
export const runtime = "nodejs";
// Onboarding mutates a remote host; never cache or statically optimize it.
export const dynamic = "force-dynamic";

/**
 * Authorize an onboarding request. Onboarding is a privileged remote write, so it
 * is gated on the dashboard holding a write token (the same token that authorizes
 * config writes — see the local node's `tokenEnv`). On a packaged install the
 * token is auto-provisioned, so onboarding works out of the box; a read-only
 * dashboard cannot onboard. Set `EASY_FAILOVER_ONBOARD_ENABLED=false` to force it
 * off regardless. Returns null when authorized, or a Response to short-circuit.
 */
function authorizeWriteRequest(_request: Request): Response | null {
  if (process.env.EASY_FAILOVER_ONBOARD_ENABLED === "false") {
    return Response.json(
      { error: "onboarding is disabled (EASY_FAILOVER_ONBOARD_ENABLED=false)" },
      { status: 403 }
    );
  }
  if (!dashboardHasWriteAuthority()) {
    return Response.json(
      {
        error:
          "onboarding requires a write token; this dashboard is read-only. " +
          "Configure the local node's write token (tokenEnv) to enable it."
      },
      { status: 403 }
    );
  }
  return null;
}

/** Minimal shape validation; the orchestrator and config gen validate further. */
function validateRequest(body: unknown): { ok: true; request: OnboardRequest } | { ok: false; error: string } {
  if (typeof body !== "object" || body === null) {
    return { ok: false, error: "request body must be a JSON object" };
  }
  const req = body as Partial<OnboardRequest>;
  if (!req.connection || typeof req.connection.host !== "string" || req.connection.host.trim() === "") {
    return { ok: false, error: "connection.host is required" };
  }
  if (typeof req.connection.username !== "string" || req.connection.username.trim() === "") {
    return { ok: false, error: "connection.username is required" };
  }
  if (!req.connection.auth || typeof req.connection.auth.kind !== "string") {
    return { ok: false, error: "connection.auth is required" };
  }
  if (!req.connection.sudo || typeof req.connection.sudo.kind !== "string") {
    return { ok: false, error: "connection.sudo is required" };
  }
  if (!req.source || typeof (req.source as { kind?: unknown }).kind !== "string") {
    return { ok: false, error: "source is required" };
  }
  if (!req.config || !req.config.vip || !Array.isArray(req.config.peers)) {
    return { ok: false, error: "config with vip and peers is required" };
  }
  return { ok: true, request: req as OnboardRequest };
}

/** Default node API port when the config doesn't pin one in `[api].bind`. */
const DEFAULT_API_PORT = 8743;

/**
 * Derive the dashboard-facing `apiBase` for a freshly onboarded node from its
 * host and the port in the request's `[api].bind` (e.g. "0.0.0.0:9000" -> 9000,
 * ":9000" -> 9000), falling back to the default port. We use the connection
 * host (the address the operator reached) rather than the bind host, because a
 * bind like 0.0.0.0 or 127.0.0.1 is not a reachable address from the dashboard.
 */
function deriveApiBase(req: OnboardRequest): string {
  const host = req.connection.host.trim();
  let port = DEFAULT_API_PORT;
  const bind = req.config.api?.bind;
  if (typeof bind === "string") {
    const colon = bind.lastIndexOf(":");
    if (colon >= 0) {
      const parsed = Number.parseInt(bind.slice(colon + 1).trim(), 10);
      if (Number.isInteger(parsed) && parsed > 0 && parsed <= 65535) {
        port = parsed;
      }
    }
  }
  return `http://${host}:${port}`;
}

/**
 * On a successful run, add the onboarded node to the file roster so it appears
 * in the fleet inventory (#95). No `tokenEnv` is set: a freshly onboarded node
 * is read-only in the dashboard until an operator wires a write token. This is
 * best-effort and never throws (appendNode is no-op-safe and swallows errors).
 */
function registerOnboardedNode(req: OnboardRequest, summary: NodeStateSummary): void {
  if (!summary.succeeded) {
    return;
  }
  const id = req.config.nodeId?.trim() || req.connection.host.trim();
  appendNode({ id, apiBase: deriveApiBase(req) });
}

export async function POST(request: Request): Promise<Response> {
  const denied = authorizeWriteRequest(request);
  if (denied) {
    return denied;
  }

  let body: unknown;
  try {
    body = await request.json();
  } catch {
    return Response.json({ error: "invalid JSON body" }, { status: 400 });
  }

  const validated = validateRequest(body);
  if (!validated.ok) {
    return Response.json({ error: validated.error }, { status: 400 });
  }

  // Auto-join the cluster: the new node inherits the current node's peers plus the
  // current node itself (addressed at the IP that routes to the target). Merged
  // with any operator-entered peers and de-duplicated. Best-effort — if the current
  // node's config can't be read, onboarding proceeds with the operator's peers.
  try {
    const inherited = await inheritedClusterPeers(validated.request.connection.host);
    if (inherited.length > 0) {
      validated.request.config.peers = mergePeers(
        validated.request.config.peers,
        inherited,
        validated.request.config.nodeId,
        validated.request.connection.host
      );
    }
  } catch {
    // Never block onboarding on the inheritance step.
  }

  // Stream redacted progress events as newline-delimited JSON (NDJSON), then a
  // final record with the summary. Only redacted strings ever leave the server.
  const encoder = new TextEncoder();
  const stream = new ReadableStream<Uint8Array>({
    async start(controller) {
      const write = (obj: unknown) => controller.enqueue(encoder.encode(`${JSON.stringify(obj)}\n`));
      const sink: ProgressSink = (event: StepProgress) => {
        write({ type: "progress", event });
      };
      try {
        const result = await onboard(validated.request, sink);
        // Add the node to the roster BEFORE the final result line so a client
        // that re-fetches /api/nodes on the result event sees it. Best-effort
        // and never throws; secrets are never persisted (only id + apiBase).
        registerOnboardedNode(validated.request, result.summary);
        write({ type: "result", summary: result.summary });
      } catch (err) {
        // Defensive: the orchestrator handles its own errors, but never leak a
        // raw secret-bearing stack to the client.
        write({ type: "error", error: "onboarding run failed unexpectedly" });
        void err;
      } finally {
        controller.close();
      }
    }
  });

  return new Response(stream, {
    status: 200,
    headers: {
      "Content-Type": "application/x-ndjson; charset=utf-8",
      "Cache-Control": "no-store"
    }
  });
}
