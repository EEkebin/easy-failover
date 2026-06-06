// Thin ssh2 execution layer for SSH onboarding.
//
// SECURITY / SERVER-ONLY: this module imports `ssh2`, a Node-only dependency. It
// must never be imported into a client component or run in the browser. Route
// handlers that use it must declare the Node.js runtime.
//
// This layer isolates the actual SSH I/O behind a small `runRemote(command,
// { stdin })` interface (`RemoteRunner`) so the orchestrator's per-step planning
// logic can be unit-tested with a fake runner and no network.
//
// Secrets travel out-of-band: SSH auth material is passed to the ssh2 connect
// config, and the sudo password is written to a command's stdin (`sudo -S`),
// never embedded in argv.

import { createHash } from "node:crypto";
import { Client, type ConnectConfig } from "ssh2";

import type { HostKeyPolicy, SshAuth } from "./types";

/** Result of a single remote command. */
export type RemoteResult = {
  /** Command exit code (or -1 if it was killed by a signal). */
  code: number;
  stdout: string;
  stderr: string;
};

/** Options for a single remote command. */
export type RemoteRunOptions = {
  /** Data written to the command's stdin (used to feed `sudo -S` a password). */
  stdin?: string;
};

/**
 * Minimal interface the orchestrator depends on. A real implementation wraps an
 * ssh2 connection; tests provide a fake.
 */
export type RemoteRunner = {
  run: (command: string, options?: RemoteRunOptions) => Promise<RemoteResult>;
  /** The pinned SHA-256 fingerprint of the connected server's host key. */
  hostKeyFingerprint: string;
  close: () => void;
};

/** Inputs needed to open an SSH connection. */
export type SshConnectInput = {
  host: string;
  port: number;
  username: string;
  auth: SshAuth;
  hostKey?: HostKeyPolicy;
  /** Connection-ready timeout in ms. */
  readyTimeoutMs?: number;
};

/** Error thrown when the host key fails strict verification. */
export class HostKeyMismatchError extends Error {
  constructor(
    readonly expected: string,
    readonly actual: string
  ) {
    super("host key fingerprint mismatch (strict mode): connection aborted");
    this.name = "HostKeyMismatchError";
  }
}

/** Compute the OpenSSH-style base64 SHA-256 fingerprint of a host key buffer. */
export function hostKeyFingerprint(key: Buffer): string {
  const digest = createHash("sha256").update(key).digest("base64").replace(/=+$/, "");
  return `SHA256:${digest}`;
}

/** Normalize fingerprints for comparison (tolerate missing `SHA256:` prefix). */
function normalizeFingerprint(value: string): string {
  return value.trim().replace(/^SHA256:/i, "");
}

/**
 * Apply the host-key policy to a presented key.
 * - Strict (expectedFingerprint set): must match exactly or throw.
 * - TOFU (no expected fingerprint): accept and return the pinned fingerprint.
 *
 * Returns the pinned fingerprint (always `SHA256:...`).
 */
export function verifyHostKey(key: Buffer, policy: HostKeyPolicy | undefined): string {
  const actual = hostKeyFingerprint(key);
  const expected = policy?.expectedFingerprint;
  if (expected && expected.trim().length > 0) {
    if (normalizeFingerprint(actual) !== normalizeFingerprint(expected)) {
      throw new HostKeyMismatchError(expected, actual);
    }
  }
  return actual;
}

/** Build the ssh2 connect config from auth material (kept out of argv). */
function buildConnectConfig(input: SshConnectInput, onHostKey: (key: Buffer) => boolean): ConnectConfig {
  const base: ConnectConfig = {
    host: input.host,
    port: input.port,
    username: input.username,
    readyTimeout: input.readyTimeoutMs ?? 20000,
    // TOFU-with-pinning: ssh2 hands us the raw host key; we pin/verify it
    // ourselves and reject by returning false (strict mismatch).
    hostVerifier: (key: Buffer) => onHostKey(key)
  };

  if (input.auth.kind === "password") {
    return { ...base, password: input.auth.password };
  }
  return {
    ...base,
    privateKey: input.auth.privateKey,
    passphrase: input.auth.passphrase
  };
}

/**
 * Open an SSH connection and return a {@link RemoteRunner}. Applies the host-key
 * policy during the handshake (strict abort on mismatch, otherwise TOFU pin).
 *
 * The caller MUST `close()` the returned runner when the run finishes or fails,
 * so the ephemeral connection (and the secrets it holds) is dropped promptly.
 */
export function connect(input: SshConnectInput): Promise<RemoteRunner> {
  return new Promise((resolve, reject) => {
    const client = new Client();
    let pinned = "";
    let hostKeyError: Error | undefined;

    const handleHostKey = (key: Buffer): boolean => {
      try {
        pinned = verifyHostKey(key, input.hostKey);
        return true;
      } catch (err) {
        hostKeyError = err instanceof Error ? err : new Error(String(err));
        return false;
      }
    };

    client.on("ready", () => {
      const runner: RemoteRunner = {
        hostKeyFingerprint: pinned,
        close: () => client.end(),
        run: (command: string, options?: RemoteRunOptions) =>
          new Promise<RemoteResult>((resolveRun, rejectRun) => {
            client.exec(command, (err, stream) => {
              if (err) {
                rejectRun(err);
                return;
              }
              let stdout = "";
              let stderr = "";
              let code = -1;
              stream
                .on("close", (exitCode: number | null) => {
                  code = typeof exitCode === "number" ? exitCode : -1;
                  resolveRun({ code, stdout, stderr });
                })
                .on("data", (chunk: Buffer) => {
                  stdout += chunk.toString("utf8");
                });
              stream.stderr.on("data", (chunk: Buffer) => {
                stderr += chunk.toString("utf8");
              });
              if (options?.stdin !== undefined) {
                stream.end(options.stdin);
              } else {
                stream.end();
              }
            });
          })
      };
      resolve(runner);
    });

    client.on("error", (err) => {
      // Surface the host-key mismatch as the cause when present; it is more
      // actionable than ssh2's generic handshake error.
      reject(hostKeyError ?? err);
    });

    try {
      client.connect(buildConnectConfig(input, handleHostKey));
    } catch (err) {
      reject(err);
    }
  });
}
