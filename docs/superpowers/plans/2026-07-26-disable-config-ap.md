# Disable the Config-AP by Default Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Flip the `SIMULACRA_WEBUI` build-flag default from `1` to `0` so a default decoy no longer boots the open, no-auth `simulacra-XXXX` config AP (the CYD is the control path); keep the webui code gated as an opt-in.

**Architecture:** A single one-line default flip at the sole `#ifndef SIMULACRA_WEBUI` site, plus its adjacent comment. The boot sequence already has a proven `#else` branch that brings Wi-Fi/ESP-NOW up directly, so no logic changes.

**Tech Stack:** C (ESP-IDF firmware), compile-verified via the build-flash-read skill.

## Global Constraints

- Flip **only** `main/simulacra_main.c:88` (`#define SIMULACRA_WEBUI 1` → `0`). This is the single default site.
- Do **not** touch `observe.c` — it defaults `SIMULACRA_LEARN` (self-learning), not `SIMULACRA_WEBUI`.
- Keep all webui code, handlers, `webui_index.html`, `EMBED_TXTFILES`, and the CMake flag entry — the path must still build under an explicit `-DSIMULACRA_WEBUI=1`.
- Commit identity is the repo-local `Em3ritus` noreply. Every commit carries the trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy`.

---

### Task 1: Flip the default + verify both build paths

**Files:**
- Modify: `main/simulacra_main.c:86-88` (the comment + the default)

- [ ] **Step 1: Flip the default and update the comment**

In `main/simulacra_main.c`, change the webui comment + default block from:

```c
// Web UI: on-demand open config AP for ~2 min at boot, then hand Wi-Fi to the decoy.
// Local MVP (open AP, no auth) -- default ON for now.
#ifndef SIMULACRA_WEBUI
#define SIMULACRA_WEBUI 1
#endif
```

to:

```c
// Web UI: on-demand open config AP at boot (status + /api/control), then hand Wi-Fi to the decoy.
// DEFAULT OFF: the CYD is the control path over the encrypted ESP-NOW link, so the open, no-auth
// simulacra-XXXX AP (a control surface + a self-identifying SSID tell) is opt-in only. Build a
// no-CYD decoy with -DSIMULACRA_WEBUI=1 to re-enable it.
#ifndef SIMULACRA_WEBUI
#define SIMULACRA_WEBUI 0
#endif
```

- [ ] **Step 2: Compile-verify c6 with the new default (WEBUI off)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Do build`
Expected: `BUILD: Project build complete.` (the `#else` boot branch compiles as the default).

- [ ] **Step 3: Compile-verify c5 with the new default**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c5 -Do build`
Expected: `BUILD: Project build complete.`

- [ ] **Step 4: Compile-verify the opt-in path still builds (`-DSIMULACRA_WEBUI=1`)**

Run: `& "$env:USERPROFILE\.claude\skills\build-flash-read\build_flash_read.ps1" -Target c6 -Do build -- -DSIMULACRA_WEBUI=1`

If the build-flash-read skill does not forward a trailing `-D` cleanly, instead run idf.py directly in the c6 IDF env, from the repo root:
```
idf.py set-target esp32c6
idf.py -DSIMULACRA_WEBUI=1 build
```
Expected: `Project build complete.` — confirms the webui path is intact behind the flag. (`SIMULACRA_WEBUI` is already in `main/CMakeLists.txt`'s forwarded flag list, so `-D` reaches the preprocessor.)

- [ ] **Step 5: Commit**

```bash
git add main/simulacra_main.c
git commit -m "$(cat <<'EOF'
feat(config-ap): disable the webui config-AP by default (SIMULACRA_WEBUI 1->0)

A default decoy no longer opens the open, no-auth simulacra-XXXX config
SoftAP. The CYD already provides status + control over the encrypted signed
ESP-NOW link, so the config AP was a redundant MVP surface plus a
self-identifying SSID tell. The proven #else boot path brings Wi-Fi STA +
probes + ESP-NOW up directly and ~30s sooner (no config-window block). The
webui code stays gated as an opt-in (-DSIMULACRA_WEBUI=1) for no-CYD decoys.
Compile-verified c5/c6 default-off and c6 with -DSIMULACRA_WEBUI=1.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
)"
```

---

## Notes for the implementer

- **No host test** — this is a boot-path default flip, not new logic. The compile-verifies (default-off
  on both targets + the explicit opt-in) are the gate.
- **On-air confirmation deferred** (needs hardware, out of this session): after flashing, a decoy should
  come up probing + visible on the CYD immediately, with no `simulacra-XXXX` AP present and no ~30 s wait.
- If a build dir carries a stale `SIMULACRA_WEBUI=1` cache define from a prior `-D` build, a target-switch
  or `idf.py fullclean` clears it (the default only governs when the flag is undefined).
