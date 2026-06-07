// Server-side SSH onboarding types.
//
// SECURITY: every type in this module is consumed only by server-side code
// (the orchestrator and the `/api/onboard` route handler). Request types carry
// ephemeral secrets (SSH password, private key, passphrase, sudo password) that
// must never be serialized into a client-bound response. Result/progress types
// are the only shapes returned to the browser, and every string they contain is
// passed through the redaction rule (see `redact.ts`) before it leaves the
// server.

/**
 * SSH authentication method. Exactly one variant is supplied per run.
 * Secrets here are ephemeral and are handed to the ssh2 auth layer out-of-band.
 */
export type SshAuth =
  | { kind: "password"; password: string }
  | { kind: "privateKey"; privateKey: string; passphrase?: string };

/**
 * How the run gains root on the target.
 * - `passwordless`: user has passwordless sudo.
 * - `password`: user has sudo but needs a password; fed via `sudo -S` on stdin.
 * - `already-root`: the login user is already root; no sudo is used.
 */
export type SudoMethod =
  | { kind: "passwordless" }
  | { kind: "password"; sudoPassword: string }
  | { kind: "already-root" };

/**
 * How easy-failover is placed on the target: a prebuilt release tarball, or a
 * build-from-source via CMake (`cmake` configure/build/install).
 */
export type InstallSource =
  | {
      kind: "release-tarball";
      /** Release tag (e.g. `v1.2.3`) or `latest`. */
      releaseRef: string;
      /**
       * Expected SHA-256 of the release tarball. SHA-256 verification is
       * mandatory; when set this value is also cross-checked against the
       * published `.sha256` via `sha256sum -c`. May be omitted to rely solely on
       * the published checksum file, but the on-target `sha256sum -c` is always
       * run regardless.
       */
      sha256?: string;
    }
  | {
      kind: "build-from-source";
      /** Branch, tag, or commit to build. Defaults to the repo default branch. */
      gitRef?: string;
    };

/** Host-key trust policy inputs (TOFU-with-pinning, optional strict mode). */
export type HostKeyPolicy = {
  /**
   * If supplied, the connection is STRICT: the server's host-key fingerprint
   * must equal this value or the run aborts at the Connect step. Format is the
   * base64 SHA-256 fingerprint (e.g. `SHA256:abc...`). When omitted, the key is
   * accepted on first connect (TOFU) and its fingerprint is pinned and returned
   * in the result so callers can store it and verify on later connections.
   */
  expectedFingerprint?: string;
};

/** A single peer entry for the generated config. */
export type PeerInput = {
  id: string;
  address: string;
};

/** Optional `[heartbeat]` overrides. */
export type HeartbeatInput = {
  bind?: string;
  intervalMs?: number;
  timeoutMs?: number;
};

/** Optional `[health]` overrides. */
export type HealthInput = {
  command?: string;
  intervalMs?: number;
  timeoutMs?: number;
};

/** Optional `[election]` overrides. */
export type ElectionInput = {
  requireQuorum?: boolean;
  preempt?: boolean;
};

/** Optional `[api]` overrides. */
export type ApiInput = {
  enabled?: boolean;
  bind?: string;
  readOnly?: boolean;
};

/**
 * The easy-failover config fields collected from the operator. These render to a
 * `config.toml` per `docs/config-reference.md`. `[mutation_safety]` is NOT part
 * of this shape: `allow_network_mutation = false` is always forced by config
 * generation and is never operator-controllable during onboarding.
 */
export type ConfigInput = {
  /** Defaults to the target hostname when blank. */
  nodeId?: string;
  priority?: number;
  vip: {
    address: string;
    interface: string;
  };
  /** At least one peer is required. */
  peers: PeerInput[];
  heartbeat?: HeartbeatInput;
  health?: HealthInput;
  election?: ElectionInput;
  api?: ApiInput;
};

/** Install knobs that map onto installer flags. */
export type InstallKnobs = {
  /** `--prefix`, default `/usr`. */
  prefix?: string;
  /** `--sysconfdir`, default `/etc`; config lands at `${sysconfdir}/easy-failover/config.toml`. */
  sysconfdir?: string;
};

/**
 * One onboarding run request. Carries ephemeral secrets; never echo back to the
 * client. The route handler accepts this shape (after auth) and passes it to the
 * orchestrator.
 */
export type OnboardRequest = {
  connection: {
    host: string;
    port?: number;
    username: string;
    auth: SshAuth;
    sudo: SudoMethod;
    hostKey?: HostKeyPolicy;
  };
  source: InstallSource;
  install?: InstallKnobs;
  config: ConfigInput;
  /**
   * If true, the run is allowed to overwrite an existing on-target config that
   * differs from the generated one. Defaults to false (the Write config step
   * fails rather than clobber a divergent config).
   */
  overwriteConfig?: boolean;
};

/** Status of a single onboarding step. */
export type StepStatus = "pending" | "running" | "ok" | "failed" | "skipped";

/** The ordered onboarding step identifiers. */
export type StepId =
  | "connect"
  | "detect"
  | "install-prereqs"
  | "fetch-build-install"
  | "write-config"
  | "validate-config"
  | "enable-start"
  | "verify";

/**
 * A progress event for one step. All free-form strings (`detail`, `stderr`,
 * `command`) are ALREADY redacted before this object is produced.
 */
export type StepProgress = {
  step: StepId;
  label: string;
  status: StepStatus;
  /** Redacted human-readable detail. */
  detail?: string;
  /** The redacted command line, if a remote command ran. */
  command?: string;
  /** Captured exit code for the last remote command in the step, if any. */
  exitCode?: number;
  /** Redacted stderr captured from a failed command. */
  stderr?: string;
};

/** Detected facts about the target collected during the Detect step. */
export type TargetFacts = {
  distroId?: string;
  distroVersion?: string;
  arch?: string;
  /** Detected init/service manager. */
  initSystem?: "systemd" | "openrc" | "runit" | "dinit" | "s6" | "unknown";
  /** Detected package manager. */
  packageManager?: "apt" | "dnf" | "pacman" | "unknown";
  /** Prerequisites that were already present. */
  presentPrereqs?: string[];
  /** Prerequisites that were missing and needed install. */
  missingPrereqs?: string[];
};

/**
 * The final node-state summary. `hostKeyFingerprint` is the pinned SHA-256
 * fingerprint of the target's host key (verified against `expectedFingerprint`
 * when strict, or newly pinned on TOFU).
 */
export type NodeStateSummary = {
  host: string;
  port: number;
  /** The last step that ran (whether it succeeded or failed). */
  reachedStep: StepId;
  /** Whether the run completed all steps successfully. */
  succeeded: boolean;
  /** Pinned host-key fingerprint (SHA256:...). */
  hostKeyFingerprint?: string;
  /** Whether host-key verification used strict mode. */
  hostKeyStrict: boolean;
  /** Detected target facts (as far as detection got). */
  facts?: TargetFacts;
  /** Redacted, human-readable description of the node's resulting state. */
  resultingState: string;
  /** Config path written on the target, if the Write config step ran. */
  configPath?: string;
};

/** The full result of an onboarding run. */
export type OnboardResult = {
  steps: StepProgress[];
  summary: NodeStateSummary;
};
