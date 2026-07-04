// soundpalette-mcp: stdio MCP server exposing the SoundPalette analysis suite (extension §5.3).
// Thin wrapper: every tool shells out to the soundpalette CLI and returns its parsed --json
// output as structuredContent plus a short text rendering.
//
// Usage: node mcp/dist/server.js --root <project_dir> [--bin <path-to-soundpalette>]

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { Resvg } from "@resvg/resvg-js";
import { z } from "zod";

import { resolveExistingInRoot, resolveOutputInRoot, runCli, RootEscapeError } from "./cli.js";

function parseArgs(argv: string[]): { root: string; bin: string } {
  let root = "";
  let bin = "";
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--root" && i + 1 < argv.length) root = argv[++i];
    else if (argv[i] === "--bin" && i + 1 < argv.length) bin = argv[++i];
  }
  if (!root) {
    console.error("usage: server.js --root <project_dir> [--bin <path-to-soundpalette>]");
    process.exit(2);
  }
  root = fs.realpathSync(root);
  if (!bin) {
    // Default: build/cli/soundpalette relative to this repo checkout (extension §5.3).
    const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
    bin = path.join(repo, "build", "cli", "soundpalette");
    if (process.platform === "win32") bin += ".exe";
  }
  return { root, bin };
}

const { root: ROOT, bin: BIN } = parseArgs(process.argv.slice(2));

type ToolResult = {
  content: ({ type: "text"; text: string } | { type: "image"; data: string; mimeType: string })[];
  structuredContent?: Record<string, unknown>;
  isError?: boolean;
};

function toolError(message: string): ToolResult {
  return { content: [{ type: "text", text: `error: ${message}` }], isError: true };
}

/** Wraps a tool body so path-confinement and CLI failures surface as MCP tool errors. */
function guarded<A extends unknown[]>(fn: (...args: A) => Promise<ToolResult>) {
  return async (...args: A): Promise<ToolResult> => {
    try {
      return await fn(...args);
    } catch (e) {
      if (e instanceof RootEscapeError) return toolError(e.message);
      return toolError(e instanceof Error ? e.message : String(e));
    }
  };
}

const server = new McpServer({ name: "soundpalette-mcp", version: "0.2.0" });

server.registerTool(
  "scan_folder",
  {
    description:
      "Analyze every audio file under a folder (recursive) and return palette statistics. " +
      "Set include_files for the full per-file feature/visual list; out_path writes the " +
      "manifest JSON inside the project root.",
    inputSchema: {
      dir: z.string().describe("folder to scan, relative to the server root"),
      include_files: z.boolean().default(false),
      out_path: z.string().optional().describe("optional manifest output path inside root"),
    },
  },
  guarded(async ({ dir, include_files, out_path }) => {
    const dirAbs = resolveExistingInRoot(ROOT, dir);
    const res = await runCli(BIN, ["scan", dirAbs, "--quiet"]);
    if (res.code !== 0) return toolError(res.stderr.trim() || `scan exited ${res.code}`);
    const manifest = JSON.parse(res.stdout);
    if (out_path !== undefined) {
      fs.writeFileSync(resolveOutputInRoot(ROOT, out_path), res.stdout);
    }
    const structured: Record<string, unknown> = {
      file_count: manifest.file_count,
      stats: manifest.stats,
    };
    if (include_files) structured.files = manifest.files;
    if (out_path !== undefined) structured.manifest_path = out_path;
    return {
      content: [
        {
          type: "text",
          text: `scanned ${manifest.file_count} files under ${dir}` +
            (out_path !== undefined ? `; manifest written to ${out_path}` : ""),
        },
      ],
      structuredContent: structured,
    };
  }),
);

