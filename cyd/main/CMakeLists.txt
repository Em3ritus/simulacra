# The Vigil control SECRET is gitignored (see .gitignore), so a fresh clone has no sim_ctrl_sk.h.
# Only required when the control plane is actually built: without SIMULACRA_CONFIG_CTRL there is no
# signing path at all, so demanding a signing secret would fail a build that never uses one. That
# is exactly what the starter / web-flasher images are, and why they build without the flag.
#
# Do NOT bring this up by copying sim_ctrl_sk.h.example. That file's public half does not match the
# committed sim_ctrl_key.h, so a Vigil built from it signs commands every decoy rejects: a control
# page that silently does nothing. gen_ctrl_key.py writes BOTH halves together, which is the only
# way they are guaranteed to agree.
if(DEFINED SIMULACRA_CONFIG_CTRL AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/sim_ctrl_sk.h")
    message(FATAL_ERROR
        "SIMULACRA_CONFIG_CTRL is set but cyd/main/sim_ctrl_sk.h is missing. It is gitignored "
        "because it holds the Vigil signing secret, which grants control over every board that "
        "trusts the matching public half. "
        "Generate one with: python tools/gen_ctrl_key.py  (writes both halves; reflash ALL boards). "
        "Or build without the control plane by omitting -DSIMULACRA_CONFIG_CTRL.")
endif()

idf_component_register(
    SRCS "cyd_main.c" "fleet_db.c" "fleet_status.c" "expo_sniff.c"
    INCLUDE_DIRS "."
    REQUIRES simulacra_radar esp_lcd esp_wifi esp_netif esp_event nvs_flash mbedtls driver esp_timer fatfs sdmmc
)

# Forward build-time gate flags passed via `idf.py -DFLAG=VALUE` to the C preprocessor,
# mirroring main/CMakeLists.txt. SIMULACRA_CONFIG_CTRL enables the Vigil CONTROL page + SEND;
# SIMULACRA_FLEET_PROVISION enables enrollment authority; FLEET_SELFTEST runs fleet_db checks.
foreach(flag SIMULACRA_CONFIG_CTRL SIMULACRA_FLEET_PROVISION FLEET_SELFTEST)
    if(DEFINED ${flag})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${flag}=${${flag}})
    endif()
endforeach()
