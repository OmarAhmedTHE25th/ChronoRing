"use strict";

const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

function normalizeUnc(p) {
  return p.replace(/[\\/]+$/, "");
}

function findMappedDrivePath(uncRoot) {
  const normalizedRoot = normalizeUnc(uncRoot);

  // 1. Check `net use` mappings (e.g. Z: -> \\wsl.localhost\Ubuntu)
  const netUse = spawnSync("net", ["use"], { encoding: "utf8", windowsHide: true });
  if (netUse.status === 0 && netUse.stdout) {
    for (const line of netUse.stdout.split(/\r?\n/)) {
      const m = line.match(/\b([A-Za-z]:)\s+(\\\\[^\s]+)/);
      if (!m) continue;
      const drive = m[1].toUpperCase();
      const remote = normalizeUnc(m[2]);
      if (normalizedRoot.toLowerCase() === remote.toLowerCase()) {
        return `${drive}\\`;
      }
      if (normalizedRoot.toLowerCase().startsWith(`${remote.toLowerCase()}\\`)) {
        const suffix = normalizedRoot.slice(remote.length + 1);
        const candidate = `${drive}\\${suffix}`;
        if (fs.existsSync(path.join(candidate, "package.json"))) {
          return candidate;
        }
      }
    }
  }

  // 2. Fallback check for WSL default Z: drive if \\wsl.localhost\<distro>\... is mapped outside `net use`
  const wslMatch = normalizedRoot.match(/^\\\\wsl(?:\.localhost|\$)\\[^\\]+\\(.+)$/i);
  if (wslMatch) {
    const candidate = `Z:\\${wslMatch[1]}`;
    if (fs.existsSync(path.join(candidate, "package.json"))) {
      return candidate;
    }
  }

  return null;
}

function quoteCmdArg(arg) {
  if (/[\s"&|<>^()%!]/.test(arg)) {
    return `"${arg.replace(/"/g, '\\"')}"`;
  }
  return arg;
}

const pkgJsonPath = process.env.npm_package_json;
const rawRoot = pkgJsonPath ? path.dirname(pkgJsonPath) : path.resolve(__dirname, "..");
const [subcommand = "dev", ...extraArgs] = process.argv.slice(1);

let effectiveRoot = rawRoot;
let usePushdFallback = false;

if (process.platform === "win32" && rawRoot.startsWith("\\\\")) {
  const mapped = findMappedDrivePath(rawRoot);
  if (mapped) {
    effectiveRoot = mapped;
  } else {
    usePushdFallback = true;
  }
}

if (usePushdFallback) {
  // No pre-mapped drive found: `pushd` creates a temporary drive letter on Windows so Vite avoids UNC paths.
  const cliArgs = [
    process.execPath,
    path.join("node_modules", "astro", "astro.js"),
    subcommand,
    "--root",
    ".",
    ...extraArgs,
  ].map(quoteCmdArg);
  const cmdLine = `pushd "${normalizeUnc(rawRoot)}" && ${cliArgs.join(" ")}`;
  const res = spawnSync(
    process.env.ComSpec || "cmd.exe",
    ["/d", "/v:on", "/c", `${cmdLine} & set rc=!errorlevel! & popd & exit /b !rc!`],
    {
      stdio: "inherit",
      env: { ...process.env },
    }
  );
  process.exit(res.status === null ? 1 : res.status);
}

const astroCli = path.join(effectiveRoot, "node_modules", "astro", "astro.js");
process.chdir(effectiveRoot);

const result = spawnSync(
  process.execPath,
  [astroCli, subcommand, "--root", effectiveRoot, ...extraArgs],
  {
    cwd: effectiveRoot,
    stdio: "inherit",
  }
);

if (result.error) {
  console.error(result.error.message);
  process.exit(1);
}

process.exit(result.status === null ? 1 : result.status);
