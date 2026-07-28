# Disable the on-decoy webui config-AP by default

**Date:** 2026-07-26
**Status:** design (approved, pre-plan)
**Area:** `main/simulacra_main.c`, `main/observe.c` (the `SIMULACRA_WEBUI` default)

## Goal

Stop every decoy from booting an open, unauthenticated Wi-Fi config access point. Flip the
`SIMULACRA_WEBUI` build-flag **default from `1` to `0`** so a normal decoy no longer opens the
`simulacra-XXXX` SoftAP; the webui code stays in the tree, gated, as an opt-in
(`-DSIMULACRA_WEBUI=1`) for a decoy run with no CYD.

## Why

The config AP (`webui.c`, `webui_run_config_window`) is a redundant MVP convenience:
- **The CYD already does status + control** over the encrypted, signed, replay-protected ESP-NOW link,
  so the config AP duplicates that function.
- It is an **open control surface**: `WIFI_AUTH_OPEN`, and `/api/control` (POST) lets anyone in RF
  range during the boot window pause or reconfigure the decoy — i.e. disable the protection.
- The SSID **`simulacra-XXXX` is a self-identifying tell** — it announces a Simulacra device by name,
  contradicting the project's blend-in premise.
- **Bonus:** `webui_run_config_window(30000)` *blocks* boot, and `esp_now_link_start()` runs after it,
  so the config window imposes a ~30 s "deaf at boot" delay before Wi-Fi probes and the ESP-NOW
  responder come up. Disabling it brings both up immediately — faster time-to-protection and the decoy
  appears on the CYD instantly instead of after ~33 s.

## Change

The boot sequence already has a proven no-webui branch (`simulacra_main.c:168-177`):

```c
#if SIMULACRA_WEBUI
    coexist_set_wifi_enabled(false);
    coexist_start();
    webui_run_config_window(30000);
    coexist_set_wifi_enabled(true);
#else
    coexist_start();                    // coexist brings Wi-Fi (STA) up directly; s_wifi_allowed defaults true
#endif
```

There is a **single** `SIMULACRA_WEBUI` default site — `main/simulacra_main.c:87-88`. Flip it:
- `main/simulacra_main.c:88` — `#define SIMULACRA_WEBUI 1` → `0`

(Note: `observe.c` defaults `SIMULACRA_LEARN`, not `SIMULACRA_WEBUI` — unrelated; do not touch it.)

No other code changes: with the default `0`, the `#else` path runs, `coexist_start()` brings Wi-Fi STA +
probes + observe up directly, and `esp_now_link_start()` (gated on `SIMULACRA_ESPNOW`) runs immediately
after. The webui source, handlers, `webui_index.html`, the `EMBED_TXTFILES`, and the `SIMULACRA_WEBUI`
CMake flag entry all remain — reachable only via an explicit `-DSIMULACRA_WEBUI=1`.

Update the adjacent source comment (`simulacra_main.c:86`) so it no longer reads "default ON": note the
config AP is now opt-in and off by default (the CYD is the control path).

## Testing

- **Firmware compile-verify (default, WEBUI now off):** c5 + c6 build; confirms the `#else` boot path
  compiles as the default.
- **Firmware compile-verify (opt-in preserved):** c5 + c6 with `-DSIMULACRA_WEBUI=1` still build (the
  webui path is intact for the no-CYD use case).
- No host unit test (this is a boot-path default change, not new logic). On-air confirmation — that a
  decoy comes up probing + visible on the CYD immediately, with no `simulacra-XXXX` AP — is deferred to
  a bench session (needs hardware).

## Out of scope

- Removing the webui code entirely (approach B) — deliberately kept gated as a no-CYD fallback.
- Any change to the CYD control path, ESP-NOW, or decoy behavior beyond the boot-time Wi-Fi bring-up.
- The separately-floated comprehensive CYD dashboard buildout (its own future brainstorm).
