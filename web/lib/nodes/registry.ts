// Server-only roster registry for the multi-node dashboard.
//
// SERVER-ONLY: this module uses `node:fs` to read/write the roster file and may
// later resolve write-token env vars. It must never be bundled for the browser.
// The `server-only` import below makes any client import a build-time error.
//
// The roster (pool membership) is resolved here, on the server, in priority
// order so that no single node owns the node list:
//   1. JSON file at $EASY_FAILOVER_NODES_FILE (default
//      /etc/easy-failover/dashboard-nodes.json) — an array of NodeEntry.
//   2. $EASY_FAILOVER_NODES — comma list of "id=apiBase" pairs.
//   3. Single fallback node { id: "local", apiBase: <default> } so a fresh
//      `npm run dev` with no roster still resolves to something.
//
// Resolution is defensive: a missing or malformed file/env never throws to the
// caller; it simply falls through to the next source.

import "server-only";

import { readFileSync, writeFileSync, renameSync, mkdirSync } from "node:fs";
import { dirname } from "node:path";

import type { NodeEntry } from "./types";

const DEFAULT_NODES_FILE = "/etc/easy-failover/dashboard-nodes.json";
const DEFAULT_API_BASE = "http://127.0.0.1:8743";

/** Path to the roster JSON file (may or may not exist on disk). */
function nodesFilePath(): string {
  const fromEnv = process.env.EASY_FAILOVER_NODES_FILE;
  if (typeof fromEnv === "string" && fromEnv.trim().length > 0) {
    return fromEnv.trim();
  }
  return DEFAULT_NODES_FILE;
}

/** True when the roster file source is the one actually in use. */
function fileSourceConfigured(): boolean {
  // The file source is "in use" when an explicit path is set, OR when the
  // default path exists and parses. We treat an explicit env var as opt-in.
  if (typeof process.env.EASY_FAILOVER_NODES_FILE === "string" && process.env.EASY_FAILOVER_NODES_FILE.trim().length > 0) {
    return true;
  }
  return readNodesFile(DEFAULT_NODES_FILE) !== null;
}

function defaultApiBase(): string {
  const fromEnv = process.env.NEXT_PUBLIC_EASY_FAILOVER_API_BASE;
  if (typeof fromEnv === "string" && fromEnv.trim().length > 0) {
    return fromEnv.trim();
  }
  return DEFAULT_API_BASE;
}

/** Coerce one parsed JSON value into a NodeEntry, or null if unusable. */
function coerceEntry(value: unknown): NodeEntry | null {
  if (typeof value !== "object" || value === null) {
    return null;
  }
  const record = value as Record<string, unknown>;
  const id = record.id;
  const apiBase = record.apiBase;
  if (typeof id !== "string" || id.trim().length === 0) {
    return null;
  }
  if (typeof apiBase !== "string" || apiBase.trim().length === 0) {
    return null;
  }
  const entry: NodeEntry = { id: id.trim(), apiBase: apiBase.trim() };
  if (typeof record.label === "string" && record.label.trim().length > 0) {
    entry.label = record.label.trim();
  }
  if (typeof record.tokenEnv === "string" && record.tokenEnv.trim().length > 0) {
    entry.tokenEnv = record.tokenEnv.trim();
  }
  return entry;
}

/**
 * Read and parse the roster file. Returns an array of entries, or null when the
 * file is absent or unreadable/unparseable (so callers can fall through). A file
 * that parses but contains no valid entries returns an empty array.
 */
function readNodesFile(path: string): NodeEntry[] | null {
  let raw: string;
  try {
    raw = readFileSync(path, "utf8");
  } catch {
    return null; // missing or unreadable -> not in use
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return null; // malformed JSON -> fall through gracefully
  }
  if (!Array.isArray(parsed)) {
    return null;
  }
  const entries: NodeEntry[] = [];
  const seen = new Set<string>();
  for (const item of parsed) {
    const entry = coerceEntry(item);
    if (entry && !seen.has(entry.id)) {
      seen.add(entry.id);
      entries.push(entry);
    }
  }
  return entries;
}

/** Parse "$EASY_FAILOVER_NODES" comma list of "id=apiBase". */
function readNodesEnv(): NodeEntry[] | null {
  const raw = process.env.EASY_FAILOVER_NODES;
  if (typeof raw !== "string" || raw.trim().length === 0) {
    return null;
  }
  const entries: NodeEntry[] = [];
  const seen = new Set<string>();
  for (const part of raw.split(",")) {
    const pair = part.trim();
    if (pair.length === 0) {
      continue;
    }
    const eq = pair.indexOf("=");
    if (eq <= 0) {
      continue; // need a non-empty id before '='
    }
    const id = pair.slice(0, eq).trim();
    const apiBase = pair.slice(eq + 1).trim();
    if (id.length === 0 || apiBase.length === 0 || seen.has(id)) {
      continue;
    }
    seen.add(id);
    entries.push({ id, apiBase });
  }
  return entries.length > 0 ? entries : null;
}

/**
 * Resolve the roster in priority order: file, then env, then a single local
 * fallback. Never throws: invalid sources fall through to the next one.
 */
export function listNodes(): NodeEntry[] {
  const fromFile = readNodesFile(nodesFilePath());
  if (fromFile && fromFile.length > 0) {
    return fromFile;
  }

  const fromEnv = readNodesEnv();
  if (fromEnv && fromEnv.length > 0) {
    return fromEnv;
  }

  return [{ id: "local", apiBase: defaultApiBase() }];
}

/** Resolve a single roster entry by id, or null if not present. */
export function getNode(id: string): NodeEntry | null {
  for (const entry of listNodes()) {
    if (entry.id === id) {
      return entry;
    }
  }
  return null;
}

/**
 * Append a node to the roster JSON file, creating the file (and parent dir) if
 * missing. Used later by the onboarding wizard (#101).
 *
 * - No-op-safe: if the file roster source is not in use (no explicit
 *   EASY_FAILOVER_NODES_FILE and no default file present), this returns without
 *   writing, so we never silently create a roster that env-based deployments
 *   wouldn't read.
 * - If an entry with the same id already exists, it is left unchanged.
 *
 * Returns true when the entry was written, false when skipped (not in use or
 * duplicate id).
 */
export function appendNode(entry: NodeEntry): boolean {
  const coerced = coerceEntry(entry);
  if (!coerced) {
    return false;
  }
  if (!fileSourceConfigured()) {
    return false;
  }

  const path = nodesFilePath();
  const existing = readNodesFile(path) ?? [];
  if (existing.some((e) => e.id === coerced.id)) {
    return false; // duplicate id: leave roster unchanged
  }

  const next = [...existing, coerced];
  try {
    mkdirSync(dirname(path), { recursive: true });
    const tmp = `${path}.${process.pid}.tmp`;
    writeFileSync(tmp, `${JSON.stringify(next, null, 2)}\n`, "utf8");
    renameSync(tmp, path); // atomic on POSIX: concurrent onboarding completions can't corrupt
    return true;
  } catch {
    return false; // never throw to caller
  }
}
