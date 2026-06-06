// Secret redaction for SSH onboarding.
//
// SECURITY: this is the single chokepoint every progress/log/detail string must
// pass through before it leaves the server. The redaction rule (from
// `docs/ssh-onboarding-design.md`) is: given the set of secret values the
// operator supplied — SSH password, private key, key passphrase, sudo password —
// replace every occurrence of each in any string with the literal `***`.
//
// Secrets are still expected to travel out-of-band (the ssh2 auth layer, and
// `sudo -S` reading the sudo password from stdin), never as argv. Redaction is a
// defense-in-depth backstop so that even if a secret were ever to appear in a
// command line, stderr, or error message, it is scrubbed before display.

const PLACEHOLDER = "***";

/**
 * A redactor closes over a fixed set of secrets and scrubs them from any string.
 * Create one per onboarding run from the run's secrets, then route every
 * outbound string through `redact`.
 */
export type Redactor = {
  /** Replace every occurrence of every secret in `value` with `***`. */
  redact: (value: string) => string;
};

/**
 * Collect the secret strings out of an arbitrary value. Empty/blank secrets are
 * ignored (replacing `""` everywhere would corrupt all output and leaks
 * nothing). Order longest-first so that a secret that is a substring of another
 * does not leave a partial fragment behind.
 */
export function collectSecrets(secrets: Array<string | undefined | null>): string[] {
  const seen = new Set<string>();
  for (const secret of secrets) {
    if (typeof secret !== "string") {
      continue;
    }
    // A secret that is only whitespace, or shorter than 1 char, is not redacted:
    // redacting "" or " " would mangle every string and protects nothing.
    if (secret.trim().length === 0) {
      continue;
    }
    seen.add(secret);
  }
  return [...seen].sort((a, b) => b.length - a.length);
}

/** Escape a string for safe use as a literal inside a RegExp. */
function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

/**
 * Build a Redactor from a list of secret values. Pass every ephemeral secret in
 * the run (SSH password, private key, passphrase, sudo password). Undefined and
 * blank entries are ignored.
 */
export function createRedactor(secrets: Array<string | undefined | null>): Redactor {
  const collected = collectSecrets(secrets);

  if (collected.length === 0) {
    return { redact: (value: string) => value };
  }

  const pattern = new RegExp(collected.map(escapeRegExp).join("|"), "g");

  return {
    redact: (value: string): string => {
      if (typeof value !== "string" || value.length === 0) {
        return value;
      }
      return value.replace(pattern, PLACEHOLDER);
    }
  };
}

/**
 * Convenience: collect all secrets from an onboarding request's auth and sudo
 * material. Kept here (rather than importing request types) as a small explicit
 * list so the redactor never depends on request shape evolution.
 */
export function secretsFromRunInputs(inputs: {
  sshPassword?: string;
  privateKey?: string;
  passphrase?: string;
  sudoPassword?: string;
}): string[] {
  return collectSecrets([
    inputs.sshPassword,
    inputs.privateKey,
    inputs.passphrase,
    inputs.sudoPassword
  ]);
}
