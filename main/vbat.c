#include "vbat.h"
#include "sdkconfig.h"

/* ---- Backend selection ------------------------------------------------------------------------
 * The sense hardware is a property of the BOARD, so it defaults per target instead of relying on
 * the operator to remember two -D flags on every build. Previously nothing passed them -- not
 * build_flash_read.ps1, not the CI flasher workflow -- so every shipped image compiled the
 * "disabled" stub below, vbat_mv() always returned -1, and the CYD rendered "USB" for every node
 * no matter what it was actually running on. The battery was never unread; it was never sensed.
 *
 * Wiring is from the board schematics (private/FLEET-SETUP.md §9). Explicit -D flags still win, so
 * a differently-wired board can override without touching this file, and an unknown target still
 * compiles out to nothing.
 */
#if !defined(SIMULACRA_VBAT_MAX17048) && !defined(SIMULACRA_VBAT_ADC)
  #if defined(CONFIG_IDF_TARGET_ESP32C5)
    /* Waveshare ESP32-C5-WIFI6-KIT: no gauge; BAT--200k--+--100k--GND divider, node on GPIO6. */
    #define SIMULACRA_VBAT_ADC       1
    #define SIMULACRA_VBAT_ADC_GPIO  6
  #elif defined(CONFIG_IDF_TARGET_ESP32C6)
    /* SparkFun Thing Plus C6: MAX17048 fuel gauge at 0x36 on SDA=GPIO4 / SCL=GPIO7. A C6 board
     * without this gauge must override with -DSIMULACRA_VBAT_ADC=1 (or disable it explicitly);
     * probing an absent device is harmless but will just report "no cell". */
    #define SIMULACRA_VBAT_MAX17048  1
    #define SIMULACRA_VBAT_SDA       4
    #define SIMULACRA_VBAT_SCL       7
  #endif
#endif

/* ---- Backend 1: MAX17048/MAX17049 fuel gauge over I2C (SparkFun Thing Plus C6) ---------------- */
#if defined(SIMULACRA_VBAT_MAX17048)
#include "driver/i2c_master.h"
#include "esp_log.h"
#ifndef SIMULACRA_VBAT_LOW_PCT
#define SIMULACRA_VBAT_LOW_PCT 15
#endif
#if !defined(SIMULACRA_VBAT_SDA) || !defined(SIMULACRA_VBAT_SCL)
#error "SIMULACRA_VBAT_MAX17048=1 requires -DSIMULACRA_VBAT_SDA=<gpio> and -DSIMULACRA_VBAT_SCL=<gpio>"
#endif
#define MAX17048_ADDR 0x36
#define REG_VCELL     0x02   // 16-bit, 78.125 uV/LSB
#define REG_SOC       0x04   // 16-bit, 1/256 %/LSB

static const char *VTAG = "vbat";
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int  s_mv = -1, s_soc = -1;
static uint8_t s_fail_streak;   // consecutive sample() calls where BOTH regs failed to read

