// Spawn helpers for the soundpalette CLI, shared by every tool (extension §5.3).
// Rules: execFile with argument arrays only (never a shell string), and every path parameter
// must resolve inside --root (symlinks resolved first). No tool writes outside root.

import { execFile } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

export class RootEscapeError extends Error {
  constructor(p: string) {
    super(`path escapes --root: ${p}`);
  }
}

function assertInside(realRoot: string, candidateReal: string, original: string): void {
  if (candidateReal !== realRoot && !candidateReal.startsWith(realRoot + path.sep)) {
    throw new RootEscapeError(original);
  }
}

/** Resolves a path parameter that must already exist, confined to root (symlinks resolved). */
export function resolveExistingInRoot(root: string, p: string): string {
  const realRoot = fs.realpathSync(root);
  const candidate = path.resolve(realRoot, p);
  let real: string;
  try {
    real = fs.realpathSync(candidate);
  } catch {
    throw new RootEscapeError(p); // nonexistent counts as unusable input, same error surface
  }
  assertInside(realRoot, real, p);
  return real;
}

/** Resolves an output path: its parent must exist inside root; the file itself may be new. */
export function resolveOutputInRoot(root: string, p: string): string {
  const realRoot = fs.realpathSync(root);
  const candidate = path.resolve(realRoot, p);
  const realDir = fs.realpathSync(path.dirname(candidate)); // throws if the parent is missing
  assertInside(realRoot, realDir, p);
  return path.join(realDir, path.basename(candidate));
}

export interface CliResult {
  stdout: string;
  stderr: string;
  code: number;
}

/** Runs the soundpalette CLI; exit codes are returned, not thrown (lint uses 1 for outliers). */
export function runCli(bin: string, args: string[]): Promise<CliResult> {
  return new Promise((resolve, reject) => {
    execFile(
      bin,
      args,
      { maxBuffer: 64 * 1024 * 1024, windowsHide: true },
      (error, stdout, stderr) => {
        if (error && typeof error.code !== "number") {
          reject(error); // spawn failure (missing binary), not a CLI exit code
          return;
        }
        resolve({ stdout, stderr, code: error ? (error.code as number) : 0 });
      },
    );
  });
}
