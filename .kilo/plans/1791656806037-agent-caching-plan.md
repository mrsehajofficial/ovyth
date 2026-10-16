# Agent caching plan

Owner: an implementation-capable agent. Work in `/home/sehaj/Work/work/project/ovyth`
(branch `main`, `make -j4` to build, `make test` for the suites).

---

## 1. What an autonomous agent actually needs cached

Autonomy is what changes the answer. An agent you drive yourself gets to
complain about slowness; one that runs unattended for an afternoon does not, and
by then the bill and the slowdowns have compounded. Three properties follow,
and no cache may violate them:

1. **Cost per loop iteration must not compound silently.** The model call is
   the expensive thing, and a multi-step turn makes 3–8 of them, each repeating
   the previous request's prefix.
2. **No scheduled degradation.** Anything whose cost grows with the agent's own
   memory has to be fixed *before* memory gets useful, because an unattended
   agent cannot report that everything got slower.
3. **No cache may make a wrong answer more confident.** A cache that can serve
   something stale, or answer a question about this machine without observing
   it, is worse than no cache.

And one explicit **rejected** option, so it is not proposed again: answering a
repeat question from the memory store without a model call. The system prompt
forbids the model from answering about this machine from memory
(`examples/agent/tui.ov:373-375`), and a lexical matcher cannot separate
"what is my cat's name" from "what is in /etc". A fast path there would trade
latency for confidently wrong answers, which is the wrong trade for an agent
nobody is watching.

---

## 2. Context: what is already cached, so nobody re-does it

- **Connections.** `ov_http_request` is a thin adapter over the libcurl pool
  (`runtime/src/http.c:35-37`), and the `http.post` builtin goes through it
  (`runtime/src/http_builtin.c:83`). The interpreter path uses the same
  function (`compiler/src/pkgman.cpp:444`). TCP+TLS is paid once per host.
  Nothing to do here.
- **Registry.** The package-manager cache is already URL-keyed with `--refresh`.
- **Per-request size is bounded.** `ai_trim(history, 16)` builds a fresh list
  per request (`compiler/... ai.ov:56-74`), so a request never exceeds 16
  messages plus the system prompt even though `history` itself grows for the
  whole session. The request does *not* grow forever; the store does.

---

## 3. The plan

### A. Make recall's cost independent of store size *(do first)*

**The bug.** Every turn files its exchange into the RAG store
(`examples/agent/tui.ov:60-64`, called at `:968`), plus every `{"remember": ...}`
(`:697-704`). Nothing evicts. Recall then tokenizes the entire store on every
query: `rag_topk` scores every chunk (`tests/fixtures/rag.ov:212-224`), and
`rag_score` calls `rag_words` — `lower()` + `split()` — on each chunk's text
(`:180-209`), plus an O(terms²) dedup via `seen`. At 50 chunks nobody notices.
At a few thousand chunks an unattended agent pays tens of thousands of string
allocations per turn, forever, with nothing to report it.

**The fix: a caller-owned inverted index.** Follow the package's existing
contract — stateless, everything the caller owns, nothing held between calls
(`tests/fixtures/rag.ov:18-21`).

- `rag_index(store)` → a map from word → sorted list of chunk positions.
- `rag_index_update(index, store, from)` → indexes only chunks appended after
  `from`, so `rag_add`'s output can be indexed incrementally. Return the count
  indexed so the caller can assert it.
- `rag_topk(store, query, k, idx)` → new optional 4th argument.
  - `idx == null` → today's full-scan path, unchanged (backward compatible).
  - `idx != null` → candidate positions from the index, deduped, **sorted
    ascending by position** (`rag_sort` is a selection sort that keeps the
    first-seen maximum, so input order decides ties and the ascending order
    preserves what the scan produces); score those with the existing
    `rag_score`, verbatim.
  - Pad the tail with zero-score non-candidates, in position order, only if
    fewer than `k` hits were found — so callers other than the agent
    (`rag_prompt`) see exactly the list they see today.

**Why this is provable, not hopeful:** a chunk that scores > 0 must contain at
least one query term, so it must be a candidate. Every skipped chunk would have
scored 0. The recall path already breaks on `score <= 0`
(`examples/agent/tui.ov:133`), so the visible result cannot change.

**Wiring.** `rag` is imported from the package store, and
`tests/fixtures/rag.ov` is a pinned snapshot (`:1-13`) — bump both. The agent
holds the index beside the store: `midx = [rag_index(mem[0])]`, updated in
`remember_turn` and the `remember` tool, and passed to `recall()`'s `rag_topk`.

**Validation**
- Differential test: build a store of ~1000 chunks from a generated corpus, and
  for a list of queries assert `rag_topk(s, q, k)` and
  `rag_topk(s, q, k, rag_index(s))` return **identical** lists — same texts, same
  scores, same order. Also assert recall blocks: run
  `tests/fixtures/rag.ov` both ways, no agent, no network.
