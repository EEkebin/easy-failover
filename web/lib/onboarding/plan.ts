// Pure per-step command planning for SSH onboarding.
//
// This module contains NO ssh2 and NO secrets in argv. It builds the shell
// command strings each onboarding step runs, and parses the output of detection
// commands. Keeping this logic pure makes the orchestrator's decision-making
// unit-testable with a fake runner and no network.
//
// The sudo password (when `sudoMethod = password`) is NEVER placed in argv: the
// privileged-command builder emits `sudo -S -p ''` and the orchestrator feeds
// the password on stdin. Other secrets never appear in any command here.

import type { InstallSource, SudoMethod, TargetFacts } from "./types";

/** A planned command plus whether the sudo password must be fed on its stdin. */
export type PlannedCommand = {
  command: string;
  /** True when the command uses `sudo -S` and needs the sudo password on stdin. */
  needsSudoPassword: boolean;
};

/** POSIX single-quote a value for safe embedding in a shell command. */
export function shellQuote(value: string): string {
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

/**
 * Wrap a command so it runs with root privileges per the chosen sudo method.
 * - `already-root`: run as-is (the login shell is already root).
 * - `passwordless`: prefix `sudo`.
 * - `password`: prefix `sudo -S -p ''` and require the password on stdin.
 *
 * For the two sudo cases the body is wrapped in `sh -c '<body>'` so the ENTIRE
 * body — including `&&`/`;`/pipe chains — runs as root. Without this,
 * `sudo cmd1 && cmd2` would run only `cmd1` as root and `cmd2` as the login
 * user. (`already-root` needs no wrap: the login shell runs the whole chain as
 * root.) When the password is fed on stdin, `sudo -S` consumes the first line
 * and the wrapped `sh -c` still sees the remaining stdin (e.g. config content).
 */
export function privileged(command: string, sudo: SudoMethod): PlannedCommand {
  switch (sudo.kind) {
    case "already-root":
      return { command, needsSudoPassword: false };
    case "passwordless":
      return { command: `sudo sh -c ${shellQuote(command)}`, needsSudoPassword: false };
    case "password":
      // `-S` reads the password from stdin; `-p ''` suppresses the prompt so it
      // does not pollute stderr. The password itself is supplied out-of-band.
      return { command: `sudo -S -p '' sh -c ${shellQuote(command)}`, needsSudoPassword: true };
  }
}

/** Command that reads /etc/os-release and basic system facts in one shot. */
export function detectFactsCommand(): string {
  // Emit clearly-delimited sections so parseDetectOutput can split them.
  return [
    "echo '=== os-release ==='",
    "cat /etc/os-release 2>/dev/null || true",
    "echo '=== arch ==='",
    "uname -m 2>/dev/null || true",
    "echo '=== init ==='",
    // Detection hints for each supported supervisor.
    "( [ -d /run/systemd/system ] && echo systemd ) || true",
    "command -v systemctl >/dev/null 2>&1 && echo has-systemctl || true",
    "command -v rc-status >/dev/null 2>&1 && echo has-rc-status || true",
    "[ -d /etc/init.d ] && echo has-initd || true",
    "command -v sv >/dev/null 2>&1 && echo has-sv || true",
    "[ -d /etc/runit ] || [ -d /etc/sv ] && echo has-runit || true",
    "command -v dinitctl >/dev/null 2>&1 && echo has-dinit || true",
    "command -v s6-rc >/dev/null 2>&1 && echo has-s6 || true",
    "[ -d /etc/s6 ] || [ -d /run/service ] && echo has-s6dir || true",
    "echo '=== pkg ==='",
    "command -v apt-get >/dev/null 2>&1 && echo apt || true",
    "command -v dnf >/dev/null 2>&1 && echo dnf || true",
    "command -v pacman >/dev/null 2>&1 && echo pacman || true",
    "echo '=== tools ==='",
    // Probe for prerequisites; print "have:<tool>" for each present one.
    'for t in tar sha256sum curl wget git cmake ninja make g++ ip arping; do command -v "$t" >/dev/null 2>&1 && echo "have:$t" || echo "miss:$t"; done'
  ].join("; ");
}

/** Parse the structured output of {@link detectFactsCommand}. */
export function parseDetectOutput(output: string): TargetFacts {
  const facts: TargetFacts = {};
  const sections = output.split(/=== (os-release|arch|init|pkg|tools) ===/);
  // sections: ["", "os-release", <body>, "arch", <body>, ...]
  const map = new Map<string, string>();
  for (let i = 1; i < sections.length; i += 2) {
    map.set(sections[i], (sections[i + 1] ?? "").trim());
  }

  const osRelease = map.get("os-release") ?? "";
  const idMatch = osRelease.match(/^ID=("?)([^"\n]*)\1/m);
  if (idMatch) {
    facts.distroId = idMatch[2];
  }
  const verMatch = osRelease.match(/^VERSION_ID=("?)([^"\n]*)\1/m);
  if (verMatch) {
    facts.distroVersion = verMatch[2];
  }

  const arch = (map.get("arch") ?? "").trim();
  if (arch) {
    facts.arch = arch;
  }

  const init = map.get("init") ?? "";
  if (/\bsystemd\b/.test(init) || /has-systemctl/.test(init)) {
    facts.initSystem = "systemd";
  } else if (/has-rc-status/.test(init)) {
    facts.initSystem = "openrc";
  } else if (/has-dinit/.test(init)) {
    facts.initSystem = "dinit";
  } else if (/has-s6\b/.test(init) || /has-s6dir/.test(init)) {
    facts.initSystem = "s6";
  } else if (/has-sv/.test(init) || /has-runit/.test(init)) {
    facts.initSystem = "runit";
  } else {
    facts.initSystem = "unknown";
  }

  const pkg = map.get("pkg") ?? "";
  if (/\bapt\b/.test(pkg)) {
    facts.packageManager = "apt";
  } else if (/\bdnf\b/.test(pkg)) {
    facts.packageManager = "dnf";
  } else if (/\bpacman\b/.test(pkg)) {
    facts.packageManager = "pacman";
  } else {
    facts.packageManager = "unknown";
  }

  const tools = map.get("tools") ?? "";
  const present: string[] = [];
  const missing: string[] = [];
  for (const line of tools.split(/\r?\n/)) {
    const have = line.match(/^have:(.+)$/);
    const miss = line.match(/^miss:(.+)$/);
    if (have) {
      present.push(have[1]);
    } else if (miss) {
      missing.push(miss[1]);
    }
  }
  facts.presentPrereqs = present;
  facts.missingPrereqs = missing;

  return facts;
}

