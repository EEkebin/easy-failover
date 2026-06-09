// easy-failover Cockpit plugin logic.
//
// Cockpit injects ../base1/cockpit.js (the `cockpit` global) and handles auth/TLS.
// We read the daemon's local API for status over loopback, and use Cockpit's
// privileged file + spawn channels to apply config and restart the daemon — so this
// page needs no bearer token of its own.

(function () {
    "use strict";

    const CONFIG_PATH = "/etc/easy-failover/config.toml";
    const SERVICE = "easy-failover.service";
    const API = { address: "127.0.0.1", port: 8743 };

    const $ = (id) => document.getElementById(id);

    function banner(msg, kind) {
        const el = $("banner");
        if (!msg) {
            el.hidden = true;
            el.textContent = "";
            el.className = "ef-banner";
            return;
        }
        el.hidden = false;
        el.textContent = msg;
        el.className = "ef-banner " + (kind === "ok" ? "ef-ok" : "ef-error");
    }

    function pill(text, good) {
        const cls = good === undefined ? "" : good ? " ef-good" : " ef-bad";
        return '<span class="ef-pill' + cls + '">' + escapeHtml(text) + "</span>";
    }

    function escapeHtml(s) {
        return String(s).replace(/[&<>"']/g, (c) => ({
            "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
        }[c]));
    }

    // ---- Status ---------------------------------------------------------------

    function http() {
        return cockpit.http(API);
    }

    function renderStatus(status) {
        const node = status.node || {};
        const vip = status.vip || {};
        const health = status.health || {};
        const lifecycle = status.lifecycle || {};

        $("s-node").textContent = node.id || "—";
        $("s-role").innerHTML = pill(node.state || "unknown");
        $("s-health").innerHTML = pill(
            health.status || (node.healthy ? "healthy" : "unknown"),
            (health.status ? health.status === "healthy" : node.healthy)
        );
        const vipText = vip.address
            ? vip.address + (vip.interface ? " on " + vip.interface : "")
            : "unconfigured";
        $("s-vip").textContent = vipText;
        $("s-owner").innerHTML = pill(vip.local_owner ? "yes" : "no", !!vip.local_owner);
        $("s-detail").textContent = lifecycle.detail || "—";
    }

    function loadStatus() {
        $("s-source").textContent = "";
        http().get("/api/v1/status")
            .then((body) => {
                renderStatus(JSON.parse(body));
                banner(null);
            })
            .catch((err) => {
                $("s-source").textContent =
                    "Could not reach the local API at 127.0.0.1:8743 (" +
                    (err && err.message ? err.message : "unreachable") +
                    "). Is easy-failover running with [api].enabled = true?";
            });
    }

    function prefillVip() {
        http().get("/api/v1/config")
            .then((body) => {
                const cfg = JSON.parse(body);
                const vip = cfg.vip || {};
                if (!$("vip-address").value) $("vip-address").value = vip.address || "";
                if (!$("vip-interface").value) $("vip-interface").value = vip.interface || "";
            })
            .catch(() => { /* status banner already covers unreachable API */ });
    }

    // ---- TOML editing ---------------------------------------------------------

    // Set vip.address / vip.interface inside the [vip] table of an existing TOML
    // document, preserving everything else. Targeted line edits (not a full
    // serializer) so operator comments/sections are untouched.
    function setVipInToml(toml, address, iface) {
        const lines = toml.split("\n");
        const out = [];
        let inVip = false;
        let wroteAddress = false;
        let wroteInterface = false;

        const addrLine = "address = " + JSON.stringify(address);
        const ifaceLine = "interface = " + JSON.stringify(iface);

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const header = line.match(/^\s*\[([^\]]+)\]\s*$/);
            if (header) {
                // Leaving the [vip] table: backfill any keys we didn't see.
                if (inVip) {
                    if (!wroteAddress) { out.push(addrLine); wroteAddress = true; }
                    if (!wroteInterface) { out.push(ifaceLine); wroteInterface = true; }
                }
                inVip = header[1].trim() === "vip";
                out.push(line);
                continue;
            }
            if (inVip && /^\s*address\s*=/.test(line)) {
                out.push(addrLine);
                wroteAddress = true;
                continue;
            }
            if (inVip && /^\s*interface\s*=/.test(line)) {
                out.push(ifaceLine);
                wroteInterface = true;
                continue;
            }
            out.push(line);
        }

        // Document ended while still in [vip], or there was no [vip] table at all.
        if (inVip) {
            if (!wroteAddress) { out.push(addrLine); wroteAddress = true; }
            if (!wroteInterface) { out.push(ifaceLine); wroteInterface = true; }
        }
        if (!wroteAddress || !wroteInterface) {
            out.push("");
            out.push("[vip]");
            out.push(addrLine);
            out.push(ifaceLine);
        }
        return out.join("\n");
    }

    // ---- Apply ----------------------------------------------------------------

    function validate(candidate) {
        return http().request({
            method: "POST",
            path: "/api/v1/config/validate",
            body: JSON.stringify({ format: "toml", config: candidate }),
            headers: { "Content-Type": "application/json" }
        }).then((body) => JSON.parse(body));
    }

    function applyVip(ev) {
        ev.preventDefault();
        const address = $("vip-address").value.trim();
        const iface = $("vip-interface").value.trim();
        const status = $("vip-status");
        const button = $("vip-apply");

        if ((address === "") !== (iface === "")) {
            banner("Set both address and interface, or leave both blank.", "error");
            return;
        }

        button.disabled = true;
        status.textContent = "Reading config…";
        banner(null);

        const file = cockpit.file(CONFIG_PATH, { superuser: "require" });
        file.read()
            .then((content) => {
                const current = content || "";
                const candidate = setVipInToml(current, address, iface);
                status.textContent = "Validating…";
                return validate(candidate).then((result) => {
                    if (!result || result.valid !== true) {
                        const errs = (result && result.errors) || ["unknown validation error"];
                        throw new Error("Config rejected: " + errs.join("; "));
                    }
                    status.textContent = "Writing config…";
                    return file.replace(candidate);
                });
            })
            .then(() => {
                status.textContent = "Restarting daemon…";
                return cockpit.spawn(["systemctl", "restart", SERVICE],
                    { superuser: "require", err: "message" });
            })
            .then(() => {
                banner("Applied. Daemon restarted with the new VIP.", "ok");
                status.textContent = "";
                window.setTimeout(loadStatus, 1500);
            })
            .catch((err) => {
                banner(err && err.message ? err.message : String(err), "error");
                status.textContent = "";
            })
            .finally(() => {
                button.disabled = false;
                file.close();
            });
    }

    // ---- Init -----------------------------------------------------------------

    function init() {
        $("vip-form").addEventListener("submit", applyVip);
        loadStatus();
        prefillVip();
        window.setInterval(loadStatus, 5000);
        cockpit.translate();
    }

    document.addEventListener("DOMContentLoaded", init);
})();
