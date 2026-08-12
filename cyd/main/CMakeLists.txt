# The Vigil control SECRET is gitignored (see .gitignore), so a fresh clone has no
# sim_ctrl_sk.h. Fail with an actionable message rather than a bare "no such file".
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/sim_ctrl_sk.h")
    message(FATAL_ERROR
        "cyd/main/sim_ctrl_sk.h is missing (it is gitignored — it holds the Vigil signing secret).\n"
        "  Bring-up:  cp cyd/main/sim_ctrl_sk.h.example cyd/main/sim_ctrl_sk.h   (PUBLIC key — no control auth)\n"
        "  Deploy:    python tools/gen_ctrl_key.py                               (fresh keypair; reflash all boards)")
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