/** The prerequisite tools required for each install source. */
export function requiredPrereqs(source: InstallSource): string[] {
  if (source.kind === "release-tarball") {
    // Download + verify + unpack; a fetcher (curl or wget) is also needed but is
    // resolved at fetch time. `ip`/`arping` are needed only when the node will
    // later move a real VIP, but installing iproute2 is cheap and harmless.
    return ["tar", "sha256sum"];
  }
  // build-from-source needs the full toolchain.
  return ["git", "cmake", "make", "g++", "tar", "sha256sum"];
}

/** Map a probe tool name to the package name for a given package manager. */
export function packageFor(tool: string, pm: TargetFacts["packageManager"]): string {
  // Most tools share names; a few differ per distro family.
  const overrides: Record<string, Partial<Record<NonNullable<TargetFacts["packageManager"]>, string>>> = {
    "g++": { apt: "g++", dnf: "gcc-c++", pacman: "gcc" },
    sha256sum: { apt: "coreutils", dnf: "coreutils", pacman: "coreutils" },
    ip: { apt: "iproute2", dnf: "iproute", pacman: "iproute2" },
    arping: { apt: "iputils-arping", dnf: "iputils", pacman: "iputils" },
    make: { apt: "make", dnf: "make", pacman: "make" }
  };
  const pmKey = pm && pm !== "unknown" ? pm : undefined;
  if (pmKey && overrides[tool]?.[pmKey]) {
    return overrides[tool][pmKey] as string;
  }
  return tool;
}

