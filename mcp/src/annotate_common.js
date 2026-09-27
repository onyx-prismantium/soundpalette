// Shared pieces of the annotator protocol (extension-4 §7.1): JSON-lines framing over
// stdio, request/response schemas, and the JSON-extraction used on model output.
import * as readline from "node:readline";
import { z } from "zod";
export const PROTOCOL_VERSION = 1;
export const HelloSchema = z.object({ v: z.number(), hello: z.literal(true) });
export const DescribeRequestSchema = z.object({
    v: z.number(),
    id: z.string(),
    stage: z.literal("describe"),
    path: z.string(),
    sha256: z.string().optional(),
    audio: z.object({
        path: z.string(),
        sample_rate: z.number(),
        duration_s: z.number(),
        truncated: z.boolean().optional(),
    }),
    analysis: z
        .object({
        lufs_i: z.number().optional(),
        true_peak_db: z.number().optional(),
        silent: z.boolean().optional(),
        duration_s: z.number().optional(),
        describe: z.string().optional(),
        words: z.array(z.string()).optional(),
        psycho: z.record(z.number()).optional(),
    })
        .optional(),
    categories: z.array(z.object({ name: z.string(), sub_categories: z.array(z.string()).optional() })),
});
export const ChooseRequestSchema = z.object({
    v: z.number(),
    id: z.string(),
    stage: z.literal("choose"),
    path: z.string(),
    sha256: z.string().optional(),
    description: z.string(),
    fx_name: z.string().optional(),
    keywords: z.array(z.string()).optional(),
    candidates: z.array(z.object({
        cat_id: z.string(),
        category: z.string(),
        sub_category: z.string(),
        explanation: z.string().optional(),
    })),
});
/** What the model must return for stage 1 (validated before it reaches the core). */
export const DescribeAnswerSchema = z.object({
    description: z.string().min(1),
    fx_name: z.string().min(1),
    category: z.string().min(1),
    keywords: z.array(z.string()).min(1).max(12),
    confidence: z.number().min(0).max(1).optional().default(0.5),
});
export const ChooseAnswerSchema = z.object({
    cat_id: z.string().min(1),
    confidence: z.number().min(0).max(1).optional().default(0.5),
});
/**
 * Pulls the first JSON object out of model text: tolerates ```json fences, leading prose and
 * trailing chatter. Returns null when nothing parses.
 */
export function extractJson(text) {
    const fenced = text.match(/```(?:json)?\s*([\s\S]*?)```/i);
    const candidates = [fenced ? fenced[1] : "", text];
    for (const c of candidates) {
        if (!c)
            continue;
        const start = c.indexOf("{");
        if (start < 0)
            continue;
        // walk to the matching brace, respecting strings
        let depth = 0;
        let inStr = false;
        let esc = false;
        for (let i = start; i < c.length; i++) {
            const ch = c[i];
            if (inStr) {
                if (esc)
                    esc = false;
                else if (ch === "\\")
                    esc = true;
                else if (ch === '"')
                    inStr = false;
                continue;
            }
            if (ch === '"')
                inStr = true;
            else if (ch === "{")
                depth++;
            else if (ch === "}") {
                depth--;
                if (depth === 0) {
                    try {
                        return JSON.parse(c.slice(start, i + 1));
                    }
                    catch {
                        break;
                    }
                }
            }
        }
    }
    return null;
}
/** Normalizes a stage-1 answer: trims, lowercases keywords, caps lengths. */
export function normalizeDescribe(a) {
    const kw = Array.from(new Set(a.keywords.map((k) => k.trim().toLowerCase()).filter((k) => k.length > 0))).slice(0, 10);
    return {
        description: a.description.trim().replace(/\s+/g, " "),
        fx_name: a.fx_name.trim().replace(/\s+/g, " ").split(" ").slice(0, 6).join(" "),
        category: a.category.trim().toUpperCase(),
        keywords: kw,
        confidence: a.confidence ?? 0.5,
    };
}
/**
 * Runs the JSON-lines loop: one request object per stdin line, one response object per stdout
 * line, out-of-order allowed, `concurrency` requests in flight. Everything diagnostic goes to
 * stderr; stdout carries protocol lines only.
 */
export async function serve(handler, concurrency) {
    const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });
    const out = (obj) => {
        process.stdout.write(JSON.stringify(obj) + "\n");
    };
    let active = 0;
    const queue = [];
    let closed = false;
    let resolveDone = () => { };
    const done = new Promise((r) => (resolveDone = r));
    const handleLine = async (line) => {
        let msg;
        try {
            msg = JSON.parse(line);
        }
        catch {
            process.stderr.write(`annotate: ignoring non-JSON line\n`);
            return;
        }
        if (HelloSchema.safeParse(msg).success) {
            out(handler.hello());
            return;
        }
        const id = msg?.id;
        const idStr = typeof id === "string" ? id : String(id ?? "");
        const stage = msg?.stage;
        try {
            if (stage === "describe") {
                const req = DescribeRequestSchema.parse(msg);
                out({ v: PROTOCOL_VERSION, id: req.id, ...(await handler.describe(req)) });
            }
            else if (stage === "choose") {
                const req = ChooseRequestSchema.parse(msg);
                out({ v: PROTOCOL_VERSION, id: req.id, ...(await handler.choose(req)) });
            }
            else {
                out({ v: PROTOCOL_VERSION, id: idStr, error: `unknown stage ${String(stage)}` });
            }
        }
        catch (e) {
            out({ v: PROTOCOL_VERSION, id: idStr, error: e instanceof Error ? e.message : String(e) });
        }
    };
    const pump = () => {
        while (active < concurrency && queue.length > 0) {
            const line = queue.shift();
            active++;
            void handleLine(line).finally(() => {
                active--;
                if (closed && active === 0 && queue.length === 0)
                    resolveDone();
                else
                    pump();
            });
        }
        if (closed && active === 0 && queue.length === 0)
            resolveDone();
    };
    rl.on("line", (line) => {
        if (line.trim().length === 0)
            return;
        queue.push(line);
        pump();
    });
    rl.on("close", () => {
        closed = true;
        pump();
    });
    await done;
}
