# The sound library: UCS tags and model annotations

SoundPalette can index a folder of sound files, classify every file into the
[Universal Category System](https://universalcategorysystem.com) (UCS v8.2.1, 753 categories)
and write a one-line description per file. Two things do the classifying:

1. **Offline, always available**: files already named per UCS
   (`CatID_FXName_CreatorID_SourceID.wav`) are recognized outright; other names are matched
   against the UCS synonym lists and accepted only when one category wins unambiguously.
2. **A model annotator** (optional): an audio-language model listens to a 16 kHz mono excerpt
   of up to 30 seconds and returns a description, keywords, a title and a category. SoundPalette
   then picks the CatID deterministically from the model's category (two-stage selection, see
   the spec's §7.2), so the taxonomy stays exact even when the model is vague.

Your own edits (`library set`, or the GUI) lock a row; no model and no forced pass ever
overwrites them.

## Quick start

```bash
soundpalette library init ~/sfx                 # analyze + offline classification
soundpalette library search ~/sfx --unannotated # what the names alone could not decide
soundpalette library annotate ~/sfx             # ask the model for those rows
soundpalette library search ~/sfx "creaky wooden door"
```

`annotate` sends only unannotated rows by default. `--all` upgrades filename-based rows too,
`--min-confidence 0.6` re-asks for everything below that confidence, `--dry-run` shows what
the model would write, `--limit n` caps a first try. Every annotation records `source`
(`filename`, `folder`, `model`, `human`), the model id, the prompt version and a timestamp;
`library show <file>` prints them together with the stage-2 candidate list.

## In the desktop app

Open the folder, switch to the **Library** tab. If the folder has no index yet the tab offers
to create one (same as `library init`). The tree on the left filters by UCS category and
CatID; the search box covers names, descriptions and keywords; the chips narrow to untagged,
low-confidence or locked rows. Clicking a row selects the sound and opens its **Annotation
(UCS)** section at the top of the inspector: CatID with type-ahead suggestions, FX name,
description, keywords. *Save* stores your edit as a human annotation and locks the row;
*Unlock* releases it; *Ask model* sends just this file to the annotator; the model's shortlist
appears as buttons. The Library menu creates/updates the index, runs the offline
classification, annotates the unannotated rows, the selection or the current search result,
cancels a running job, and sets the annotator command. The footer shows the index size, the
job's progress, and whether the annotator answers.

## The annotator

The core never talks to a model directly. It starts an **annotator command** and speaks a small
JSON-lines protocol with it over stdin/stdout. The command is, in order of precedence:

1. `--annotator "<command>"` on the command line,
2. the `SP_ANNOTATOR` environment variable,
3. `soundpalette-annotate` on your `PATH`.

SoundPalette ships one reference annotator in the `mcp/` package (Node 20+):

```bash
cd mcp && npm ci && npm run build
npm link        # puts soundpalette-annotate and soundpalette-annotate-mock on PATH
```

or, without linking: `--annotator "node /path/to/soundpalette/mcp/dist/annotate.js"`.

The reference annotator calls any **OpenAI-compatible chat-completions endpoint that accepts
`input_audio` content parts**. Configure it with environment variables (or the same names as
flags after the script, e.g. `--base-url`):

| Variable | Default | Meaning |
|---|---|---|
| `SP_LLM_BASE_URL` | `http://127.0.0.1:8080/v1` | endpoint base URL |
| `SP_LLM_API_KEY` | (none) | bearer token for hosted endpoints |
| `SP_LLM_MODEL` | first id from `/models` | model name sent in requests |
| `SP_LLM_CONCURRENCY` | `2` | parallel requests to the endpoint |
| `SP_LLM_MAX_TOKENS` | `400` | answer budget |
| `SP_LLM_TEMPERATURE` | `0.2` | sampling temperature |

`--inflight n` (default 4) is how many files SoundPalette keeps in flight toward the annotator;
`--timeout s` (default 120) kills and restarts a stuck annotator after one unanswered request.
Timeouts and restarts are counted in the summary; the run never blocks on a single file.

### Local: llama.cpp + Qwen2-Audio (recommended)

Qwen2-Audio-7B-Instruct (Apache-2.0) runs on a 12 GB GPU in 4-bit. With a llama.cpp build
that includes the multimodal server:

```bash
llama-server -m Qwen2-Audio-7B-Instruct-Q4_K_M.gguf --mmproj mmproj-Qwen2-Audio-7B-Instruct.gguf \
             -ngl 99 --port 8080 -c 8192
export SP_LLM_BASE_URL=http://127.0.0.1:8080/v1
soundpalette library annotate ~/sfx --annotator "node mcp/dist/annotate.js"
```

vLLM works the same way: `vllm serve Qwen/Qwen2-Audio-7B-Instruct --port 8000` and
`SP_LLM_BASE_URL=http://127.0.0.1:8000/v1`. Newer audio models (Qwen2.5-Omni, Qwen3-Omni) are a
change of endpoint, not of SoundPalette; the prompt asks for JSON only and the annotator
retries once when a model chats instead.

### Hosted endpoints

Any provider whose chat API accepts `input_audio` parts works: set `SP_LLM_BASE_URL`,
`SP_LLM_API_KEY` and `SP_LLM_MODEL`. **Privacy note**: the annotator uploads a 16 kHz mono
excerpt (up to 30 s) of every file it annotates to that endpoint, together with the file name.
Nothing else leaves your machine, and nothing is uploaded unless you run `library annotate`
with a hosted URL. The default configuration is local.

### Writing your own annotator

Any program that reads one JSON object per line on stdin and writes one per line on stdout can
be an annotator (Python, Go, a shell around a different API). The contract:

```jsonc
// handshake — first line each way
{"v":1,"hello":true}
{"v":1,"hello":true,"name":"my-annotator","model":"whatever","prompt_version":"v1",
 "stages":["describe","choose"]}

// stage 1 request (one per file; answer out of order if you like, `id` correlates)
{"v":1,"id":"12","stage":"describe","path":"ui/click.wav","sha256":"…",
 "audio":{"path":"/tmp/…/12-0.wav","sample_rate":16000,"duration_s":0.5,"truncated":false},
 "analysis":{"lufs_i":-21.3,"describe":"bright, thin, tonal; …","words":[…],"psycho":{…}},
 "categories":[{"name":"AIR","sub_categories":["BLOW","BURST",…]},…]}
{"v":1,"id":"12","description":"…","fx_name":"…","category":"USER INTERFACE",
 "keywords":["…"],"confidence":0.7}

// stage 2 request (only if you advertised "choose"; else the top candidate is taken)
{"v":1,"id":"13","stage":"choose","path":"ui/click.wav","description":"…","fx_name":"…",
 "keywords":[…],"candidates":[{"cat_id":"UIClick","category":"USER INTERFACE",
 "sub_category":"CLICK","explanation":"…"},…]}
{"v":1,"id":"13","cat_id":"UIClick","confidence":0.8}

// any request may be answered with {"v":1,"id":"…","error":"…"}
```

Log to stderr, never to stdout. The audio file is deleted after your answer. The mock
annotator (`mcp/src/annotate_mock.ts`) is a complete, tiny example.

## What the model cannot know

An audio-language model hears a mono excerpt. It cannot know a recording's provenance, rights
or intended use, it confuses acoustically similar sources, and its confidence is a self-report
calibrated by nothing. UCS is a sound-effects taxonomy; music and dialogue land in its MUSIC and
VOICE branches only coarsely. Treat model annotations as a first pass to search by and correct,
which is what locking is for.
