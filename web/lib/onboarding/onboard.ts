// SSH onboarding orchestrator.
//
// Runs the step sequence from `docs/ssh-onboarding-design.md`:
//   connect -> detect -> install-prereqs -> fetch-build-install ->
//   write-config -> validate-config -> enable-start -> verify
//
// SECURITY / SERVER-ONLY: this module drives ssh2 (via `ssh.ts`) and holds the
// run's ephemeral secrets in memory only for the duration of the run. Nothing
// here is importable into a client component. Every outbound string is passed
// through the run's redactor before it is yielded as a progress event. Secrets
// travel out-of-band: SSH auth via the ssh2 connect config, the sudo password
// via `sudo -S` stdin — never as argv.
//
// The per-step planning logic lives in `plan.ts` (pure, unit-testable). The
// actual ssh2 execution is isolated behind the `RemoteRunner` interface so the
// orchestrator can be exercised against a fake runner with no network.

import { generateConfigToml } from "./config";
import {
  configPath,
  detectFactsCommand,
  enableStartCommand,
  installPackagesCommand,
  packageFor,
  parseDetectOutput,
  privileged,
  readConfigCommand,
  releaseInstallCommand,
  requiredPrereqs,
  sourceInstallCommand,
  validateConfigCommand,
  verifyApiCommand,
  verifyServiceCommand,
  writeConfigCommand,
  type PlannedCommand
} from "./plan";
import { createRedactor, type Redactor } from "./redact";
import { connect, HostKeyMismatchError, type RemoteResult, type RemoteRunner } from "./ssh";
import type {
  NodeStateSummary,
  OnboardRequest,
  OnboardResult,
  StepId,
  StepProgress,
  TargetFacts
} from "./types";

const STEP_LABELS: Record<StepId, string> = {
  connect: "Connect",
  detect: "Detect distro and prereqs",
  "install-prereqs": "Install prereqs",
  "fetch-build-install": "Fetch / build / install easy-failover",
  "write-config": "Write config",
  "validate-config": "Validate config",
  "enable-start": "Enable and start the service",
  verify: "Verify"
};

const STEP_ORDER: StepId[] = [
  "connect",
  "detect",
  "install-prereqs",
  "fetch-build-install",
  "write-config",
  "validate-config",
  "enable-start",
  "verify"
];

/** Default install knobs. */
const DEFAULT_PREFIX = "/usr";
const DEFAULT_SYSCONFDIR = "/etc";
const DEFAULT_PORT = 22;

/**
 * A consumer of streamed progress. The orchestrator calls this for every step
 * transition. Strings are already redacted.
 */
export type ProgressSink = (event: StepProgress) => void | Promise<void>;

/** Dependency injection point so tests can supply a fake connect(). */
export type OnboardDeps = {
  connect: typeof connect;
};

const defaultDeps: OnboardDeps = { connect };

/**
 * Turn a confusing sudo failure into an actionable hint. Returns undefined when
 * the stderr doesn't look like a sudo auth problem. No secrets are involved.
 */
function diagnoseSudoFailure(
  stderr: string,
  sudoKind: OnboardRequest["connection"]["sudo"]["kind"]
): string | undefined {
  const s = stderr.toLowerCase();
  if (/not in the sudoers file|may not run sudo|user .* is not allowed/.test(s)) {
    return "This user lacks sudo rights on the target. Use an account with sudo, or the 'already root' method.";
  }
  if (/a terminal is required to authenticate|sudo: a password is required|no askpass|tty/.test(s)) {
    if (sudoKind === "passwordless") {
      return "This account does not have passwordless sudo. Re-run with the 'password' sudo method and enter the sudo password.";
    }
    return "sudo could not authenticate without a terminal. Use the 'password' sudo method so the password is fed on stdin.";
  }
  if (/incorrect password|sorry, try again|authentication failure/.test(s)) {
    return "The sudo password was rejected. Check the sudo password and try again.";
  }
  return undefined;
}

/** Build the run's redactor from its ephemeral secrets. */
function redactorFor(req: OnboardRequest): Redactor {
  const sshPassword = req.connection.auth.kind === "password" ? req.connection.auth.password : undefined;
  const privateKey = req.connection.auth.kind === "privateKey" ? req.connection.auth.privateKey : undefined;
  const passphrase =
    req.connection.auth.kind === "privateKey" ? req.connection.auth.passphrase : undefined;
  const sudoPassword = req.connection.sudo.kind === "password" ? req.connection.sudo.sudoPassword : undefined;
  return createRedactor([sshPassword, privateKey, passphrase, sudoPassword]);
}

