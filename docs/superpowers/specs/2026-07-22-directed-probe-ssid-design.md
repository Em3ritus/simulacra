# Directed-Probe SSID Realism — Design

**Date:** 2026-07-22
**Status:** Approved (design), pending implementation plan
**Roadmap:** Phase 3/4 — Wi-Fi probe realism (cross-protocol personas follow-up)

## Goal

Make a realistic minority of decoy probe agents probe for **named** SSIDs (not only the wildcard
broadcast probe), so the fake-phone crowd matches the measured real-crowd behavior and blends into
the same networks real phones are already probing everywhere. Today every decoy agent probes
wildcard-only, 100% of the time (`main/probe_frame.c`: every archetype tail begins with a wildcard
SSID element `0x00,0x00`).

## Motivation — measured, not assumed

An earlier session banked this idea but flagged it as possibly-not-worth-it ("modern phones are
mostly wildcard anyway"). This session's gate-check **disproved that assumption with real data.**
`tools/probe_audit/analyzers/kismet_behavior.py::reference_profile` measured `wildcard_fraction`
across three real Kismet captures with decreasing decoy contamination:

| Capture | decoy contamination | measured real wildcard_fraction |
|---|---|---|
| `Kismetscannew` | heavy (fixed-16 fleet) | 0.84 |
| `718scan` | mixed-firmware | 0.81 |
| `newest` | cleanest (post-fix fleet) | **0.36** |

The trend is diagnostic: our always-wildcard decoys *inflate* the measured wildcard fraction, so the
cleanest capture (fewest residual decoys) is the most trustworthy read of the true real crowd —
**~0.36 wildcard**, i.e. **~64% of real devices probe at least one named SSID.** Decoys sit at 1.0.
That is a large, real behavioral gap, directly measurable on the `wildcard_fraction` audit axis.

**Calibration anchor (honest caveat):** 0.36 is one location/capture; treat it as an evidence-based
anchor to aim near (target decoy wildcard fraction ~0.36–0.45), not a universal constant. Landing in
that band — versus the current 1.0 — is the success criterion, not hitting a specific decimal.

## Safety invariants (the part that must not be gotten wrong)

1. **Never draw an SSID from anything locally observed.** The pool is a fixed, compiled-in, curated
   list of generic/common **public** network names only. The on-device `observe`/`learn` pipeline
   (which sees the user's *actual* nearby/known networks) is **never** a source. Probing for a real
   local SSID would announce the user's actual location/associations, or be indistinguishable from
   genuinely being on that network — the exact deanonymization this whole project exists to prevent.
   This is a hard, tested invariant.
2. **Per-persona independent draw — no shared PNL constellation.** Each persona independently draws
   its own small random subset of the pool. The design explicitly does **not** assign one shared
   SSID list to a node's whole population (that recreates a per-node shared-PNL fingerprint — the
   exact constellation tell removed elsewhere) and does **not** enforce node-to-node SSID uniqueness
   (real common SSIDs like retail captive portals are probed by *many* devices at once; forcing
   uniqueness is itself unrealistic). Popular pool entries naturally recur across personas =
   realistic overlap, which is correct, not a leak.
3. **Law-3 is unaffected and does not need extending.** Verified against `components/simulacra_radar/
   law3.c`: Law-3 is BLE-manufacturer-data-specific (it scans for the Apple Continuity/Find-My
   subtype bytes after the `4C 00` company prefix). A Wi-Fi probe request triggers no pairing UI on
   any device regardless of SSID — directed SSID probing is a *different, simpler* safety property
   (invariant #1 above), not a Law-3 case. No Law-3 code changes.

## Architecture

Three small, well-bounded units.

### 1. SSID pool — `main/ssid_pool.{h,c}` (new, pure, host-testable)

A fixed compiled-in table of generic/common public SSID strings with per-entry weights (popular
names weighted higher so they recur realistically). Pure accessors only:

```c
#define SSID_POOL_MAX_LEN 32   // 802.11 SSID element max
int         ssid_pool_count(void);
const char *ssid_pool_at(int i, uint8_t *len_out);   // NUL-terminated name + its byte length
int         ssid_pool_pick_weighted(void);           // weighted random index (esp_random)
```

Curation principle (documented in the header): entries are names that appear in a very large number
of real devices' preferred-network lists across many locations — ubiquitous captive-portal / carrier
/ default-router names — so a directed probe for one is indistinguishable from the real background
and reveals nothing about *this* user. No brand-owned proprietary identifiers that could imply a
specific private network. Exact list finalized in the plan; it is data, trivially editable later.

### 2. Per-persona SSID assignment — `main/probe_agents.{h,c}`

`probe_agent_t` gains a tiny fixed-size assigned set (pool indices, not raw bytes — an index into
the trusted pool cannot smuggle arbitrary/observed bytes):

```c
#define AGENT_SSID_MAX 3
uint8_t ssid_n;                 // 0 = permanently wildcard-only (this persona has no named nets)
uint8_t ssid_idx[AGENT_SSID_MAX];
```

Assigned **once per persona life**, in `agent_spawn` and `probe_agent_sync` (the two identity-birth
sites), NOT on intra-life MAC rotation — a real phone's saved-network list is a property of the
*device/session*, not of a MAC rotation. With probability `~0.62` (the calibration target) a persona
draws 1–3 distinct pool entries via `ssid_pool_pick_weighted`; otherwise `ssid_n = 0` (stays
wildcard for its whole life). This split is what moves the aggregate wildcard fraction from 1.0 to
~0.38.

**Per-burst choice.** When a persona *has* an assigned set, each individual probe burst still
independently chooses wildcard vs. one of its own assigned names (real phones interleave both within
a scan cycle). The exact per-burst wildcard probability is chosen so the *device-level* wildcard
fraction the audit measures lands in the target band — the plan derives it from the persona-level
0.62 and validates empirically, since the audit measures per-device (per-MAC) wildcard status.

### 3. Frame builder — `main/probe_frame.c`

`probe_build_request` currently `memcpy`s the whole archetype tail, whose first 2 bytes are the
hardcoded wildcard SSID element. Change the signature to accept an optional SSID:

```c
int probe_build_request(const uint8_t mac[6], uint8_t ch, probe_arch_t arch, bool band5,
                        const char *ssid, uint8_t ssid_len,   // NULL/0 -> wildcard (unchanged)
                        uint8_t *out, size_t *out_len);
```

Compose in two pieces instead of one blind copy: emit the SSID element ourselves
(`0x00, ssid_len, <bytes>` for directed, or `0x00, 0x00` for wildcard), then copy the archetype tail
**from offset 2 onward** (everything after its placeholder wildcard element) unchanged. **When
`ssid==NULL`, the output is byte-identical to today** — zero risk to the existing archetype
fingerprints the `probe_audit` byte-exact tests pin. Guard the frame length: `24 + 2 + ssid_len +
(tail_len - 2) <= PROBE_FRAME_MAX`; pool entries are short (≤ ~15 chars) so this never trips, but the
bound is checked and returns the existing error path if it ever would.

Caller (the live probe injector in `main/probe.c`) resolves the due agent's per-burst SSID choice and
passes it through.

## Data flow (per probe burst)

1. Persona/agent is born (`agent_spawn` / `probe_agent_sync`) → with ~0.62 probability it draws 1–3
   pool SSID indices into `ssid_idx[]`; else `ssid_n = 0`.
2. A burst comes due → if `ssid_n > 0`, roll per-burst wildcard-vs-named; if named, pick one of the
   agent's own `ssid_idx[]` and resolve it via `ssid_pool_at`.
3. `probe_build_request(..., ssid, ssid_len, ...)` emits the frame (wildcard path byte-identical to
   today).

## Testing (host, `tools/probe_audit` — all existing infrastructure)

- **`ssid_pool` (pure):** `ssid_pool_count > 0`; every entry `≤ SSID_POOL_MAX_LEN` and non-empty;
  `ssid_pool_pick_weighted` returns valid indices and (over many draws) favors higher-weighted
  entries; **invariant test:** the pool contains only the compiled-in strings (a structural test that
  the pool has no path to `observe`/`learn` — the module includes nothing from that pipeline).
- **`probe_frame` byte-exactness:** with `ssid=NULL`, `probe_build_request` output is byte-for-byte
  identical to the pre-change build for every archetype/band (extend the existing byte-exact
  fixtures — this is the safety net for "wildcard path unchanged"). With a directed SSID, the emitted
  SSID element is `0x00,len,<bytes>` and the rest of the body matches the wildcard body from offset 2
  on; total length within `PROBE_FRAME_MAX`.
- **`probe_agents` assignment:** over many spawns, ~62% of agents get `ssid_n > 0` (assert a band,
  e.g. 0.5–0.75, not an exact fraction); assigned indices are valid and distinct; assignment is
  stable across an intra-life MAC rotation (same `ssid_idx[]` before/after) and re-drawn on
  reincarnation.
- **End-to-end via `probe_behavior_scorecard.py`:** a new `probe_dump` mode dumps per-agent
  wildcard-vs-named burst behavior over a window; feed it through the existing `wildcard_fraction`
  axis and confirm the modeled decoy wildcard fraction drops from 1.0 into the ~0.36–0.45 target
  band. This is the headline verification the whole feature exists for.

## Scope check

Considered splitting "pool curation" from "draw mechanism" into two specs. Rejected — the pool is
~30 lines of static data with trivial pure accessors; it is not an independent subsystem, and the
draw mechanism is meaningless without it. One spec, three tightly-coupled files, one plan. The pool
*contents* are data and freely editable later without re-design.

## Out of scope

- Per-band or per-archetype SSID differentiation (e.g. Apple vs Android probing different network
  sets) — no evidence it matters; YAGNI. All personas draw from one pool.
- Directed probes on the BLE side — N/A (BLE has no SSID concept).
- Sourcing SSIDs from observed traffic — explicitly forbidden (safety invariant #1), never in scope.
- Tuning the pool to a *specific* capture's SSID distribution — the pool is generic-by-design; we
  match the wildcard *fraction*, not a particular location's network names.