/** Build the package-manager install command for a set of packages. */
export function installPackagesCommand(
  packages: string[],
  pm: TargetFacts["packageManager"]
): string {
  const quoted = packages.map(shellQuote).join(" ");
  switch (pm) {
    case "apt":
      return `DEBIAN_FRONTEND=noninteractive apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y ${quoted}`;
    case "dnf":
      return `dnf install -y ${quoted}`;
    case "pacman":
      return `pacman -Sy --noconfirm ${quoted}`;
    default:
      // No known package manager; emit a command that fails clearly.
      return `sh -c 'echo "no supported package manager detected" >&2; exit 1'`;
  }
}

/** The release tarball + checksum filenames and URLs for a release ref. */
export function releaseArtifacts(releaseRef: string): {
  tarball: string;
  checksum: string;
  baseUrl: string;
} {
  const tarball = "easy-failover-linux-x86_64.tar.gz";
  const checksum = `${tarball}.sha256`;
  const repo = "EEkebin/easy-failover";
  const refPath = releaseRef === "latest" ? "latest/download" : `download/${shellQuoteRef(releaseRef)}`;
  const baseUrl = `https://github.com/${repo}/releases/${refPath}`;
  return { tarball, checksum, baseUrl };
}

/** Restrict a release ref to safe URL-path characters (defense in depth). */
function shellQuoteRef(ref: string): string {
  // Release tags are like `v1.2.3`; reject anything outside a conservative set.
  if (!/^[A-Za-z0-9._-]+$/.test(ref)) {
    throw new Error(`invalid release ref: ${ref}`);
  }
  return ref;
}

/**
 * Build the fetch/verify/install command for the release-tarball source.
 * Runs entirely in a temp dir, fetches the tarball and its `.sha256`, runs
 * `sha256sum -c` (mandatory), optionally cross-checks an expected hash, unpacks,
 * and installs into prefix/sysconfdir. Returns the unprivileged command body to
 * be wrapped by `privileged()`.
 */
export function releaseInstallCommand(
  source: Extract<InstallSource, { kind: "release-tarball" }>,
  prefix: string,
  sysconfdir: string
): string {
  const { tarball, checksum, baseUrl } = releaseArtifacts(source.releaseRef);
  const expectedCheck = source.sha256
    ? `printf '%s  %s\\n' ${shellQuote(source.sha256)} ${shellQuote(tarball)} | sha256sum -c - || exit 1; `
    : "";
  // Use a dedicated temp dir; prefer curl, fall back to wget.
  return [
    "set -e",
    'TMP="$(mktemp -d)"',
    'cd "$TMP"',
    `fetch() { if command -v curl >/dev/null 2>&1; then curl -fsSL -o "$2" "$1"; else wget -qO "$2" "$1"; fi; }`,
    `fetch ${shellQuote(`${baseUrl}/${tarball}`)} ${shellQuote(tarball)}`,
    `fetch ${shellQuote(`${baseUrl}/${checksum}`)} ${shellQuote(checksum)}`,
    `sha256sum -c ${shellQuote(checksum)}`,
    expectedCheck.trim(),
    `tar -xzf ${shellQuote(tarball)}`,
    // Install tree layout: copy into prefix; config root under sysconfdir.
    `mkdir -p ${shellQuote(prefix)} ${shellQuote(`${sysconfdir}/easy-failover`)}`,
    // The tarball is a staged install tree rooted at the prefix; replicate it.
    `cp -a ./usr/. ${shellQuote(prefix)}/ 2>/dev/null || cp -a . ${shellQuote(prefix)}/`,
    'rm -rf "$TMP"'
  ]
    .filter((line) => line.length > 0)
    .join("; ");
}

/**
 * Build the clone + CMake build/install command for build-from-source. Returns
 * the unprivileged body; the orchestrator runs it via privileged() so the
 * `cmake --install` step can write under the prefix and sysconfdir. The install
 * stages only the example config (no active config.toml) — the onboarding flow
 * writes the active config separately in the write-config step.
 */