/** The sudo password for this run, if any (fed on stdin, never argv). */
function sudoPasswordFor(req: OnboardRequest): string | undefined {
  return req.connection.sudo.kind === "password" ? req.connection.sudo.sudoPassword : undefined;
}

/** Run a planned (possibly privileged) command, feeding the sudo password if needed. */
async function runPlanned(
  runner: RemoteRunner,
  planned: PlannedCommand,
  sudoPassword: string | undefined,
  extraStdin?: string
): Promise<RemoteResult> {
  let stdin: string | undefined;
  if (planned.needsSudoPassword && sudoPassword !== undefined) {
    // sudo -S consumes the first line as the password; append any extra stdin
    // (e.g. config content) after the newline.
    stdin = `${sudoPassword}\n${extraStdin ?? ""}`;
  } else {
    stdin = extraStdin;
  }
  return runner.run(planned.command, stdin !== undefined ? { stdin } : undefined);
}

/**
 * Orchestrate one onboarding run. Yields redacted progress through `onProgress`
 * and returns the final {@link OnboardResult}. Stops the sequence on the first
 * failure and records the node's resulting state. Credentials are held only for
 * the duration of this call.
 */
export async function onboard(
  req: OnboardRequest,
  onProgress: ProgressSink = () => {},
  deps: OnboardDeps = defaultDeps
): Promise<OnboardResult> {
  const redactor = redactorFor(req);
  const sudoPassword = sudoPasswordFor(req);
  const prefix = req.install?.prefix ?? DEFAULT_PREFIX;
  const sysconfdir = req.install?.sysconfdir ?? DEFAULT_SYSCONFDIR;
  const port = req.connection.port ?? DEFAULT_PORT;
  const strict = Boolean(req.connection.hostKey?.expectedFingerprint?.trim());

  const steps: StepProgress[] = [];
  let facts: TargetFacts | undefined;
  let hostKeyFingerprint: string | undefined;
  let reachedStep: StepId = "connect";
  let runner: RemoteRunner | undefined;

  // Helper to emit a progress event (already redacted) and record it.
  const emit = async (event: StepProgress) => {
    const redacted: StepProgress = {
      ...event,
      detail: event.detail !== undefined ? redactor.redact(event.detail) : undefined,
      command: event.command !== undefined ? redactor.redact(event.command) : undefined,
      stderr: event.stderr !== undefined ? redactor.redact(event.stderr) : undefined
    };
    steps.push(redacted);
    await onProgress(redacted);
  };

  // Wrap a single command execution with running/ok/failed progress emission.
  // Returns the result, or undefined if the command failed (caller stops).
  const execStep = async (
    step: StepId,
    planned: PlannedCommand,
    extraStdin?: string,
    okDetail?: (r: RemoteResult) => string
  ): Promise<RemoteResult | undefined> => {
    reachedStep = step;
    await emit({ step, label: STEP_LABELS[step], status: "running", command: planned.command });
    const result = await runPlanned(runner!, planned, sudoPassword, extraStdin);
    if (result.code !== 0) {
      const sudoHint = diagnoseSudoFailure(
        result.stderr || result.stdout || "",
        req.connection.sudo.kind
      );
      await emit({
        step,
        label: STEP_LABELS[step],
        status: "failed",
        command: planned.command,
        exitCode: result.code,
        stderr: result.stderr || result.stdout,
        ...(sudoHint ? { detail: sudoHint } : {})
      });
      return undefined;
    }
    await emit({
      step,
      label: STEP_LABELS[step],
      status: "ok",
      command: planned.command,
      exitCode: result.code,
      detail: okDetail ? okDetail(result) : undefined
    });
    return result;
  };

  const summarize = (succeeded: boolean, resultingState: string): NodeStateSummary => ({
    host: req.connection.host,
    port,
    reachedStep,
    succeeded,
    hostKeyFingerprint,
    hostKeyStrict: strict,
    facts,
    resultingState: redactor.redact(resultingState),
    configPath: configPath(sysconfdir)
  });

  try {
    // Step 1: Connect (apply host-key policy).
    await emit({ step: "connect", label: STEP_LABELS.connect, status: "running" });
    try {
      runner = await deps.connect({
        host: req.connection.host,
        port,
        username: req.connection.username,
        auth: req.connection.auth,
        hostKey: req.connection.hostKey
      });
    } catch (err) {
      const isMismatch = err instanceof HostKeyMismatchError;
      const detail = isMismatch
        ? "host key fingerprint mismatch; connection aborted (strict mode)"
        : `connection failed: ${redactor.redact(errorMessage(err))}`;
      await emit({ step: "connect", label: STEP_LABELS.connect, status: "failed", detail });
      return {
        steps,
        summary: summarize(
          false,
          isMismatch ? "untouched: host key mismatch, aborted before connect" : "untouched: connection failed"
        )
      };
    }
    hostKeyFingerprint = runner.hostKeyFingerprint;
    await emit({
      step: "connect",
      label: STEP_LABELS.connect,
      status: "ok",
      detail: `connected to ${req.connection.host}:${port} as ${req.connection.username}; host key ${
        strict ? "verified (strict)" : "pinned (TOFU)"
      } ${hostKeyFingerprint}`
    });

    // Step 2: Detect distro/arch/init/prereqs (no changes).
    {
      reachedStep = "detect";
      const cmd = detectFactsCommand();
      await emit({ step: "detect", label: STEP_LABELS.detect, status: "running", command: cmd });
      const result = await runner.run(cmd);
      if (result.code !== 0) {
        await emit({
          step: "detect",
          label: STEP_LABELS.detect,
          status: "failed",
          command: cmd,
          exitCode: result.code,
          stderr: result.stderr
        });
        return { steps, summary: summarize(false, "untouched: detection failed") };
      }
      facts = parseDetectOutput(result.stdout);
      await emit({
        step: "detect",
        label: STEP_LABELS.detect,
        status: "ok",
        command: cmd,
        detail: `distro=${facts.distroId ?? "?"} ${facts.distroVersion ?? ""} arch=${facts.arch ?? "?"} init=${
          facts.initSystem
        } pkg=${facts.packageManager} missing=[${(facts.missingPrereqs ?? []).join(",")}]`
      });
    }

    // Step 3: Install prereqs (idempotent; skip when none missing).
    {
      const needed = requiredPrereqs(req.source);
      const missing = needed.filter((tool) => (facts!.missingPrereqs ?? []).includes(tool));
      if (missing.length === 0) {
        reachedStep = "install-prereqs";
        await emit({
          step: "install-prereqs",
          label: STEP_LABELS["install-prereqs"],
          status: "skipped",
          detail: "all required prerequisites already present"
        });
      } else {
        const packages = [...new Set(missing.map((tool) => packageFor(tool, facts!.packageManager)))];
        const planned = privileged(installPackagesCommand(packages, facts!.packageManager), req.connection.sudo);
        const result = await execStep(
          "install-prereqs",
          planned,
          undefined,
          () => `installed ${packages.join(", ")} via ${facts!.packageManager}`
        );
        if (!result) {
          return { steps, summary: summarize(false, "prereq install failed; no config written, service not enabled") };
        }
      }
    }

    // Step 4: Fetch / build / install easy-failover.
    {
      const body =
        req.source.kind === "release-tarball"
          ? releaseInstallCommand(req.source, prefix, sysconfdir)
          : sourceInstallCommand(req.source, prefix, sysconfdir);
      // privileged() wraps the body in `sh -c` for sudo methods, so the whole
      // multi-command install body runs as root.
      const planned = privileged(body, req.connection.sudo);
      const result = await execStep(
        "fetch-build-install",
        planned,
        undefined,
        () =>
          req.source.kind === "release-tarball"
            ? `installed release ${req.source.releaseRef} (sha256 verified) into ${prefix}`
            : `built and installed from source into ${prefix}`
      );
      if (!result) {
        return {
          steps,
          summary: summarize(false, "install failed; possible partial install, no config written, service not enabled")
        };
      }
    }

    // Step 5: Write config (do not clobber a divergent existing config silently).
    {
      const tokenPath = `${sysconfdir.replace(/\/$/, "")}/easy-failover/api.token`;
      const generated = generateConfigToml(req.config, { authTokenFile: tokenPath });
      reachedStep = "write-config";
      // First, read any existing config to detect divergence. Run privileged: a
      // root-only (0600) existing config would otherwise read empty under the
      // login user, silently bypassing the no-clobber guard below.
      const existingPlanned = privileged(readConfigCommand(sysconfdir), req.connection.sudo);
      const existing = await runPlanned(runner, existingPlanned, sudoPassword);
      const existingContent = existing.code === 0 ? existing.stdout : "";
      if (existingContent.trim().length > 0 && existingContent.trim() !== generated.trim() && !req.overwriteConfig) {
        await emit({
          step: "write-config",
          label: STEP_LABELS["write-config"],
          status: "failed",
          detail:
            "an existing config.toml differs from the generated config; refusing to overwrite (set overwriteConfig to proceed)"
        });
        return {
          steps,
          summary: summarize(false, "installed but existing config differs; not overwritten, service not enabled")
        };
      }
      const planned = privileged(writeConfigCommand(sysconfdir), req.connection.sudo);
      const result = await execStep(
        "write-config",
        planned,
        generated,
        () => `wrote ${configPath(sysconfdir)} (vip=${req.config.vip.address}, peers=${req.config.peers.length})`
      );
      if (!result) {
        return { steps, summary: summarize(false, "installed but config write failed; service not enabled") };
      }
    }

    // Step 6: Validate config on the target.
    {
      const planned = privileged(validateConfigCommand(prefix, sysconfdir), req.connection.sudo);
      const result = await execStep("validate-config", planned, undefined, () => "config validated on target");
      if (!result) {
        return { steps, summary: summarize(false, "installed, config written, but validation failed; service not enabled") };
      }
    }

    // Step 7: Enable and start the service via the detected init system.
    {
      const planned = privileged(enableStartCommand(facts!.initSystem), req.connection.sudo);
      const result = await execStep(
        "enable-start",
        planned,
        undefined,
        () => `enabled and started easy-failover via ${facts!.initSystem}`
      );
      if (!result) {
        return {
          steps,
          summary: summarize(false, "installed and validated, but enable/start failed")
        };
      }
    }

    // Step 8: Verify (service active; optional API poll).
    {
      reachedStep = "verify";
      const planned = privileged(verifyServiceCommand(facts!.initSystem), req.connection.sudo);
      await emit({ step: "verify", label: STEP_LABELS.verify, status: "running", command: planned.command });
      const svc = await runPlanned(runner, planned, sudoPassword);
      const active = svc.code === 0 && /active|running|up|true/i.test(svc.stdout);

      let apiDetail = "";
      // The generated config always enables the API (matching a packaged install),
      // unless the operator explicitly disabled it.
      if (req.config.api?.enabled !== false) {
        const bind = req.config.api?.bind ?? "0.0.0.0:8743";
        const apiPlanned = privileged(verifyApiCommand(bind), req.connection.sudo);
        const apiResult = await runPlanned(runner, apiPlanned, sudoPassword);
        apiDetail = apiResult.code === 0 ? `; API reachable at ${bind}` : `; API not yet reachable at ${bind}`;
      }

      if (!active) {
        await emit({
          step: "verify",
          label: STEP_LABELS.verify,
          status: "failed",
          command: planned.command,
          exitCode: svc.code,
          stderr: svc.stderr || svc.stdout
        });
        return {
          steps,
          summary: summarize(false, `installed and configured, but the service was not confirmed active${apiDetail}`)
        };
      }
      await emit({
        step: "verify",
        label: STEP_LABELS.verify,
        status: "ok",
        command: planned.command,
        detail: `service active${apiDetail}`
      });
    }

    return {
      steps,
      summary: summarize(
        true,
        "onboarded: installed, validated, and running as a full failover node (mutation on, API on)"
      )
    };
  } catch (err) {
    await emit({
      step: reachedStep,
      label: STEP_LABELS[reachedStep],
      status: "failed",
      detail: `unexpected error: ${redactor.redact(errorMessage(err))}`
    });
    return { steps, summary: summarize(false, `run aborted by error at step ${reachedStep}`) };
  } finally {
    // Drop the ephemeral connection (and the secrets it held) promptly.
    runner?.close();
  }
}

/** The ordered step ids, exported for callers that want the full plan upfront. */
export const onboardSteps = STEP_ORDER;

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}
