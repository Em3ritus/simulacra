# docs/

Two parallel trees, both kept. They differ by *when* and *how* they were written, not by status —
neither supersedes the other, and both describe shipped behaviour.

| Tree | Period | What it holds |
|---|---|---|
| `design/{plans,specs}` | Jun–Jul 2026 | The original milestone design work: the decoy pipeline, the wire protocol, the Vigil control node. Written before the plan/spec workflow was formalised. |
| `superpowers/{plans,specs}` | Jul 2026 → | Everything since, one document per change, written to a fixed plan → spec → implement shape. Newer work is here. |

**Which one is authoritative?** The code is. Where a document and the source disagree, the source
wins and the document is stale — these are records of intent at a point in time, not specifications
the firmware is validated against. The behavioural contracts that *are* enforced live in
`main/churn_selftest.c` (on-target) and `tools/*/tests/` (host).

**Finding the reasoning behind a piece of code:** most non-obvious decisions carry a comment naming
the document that motivated them. Search for the filename rather than browsing either tree.

**`hardware/`** holds board notes and pinouts; `ROADMAP.md` is the current forward view.