static int rd_reg(uint8_t reg, uint16_t *out)
{
    uint8_t r = reg, b[2];
    if (i2c_master_transmit_receive(s_dev, &r, 1, b, 2, 100) != ESP_OK) return -1;
    *out = (uint16_t)((b[0] << 8) | b[1]);   // MAX17048 is big-endian
    return 0;
}
// ensure_present() only ever sets s_present=true (never re-checks), so a gauge that later stops
// answering -- battery unplugged, I2C glitch, gauge browned out -- used to leave s_present latched
// true forever with the last-known s_mv/s_soc reported as current, indistinguishable from a live
// reading. Reset the latch after a few straight total-failure samples so callers correctly fall
// back to "absent" instead of a frozen stale value.
static void sample(void)
{
    uint16_t v, s;
    bool ok_v = (rd_reg(REG_VCELL, &v) == 0);
    bool ok_s = (rd_reg(REG_SOC,   &s) == 0);
    if (ok_v) s_mv  = (int)((uint32_t)v * 5 / 64);   // 78.125 uV = 5/64 mV
    if (ok_s) {
        int soc = (int)(s / 256);
        s_soc = soc > 100 ? 100 : soc;   // MAX17048 SOC can read a few % over 100 near full charge
    }
    if (ok_v || ok_s) {
        s_fail_streak = 0;
    } else if (++s_fail_streak >= 3) {
        s_present = false; s_mv = -1; s_soc = -1; s_fail_streak = 0;
        ESP_LOGW(VTAG, "MAX17048 stopped answering -- treating as absent");
    }
}
void vbat_init(void)
{
    i2c_master_bus_config_t bc = {
        .clk_source = I2C_CLK_SRC_DEFAULT, .i2c_port = -1,
        .sda_io_num = SIMULACRA_VBAT_SDA, .scl_io_num = SIMULACRA_VBAT_SCL,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { ESP_LOGW(VTAG, "i2c bus init failed"); return; }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = MAX17048_ADDR, .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) { ESP_LOGW(VTAG, "i2c add dev failed"); return; }
    uint16_t probe;
    if (rd_reg(REG_VCELL, &probe) == 0) { s_present = true; sample();
        ESP_LOGW(VTAG, "MAX17048 present: %d mV, %d%%", s_mv, s_soc); }
    else ESP_LOGW(VTAG, "no fuel gauge yet on I2C (SDA=%d SCL=%d) -- will detect on battery connect",
                  SIMULACRA_VBAT_SDA, SIMULACRA_VBAT_SCL);
}
// Re-probe if not yet seen: the gauge only answers once V_BATT is powered (a LiPo is connected),
// so a battery plugged into a running board is picked up here without a reboot. s_dev is valid
// after init (add_device succeeds even if the gauge didn't ACK).
static void ensure_present(void)
{
    if (s_present || !s_dev) return;
    uint16_t p;
    if (rd_reg(REG_VCELL, &p) == 0) { s_present = true; sample();
        ESP_LOGW(VTAG, "MAX17048 detected: %d mV, %d%%", s_mv, s_soc); }
}
const char *vbat_backend(void) { return "max17048"; }
bool vbat_present(void) { ensure_present(); return s_present; }
int  vbat_mv(void)      { ensure_present(); if (s_present) sample(); return s_present ? s_mv  : -1; }
int  vbat_soc_pct(void) { ensure_present(); if (s_present) sample(); return s_present ? s_soc : -1; }
bool vbat_low(void)     { ensure_present(); return s_present && s_soc >= 0 && s_soc < SIMULACRA_VBAT_LOW_PCT; }

/* ---- Backend 2: ADC voltage divider (Waveshare ESP32-C5-WIFI6-KIT: BAT_ADC on GPIO6, /3) ------ */
#elif defined(SIMULACRA_VBAT_ADC)
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#ifndef SIMULACRA_VBAT_ADC_GPIO
#error "SIMULACRA_VBAT_ADC=1 requires -DSIMULACRA_VBAT_ADC_GPIO=<gpio>"
#endif
#ifndef SIMULACRA_VBAT_ADC_DIV
#define SIMULACRA_VBAT_ADC_DIV 3          // 200k/100k divider -> Vbat = Vadc * 3
#endif
#ifndef SIMULACRA_VBAT_LOW_MV
#define SIMULACRA_VBAT_LOW_MV 3400
#endif
#define VBAT_PRESENT_MV 2500              // below this = no cell / floating, not a real battery

static const char *VTAG = "vbat";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static adc_unit_t s_unit;
static bool s_ready;

void vbat_init(void)
{
    if (adc_oneshot_io_to_channel(SIMULACRA_VBAT_ADC_GPIO, &s_unit, &s_chan) != ESP_OK) {
        ESP_LOGW(VTAG, "gpio %d is not ADC-capable", SIMULACRA_VBAT_ADC_GPIO); return; }
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = s_unit };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { ESP_LOGW(VTAG, "adc unit init failed"); return; }
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, s_chan, &cc);
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = s_unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK)
        ESP_LOGW(VTAG, "adc calibration unavailable (using raw scaling)");
    s_ready = true;
    ESP_LOGW(VTAG, "battery ADC gpio %d (div %d): %d mV", SIMULACRA_VBAT_ADC_GPIO, SIMULACRA_VBAT_ADC_DIV, vbat_mv());
}
int vbat_mv(void)
{
    if (!s_ready) return -1;
    int raw; if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) return -1;
    int mv;
    if (s_cali) { if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1; }
    else mv = raw * 3300 / 4095;                     // crude fallback: 12-bit, ~3.3V ref
    return mv * SIMULACRA_VBAT_ADC_DIV;
}
const char *vbat_backend(void) { return "adc"; }
int  vbat_soc_pct(void) { return -1; }               // an ADC divider gives voltage, not SoC
bool vbat_present(void) { return vbat_mv() > VBAT_PRESENT_MV; }
bool vbat_low(void)     { int mv = vbat_mv(); return mv > VBAT_PRESENT_MV && mv < SIMULACRA_VBAT_LOW_MV; }

/* ---- Disabled: no battery hardware, zero cost --------------------------------------------------- */
#else
void vbat_init(void)    {}
const char *vbat_backend(void) { return "none"; }
bool vbat_present(void) { return false; }
int  vbat_mv(void)      { return -1; }
int  vbat_soc_pct(void) { return -1; }
bool vbat_low(void)     { return false; }
#endif