server.registerTool(
  "lint_against_baseline",
  {
    description:
      "Scan a folder and flag files that drift off-palette relative to a baseline manifest's " +
      "stats (z-score over the seven mapping dimensions).",
    inputSchema: {
      dir: z.string(),
      baseline_path: z.string(),
      threshold: z.number().default(2.5),
      top: z.number().int().default(10),
    },
  },
  guarded(async ({ dir, baseline_path, threshold, top }) => {
    const dirAbs = resolveExistingInRoot(ROOT, dir);
    const baseAbs = resolveExistingInRoot(ROOT, baseline_path);
    const res = await runCli(BIN, [
      "lint", dirAbs, "--baseline", baseAbs,
      "--threshold", String(threshold), "--top", String(top), "--json",
    ]);
    if (res.code === 2) return toolError(res.stderr.trim() || "lint failed");
    const report = JSON.parse(res.stdout);
    const lines: string[] = report.pass
      ? [`PASS: all files within the palette at threshold ${threshold}`]
      : report.outliers.map(
          (o: { path: string; worst_dim: string; max_z: number; dims_over: string[] }) =>
            `OUTLIER ${o.path} worst=${o.worst_dim} z=${o.max_z.toFixed(2)} dims=${o.dims_over.join(",")}`,
        );
    return { content: [{ type: "text", text: lines.join("\n") }], structuredContent: report };
  }),
);

server.registerTool(
  "describe_sound",
  {
    description:
      "Analyze one audio file and describe it in deterministic plain language, with the full " +
      "feature/visual/dimension breakdown as structured content.",
    inputSchema: { path: z.string() },
  },
  guarded(async ({ path: p }) => {
    const fileAbs = resolveExistingInRoot(ROOT, p);
    const res = await runCli(BIN, ["describe", fileAbs, "--json"]);
    if (res.code !== 0) return toolError(res.stderr.trim() || `describe exited ${res.code}`);
    const detail = JSON.parse(res.stdout);
    return {
      content: [{ type: "text", text: detail.sentence }],
      structuredContent: detail,
    };
  }),
);

server.registerTool(
  "render_palette_sheet",
  {
    description:
      "Render the palette as a glyph-grid image (PNG). Pass either a folder to scan or an " +
      "existing manifest path.",
    inputSchema: {
      dir: z.string().optional(),
      manifest_path: z.string().optional(),
      columns: z.number().int().min(1).max(64).default(8),
    },
  },
  guarded(async ({ dir, manifest_path, columns }) => {
    if ((dir === undefined) === (manifest_path === undefined)) {
      return toolError("provide exactly one of dir or manifest_path");
    }
    // Temp files stay inside root (extension §5.3: no writes outside --root, ever).
    const tmp = fs.mkdtempSync(path.join(ROOT, ".sp-mcp-"));
    try {
      let manifestAbs: string;
      let fileCount = 0;
      if (dir !== undefined) {
        const dirAbs = resolveExistingInRoot(ROOT, dir);
        manifestAbs = path.join(tmp, "palette.json");
        const scan = await runCli(BIN, ["scan", dirAbs, "--out", manifestAbs, "--quiet"]);
        if (scan.code !== 0) return toolError(scan.stderr.trim() || `scan exited ${scan.code}`);
      } else {
        manifestAbs = resolveExistingInRoot(ROOT, manifest_path as string);
      }
      fileCount = JSON.parse(fs.readFileSync(manifestAbs, "utf8")).file_count;
      const svgAbs = path.join(tmp, "sheet.svg");
      const svg = await runCli(BIN, [
        "export-svg", manifestAbs, "--out", svgAbs, "--columns", String(columns),
      ]);
      if (svg.code !== 0) return toolError(svg.stderr.trim() || `export-svg exited ${svg.code}`);
      const png = new Resvg(fs.readFileSync(svgAbs, "utf8")).render().asPng();
      return {
        content: [
          { type: "image", data: Buffer.from(png).toString("base64"), mimeType: "image/png" },
          { type: "text", text: `palette sheet: ${fileCount} sounds, ${columns} columns` },
        ],
      };
    } finally {
      fs.rmSync(tmp, { recursive: true, force: true });
    }
  }),
);

await server.connect(new StdioServerTransport());
