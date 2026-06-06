// Controller route: GET/POST /api/nodes/[id]/config — read + apply one node's config.
//
// This is the server-side proxy the browser talks to for the config editor. The
// browser never reaches the daemon directly and never sees the write token.
//
// SECURITY: the write bearer token is read ONLY here, server-side, from the env
// var named by `node.tokenEnv`. It is attached as `Authorization: Bearer <token>`
// when proxying to the daemon's authenticated apply endpoint and is NEVER placed
// in any response body, header, or log line.
//
// GET  -> proxies daemon `GET /api/v1/config` (effective, redacted) and adds a
//         `writable` boolean (true when a token is configured + present).
// POST -> body is the candidate config TOML string built by the client. We first
//         proxy `POST /api/v1/config/validate`; if invalid we return
//         { applied:false, errors } (HTTP 200). If valid AND the node is writable
//         we proxy the authenticated `POST /api/v1/config/apply` with the bearer
//         token and return the daemon's result. Non-writable nodes get a clear
//         409; 401/403 from the daemon become a clear "authentication failed".

import { getNode } from "../../../../../lib/nodes/registry";
import type { NodeEntry } from "../../../../../lib/nodes/types";
import type { ConfigResponse } from "../../../../../lib/api";

// Registry uses node:fs and we do outbound fetch + read process.env — pin Node.
export const runtime = "nodejs";
// Always reflect live config + token availability; never cache.
export const dynamic = "force-dynamic";

const PROXY_TIMEOUT_MS = 5000;

function trimBase(apiBase: string): string {
  return apiBase.replace(/\/$/, "");
}

/** Resolve this node's write token from its tokenEnv, or null if unavailable. */
function resolveToken(entry: NodeEntry): string | null {
  if (typeof entry.tokenEnv !== "string" || entry.tokenEnv.trim().length === 0) {
    return null;
  }
  const value = process.env[entry.tokenEnv];
  if (typeof value !== "string" || value.length === 0) {
    return null;
  }
  return value;
}

/** True when a usable write token is configured + present for this node. */
function isWritable(entry: NodeEntry): boolean {
  return resolveToken(entry) !== null;
}

function notFound(id: string): Response {
  return Response.json(
    { error: `node not found: ${id}` },
    { status: 404, headers: { "Cache-Control": "no-store" } }
  );
}

/** Fetch with a hard timeout so a stuck daemon can't hang the request. */
async function fetchWithTimeout(url: string, init: RequestInit): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), PROXY_TIMEOUT_MS);
  try {
    return await fetch(url, { ...init, signal: controller.signal, cache: "no-store" });
  } finally {
    clearTimeout(timer);
  }
}

export async function GET(
  _request: Request,
  context: { params: Promise<{ id: string }> }
): Promise<Response> {
  const { id } = await context.params;
  const entry = getNode(id);
  if (!entry) {
    return notFound(id);
  }

  const writable = isWritable(entry);
  try {
    const response = await fetchWithTimeout(`${trimBase(entry.apiBase)}/api/v1/config`, {
      headers: { Accept: "application/json" }
    });
    if (!response.ok) {
      return Response.json(
        { reachable: false, writable, error: `status ${response.status}` },
        { status: 200, headers: { "Cache-Control": "no-store" } }
      );
    }
    const config = (await response.json()) as ConfigResponse;
    return Response.json(
      { reachable: true, writable, config },
      { headers: { "Cache-Control": "no-store" } }
    );
  } catch (err) {
    const reason =
      err instanceof Error && err.name === "AbortError" ? "timed out" : "unreachable";
    return Response.json(
      { reachable: false, writable, error: reason },
      { status: 200, headers: { "Cache-Control": "no-store" } }
    );
  }
}

/** Read the candidate TOML string from the request body. */
async function readCandidate(request: Request): Promise<string | null> {
  try {
    const body = (await request.json()) as { config?: unknown };
    if (typeof body.config === "string" && body.config.length > 0) {
      return body.config;
    }
  } catch {
    // fall through
  }
  return null;
}

export async function POST(
  request: Request,
  context: { params: Promise<{ id: string }> }
): Promise<Response> {
  const { id } = await context.params;
  const entry = getNode(id);
  if (!entry) {
    return notFound(id);
  }

  const candidate = await readCandidate(request);
  if (candidate === null) {
    return Response.json(
      { applied: false, error: "request body must include a non-empty config string" },
      { status: 400, headers: { "Cache-Control": "no-store" } }
    );
  }

  const base = trimBase(entry.apiBase);
  const payload = JSON.stringify({ format: "toml", config: candidate });

  // 1. Validate (unauthenticated, read-only). On invalid config, stop here.
  let validateResult: { valid?: boolean; errors?: unknown };
  try {
    const validateResponse = await fetchWithTimeout(`${base}/api/v1/config/validate`, {
      method: "POST",
      headers: { "Content-Type": "application/json", Accept: "application/json" },
      body: payload
    });
    if (!validateResponse.ok) {
      return Response.json(
        { applied: false, error: `validation request failed: status ${validateResponse.status}` },
        { status: 502, headers: { "Cache-Control": "no-store" } }
      );
    }
    validateResult = (await validateResponse.json()) as { valid?: boolean; errors?: unknown };
  } catch (err) {
    const reason =
      err instanceof Error && err.name === "AbortError" ? "timed out" : "unreachable";
    return Response.json(
      { applied: false, error: `could not reach node for validation: ${reason}` },
      { status: 502, headers: { "Cache-Control": "no-store" } }
    );
  }

  if (validateResult.valid !== true) {
    const errors = Array.isArray(validateResult.errors) ? validateResult.errors : [];
    return Response.json(
      { applied: false, errors },
      { headers: { "Cache-Control": "no-store" } }
    );
  }

  // 2. Apply requires a write token. Fail closed with a clear 409 if absent.
  const token = resolveToken(entry);
  if (token === null) {
    return Response.json(
      { applied: false, error: "node is not configured for writes (no token)" },
      { status: 409, headers: { "Cache-Control": "no-store" } }
    );
  }

  // 3. Apply (authenticated). The token is attached here and never echoed back.
  try {
    const applyResponse = await fetchWithTimeout(`${base}/api/v1/config/apply`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Accept: "application/json",
        Authorization: `Bearer ${token}`
      },
      body: payload
    });

    if (applyResponse.status === 401 || applyResponse.status === 403) {
      return Response.json(
        {
          applied: false,
          error: "authentication failed: the node rejected the write token"
        },
        { status: 200, headers: { "Cache-Control": "no-store" } }
      );
    }

    if (!applyResponse.ok) {
      return Response.json(
        { applied: false, error: `apply failed: status ${applyResponse.status}` },
        { status: 502, headers: { "Cache-Control": "no-store" } }
      );
    }

    // Pass through the daemon's apply result (applied, backup_path, errors, ...).
    // The token is never part of this body.
    const result = (await applyResponse.json()) as Record<string, unknown>;
    return Response.json(result, { headers: { "Cache-Control": "no-store" } });
  } catch (err) {
    const reason =
      err instanceof Error && err.name === "AbortError" ? "timed out" : "unreachable";
    return Response.json(
      { applied: false, error: `could not reach node to apply: ${reason}` },
      { status: 502, headers: { "Cache-Control": "no-store" } }
    );
  }
}