- Assert `rag_index_update` after `rag_add` keeps the index correct (re-run the
  differential test after every append).
- Keep the memory suite passing unchanged — those 12 checks are the regression
  net for "same answers".

### B. Session caches that are exact and fail safe

**B1. Resolved-name cache for the sweep.** `sweep_not_found` runs
`command -v` over up to 9 invented variants every time
(`examples/agent/tui.ov:564-598`). A model that consistently invents
`google-chrome` pays a fork per attempt. Cache the *name actually found*
(`sweep_hit = [{}]`) keyed by the invented first token, and consult it before
shelling out. Safe: PATH does not change inside one session, and a missing
entry just does what it does today. On a hit, report the earlier resolution
without re-forking, and say it was resolved earlier.

**B2. Read-only command cache, conservative whitelist.** A looping model will
ask for the same command twice in one turn. Keep a per-turn ledger
(`cmd_cache = [{}]`) keyed by the exact command string, and serve it only for a
small explicit whitelist (`pwd`, `ls`, `cat`, `uname`, `date`, `whoami`,
`nproc`, `id`, `df`, `free`, `ip`, `env`, `hostname`, `uptime`). Anything not
on the list runs, always — so a `make` or a `cp` can never be served from
cache. Clear it at the end of each turn (`handle_turn`), not each session: a
`ls` cached across two user turns would be stale. When served, the reply text
must say the result was cached, so the model knows it is seeing the earlier
result and can reason about it.

**Validation.** Unit-level via a fixture: point the agent at a gateway that
asks for `pwd` twice and assert the second answer comes from the cache with no
second `os.run` (count forks via the agent's own output); ask for `make` twice
and assert it runs twice. New checks in `tests/memory.sh` (or a sibling suite
if `tests/memory.sh` starts to feel like the wrong home — the net suite is
the remote one; the agent suites are `tests/memory.sh` and whatever the caches
belong in).

### C. Provider prompt caching, opt-in *(last)*

**The bug.** Each request re-sends the previous request's prefix byte for byte.
For an unattended agent this is the dominant recurring cost, and it is invisible
per-turn.

**The fix, conservative:** `:config cache on|off`, persisted in
`~/.ovyth/agent.json` like key/url/model (`examples/agent/tui.ov:293-297`).
When on — and only on — the request body carries the Anthropic-style
`cache_control: {"type": "ephemeral"}` marker on the messages that form the
stable prefix (the system message and the priming exchange; the marker belongs
on the *last* message of the prefix). Default off. This is not auto-detected:
an endpoint that ignores the field is fine, an endpoint that rejects it must
not take an agent down that has not asked for this.

Add the flag to `ai_request`'s body only when enabled — that means the `ai`
package needs the option, so bump it (currently 1.0.4) and refresh
`tests/fixtures/ai.ov` (`:1-13`), keeping the documented `ai_path` deviation.

**Validation.** A fixture gateway that records the request body; assert
`cache_control` is absent by default, present when `cache on`, and that the
marker sits on the right message. No real endpoint, no billing, no network.

---

## 4. Decisions taken, so they are not re-litigated

| Decision | Choice |
| --- | --- |
| Response cache for repeat questions | **No.** Can't separate machine-state from personal facts; violates `tui.ov:373-375`. |
| Blanket `os.run` cache | **No.** Side effects. B2's whitelist is the fail-safe form. |
| Store eviction / decay | **Out of scope here**, but it is the natural next question: A makes recall cheap at any size, yet the store still grows forever. Raise it with the user, do not decide it in this plan. |
| Prompt caching default | **Off.** Opt-in, per `:config`, never auto-detected. |
| Rag package state | Stays stateless. The index is a value the caller owns, like the store. |

## 5. Risks

- **Snapshot drift.** `tests/fixtures/{ai,rag}.ov` are pinned copies
  (`:1-13`); publishing without refreshing them turns the suites green against
  code nobody runs. Refreshing them is part of the change.
- **Exactness.** The claim that the index changes no answer must be demonstrated
  by A's differential test, not argued. If the test needs the padding rule to
  pass, keep the padding rule.
- **Native backend.** Top-level `let` is invisible to functions there (see the
  note in `tests/fixtures/ai.ov:9-11`). Any new shared state stays a
  one-element list, as `mem`, `sink`, `key` already are.
- **B2's whitelist.** It is a maintenance trap by nature. It must fail closed —
  unknown command runs — and the list should stay short enough to read in one
  glance.

## 6. Validation before pushing

```
make -j4
make test          # 54 core + 43 pkg + 25 ui + 9 net + 12 memory, all green
```
plus the new differential and cache checks. Then
`OVYTH_AGENT_DEBUG=1` run against a fixture gateway once by hand, so the
recall block and the request body are both eyeballed before commit.