export function sourceInstallCommand(
  source: Extract<InstallSource, { kind: "build-from-source" }>,
  prefix: string,
  sysconfdir: string
): string {
  const repo = "https://github.com/EEkebin/easy-failover.git";
  const ref = source.gitRef;
  const refArg = ref ? `--branch ${shellQuote(ref)}` : "";
  return [
    "set -e",
    'TMP="$(mktemp -d)"',
    `git clone --depth 1 ${refArg} ${shellQuote(repo)} "$TMP/src"`,
    'cd "$TMP/src"',
    `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${shellQuote(prefix)} -DCMAKE_INSTALL_SYSCONFDIR=${shellQuote(sysconfdir)} -DBUILD_TESTING=OFF`,
    "cmake --build build --parallel",
    "cmake --install build",
    'rm -rf "$TMP"'
  ]
    .filter((line) => line.length > 0)
    .join("; ");
}

/** Path where the active config is written on the target. */
export function configPath(sysconfdir: string): string {
  return `${sysconfdir.replace(/\/$/, "")}/easy-failover/config.toml`;
}

/**
 * Build the command that writes the rendered config to the target with 0644,
 * creating the directory 0755. The config content is delivered on stdin via a
 * heredoc-free `cat` so the (non-secret) TOML never needs shell escaping in argv.
 * Returns the unprivileged body to be wrapped by `privileged()`; the config text
 * is supplied separately on stdin by the orchestrator.
 */
export function writeConfigCommand(sysconfdir: string): string {
  const dir = `${sysconfdir.replace(/\/$/, "")}/easy-failover`;
  const path = configPath(sysconfdir);
  return [
    `install -d -m 0755 ${shellQuote(dir)}`,
    `cat > ${shellQuote(path)}`,
    `chmod 0644 ${shellQuote(path)}`
  ].join(" && ");
}

/** Command that reads an existing config (to compare against generated). */
export function readConfigCommand(sysconfdir: string): string {
  const path = configPath(sysconfdir);
  return `cat ${shellQuote(path)} 2>/dev/null || true`;
}

/** Build the on-target validation command. */
export function validateConfigCommand(prefix: string, sysconfdir: string): string {
  const bin = `${prefix.replace(/\/$/, "")}/bin/easy-failover`;
  const path = configPath(sysconfdir);
  // Prefer the installed binary; fall back to PATH if the prefix bin is absent.
  return `{ ${shellQuote(bin)} --config ${shellQuote(path)} --validate-config 2>&1 || easy-failover --config ${shellQuote(path)} --validate-config 2>&1; }`;
}

/** Build the enable+start command for the detected init system. */
export function enableStartCommand(initSystem: TargetFacts["initSystem"]): string {
  const unit = "easy-failover";
  switch (initSystem) {
    case "systemd":
      return `systemctl daemon-reload && systemctl enable --now ${unit}.service`;
    case "openrc":
      return `rc-update add ${unit} default && rc-service ${unit} start`;
    case "runit":
      return `ln -sf /etc/sv/${unit} /var/service/${unit} 2>/dev/null || ln -sf /etc/sv/${unit} /etc/service/${unit}; sv up ${unit} || true`;
    case "dinit":
      return `dinitctl enable ${unit} && dinitctl start ${unit}`;
    case "s6":
      return `s6-rc-bundle-update add default ${unit} 2>/dev/null; s6-rc -u change ${unit} || true`;
    default:
      return `sh -c 'echo "no supported init system detected" >&2; exit 1'`;
  }
}

/** Build the command that checks whether the service is active. */
export function verifyServiceCommand(initSystem: TargetFacts["initSystem"]): string {
  const unit = "easy-failover";
  switch (initSystem) {
    case "systemd":
      return `systemctl is-active ${unit}.service`;
    case "openrc":
      return `rc-service ${unit} status`;
    case "runit":
      return `sv status ${unit}`;
    case "dinit":
      return `dinitctl status ${unit}`;
    case "s6":
      return `s6-rc -a list | grep -qx ${unit} && echo active`;
    default:
      return `sh -c 'echo unknown; exit 1'`;
  }
}

/** Build the API poll command (read-only status snapshot) if api is enabled. */
export function verifyApiCommand(apiBind: string): string {
  // apiBind is like `127.0.0.1:8743`; query the status endpoint locally.
  const url = `http://${apiBind}/api/v1/status`;
  return `{ curl -fsS ${shellQuote(url)} || wget -qO- ${shellQuote(url)}; }`;
}
