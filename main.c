#include "board_pins.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "beatbox";
#define AUDIO_SAMPLE_RATE_HZ 32000U
#define AUDIO_BLOCK_FRAMES 128U
#define AUDIO_CHANNELS 2U
#define AUDIO_PPQN 96U
#define AUDIO_DEFAULT_BPM 120U
#define AUDIO_DEFAULT_VOLUME 80U
#define AUDIO_MIN_BPM 60U
#define AUDIO_MAX_BPM 240U
#define AUDIO_MIN_VOLUME 0U
#define AUDIO_MAX_VOLUME 100U
#define VOLUME_STEP_PERCENT 5U
#define CLICK_LENGTH_FRAMES ((AUDIO_SAMPLE_RATE_HZ * 9U) / 1000U)
#define CLICK_LUT_SIZE 256U
#define NOTE_COUNT 7U
#define REMOTE_VOICE_COUNT 16U
#define NOTE_ATTACK_FRAMES ((AUDIO_SAMPLE_RATE_HZ * 1U) / 1000U)
#define NOTE_RELEASE_FRAMES ((AUDIO_SAMPLE_RATE_HZ * 6U) / 1000U)
#define NOTE_VOICE_AMPLITUDE 4500U
#define AUDIO_TASK_STACK_WORDS 4096U
#define INPUT_TASK_STACK_WORDS 4096U
#define PROTOCOL_TASK_STACK_WORDS 3072U
#define INPUT_POLL_MS 5U
#define INPUT_POLL_TICKS ((pdMS_TO_TICKS(INPUT_POLL_MS) > 0) ? pdMS_TO_TICKS(INPUT_POLL_MS) : 1U)
#define INPUT_DEBOUNCE_POLLS 3U
#define CONFIG_NAMESPACE "beatbox"
#define CONFIG_KEY "config"
#define CONFIG_VERSION 1U
#define WS2812_COUNT 5U
#define WS2812_FRAME_BYTES (WS2812_COUNT * 3U)

typedef struct { uint8_t r, g, b, brightness; } rgb_color_t;
typedef struct __attribute__((packed)) {
    uint16_t version, length;
    uint32_t revision, volume, bpm;
    uint8_t notes[NOTE_COUNT], enabled[NOTE_COUNT];
    rgb_color_t colors[NOTE_COUNT];
    uint32_t crc32;
} persisted_config_t;
typedef struct { uint32_t phase, phase_step, amplitude_q16; bool active; } note_voice_t;

static i2s_chan_handle_t s_i2s_tx;
static StaticTask_t s_audio_task_tcb, s_input_task_tcb, s_protocol_task_tcb;
static StackType_t s_audio_task_stack[AUDIO_TASK_STACK_WORDS];
static StackType_t s_input_task_stack[INPUT_TASK_STACK_WORDS];
static StackType_t s_protocol_task_stack[PROTOCOL_TASK_STACK_WORDS];
static int16_t s_audio_block[AUDIO_BLOCK_FRAMES * AUDIO_CHANNELS];
static int16_t s_click_lut[CLICK_LUT_SIZE];
static const gpio_num_t s_note_pins[NOTE_COUNT] = {
    BEATBOX_GPIO_S1, BEATBOX_GPIO_S2, BEATBOX_GPIO_S3, BEATBOX_GPIO_S4,
    BEATBOX_GPIO_S5, BEATBOX_GPIO_S6, BEATBOX_GPIO_S7,
};
static const uint8_t s_default_note_midi[NOTE_COUNT] = {60U, 62U, 64U, 65U, 67U, 69U, 71U};
static const rgb_color_t s_default_colors[NOTE_COUNT] = {
    {255U, 0U, 0U, 100U}, {255U, 136U, 0U, 100U}, {255U, 255U, 0U, 100U},
    {0U, 255U, 0U, 100U}, {0U, 170U, 255U, 100U}, {68U, 68U, 255U, 100U},
    {170U, 0U, 255U, 100U},
};
static note_voice_t s_note_voices[NOTE_COUNT], s_remote_voices[REMOTE_VOICE_COUNT];
static volatile bool s_note_requested[NOTE_COUNT], s_remote_requested[REMOTE_VOICE_COUNT];
static volatile uint8_t s_note_midi[NOTE_COUNT], s_note_enabled[NOTE_COUNT], s_remote_midi[REMOTE_VOICE_COUNT];
static volatile uint32_t s_note_phase_steps[NOTE_COUNT], s_remote_phase_steps[REMOTE_VOICE_COUNT];
static volatile uint32_t s_remote_target_amplitude_q16[REMOTE_VOICE_COUNT];
static volatile uint32_t s_note_mapping_generation;
static rgb_color_t s_colors[NOTE_COUNT];
static uint32_t s_config_revision;
static volatile bool s_transport_running, s_start_requested, s_stop_requested;
static volatile uint32_t s_bpm = AUDIO_DEFAULT_BPM, s_volume_percent = AUDIO_DEFAULT_VOLUME;
static bool s_click_enabled = true;
static uint64_t s_sample_cursor_q32, s_next_tick_sample_q32, s_tick_index, s_frames_per_tick_q32;
static uint32_t s_click_phase, s_click_phase_step, s_click_amplitude, s_click_remaining;
static rmt_channel_handle_t s_led_rmt_channel;
static rmt_encoder_handle_t s_led_bytes_encoder;
static uint8_t s_led_frame[WS2812_FRAME_BYTES], s_led_active_mask;
static bool s_led_ready;

static uint32_t midi_note_phase_step(uint8_t midi_note)
{
    const float frequency = 440.0f * powf(2.0f, ((float)midi_note - 69.0f) / 12.0f);
    return (uint32_t)(((uint64_t)(frequency * 4294967296.0f)) / AUDIO_SAMPLE_RATE_HZ);
}

static int32_t scale_audio_sample(int32_t sample)
{
    if (s_volume_percent == AUDIO_MIN_VOLUME) return 0;
    return (int32_t)(((int64_t)sample * s_volume_percent) / AUDIO_DEFAULT_VOLUME);
}

static uint32_t config_crc32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffU;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (uint8_t bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static void config_defaults(void)
{
    s_config_revision = 0U;
    s_bpm = AUDIO_DEFAULT_BPM;
    s_volume_percent = AUDIO_DEFAULT_VOLUME;
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
        s_note_midi[index] = s_default_note_midi[index];
        s_note_enabled[index] = 1U;
        s_note_phase_steps[index] = midi_note_phase_step(s_note_midi[index]);
        s_colors[index] = s_default_colors[index];
    }
}

static void note_mapping_apply(const uint8_t notes[NOTE_COUNT], const uint8_t enabled[NOTE_COUNT])
{
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
        s_note_midi[index] = notes[index];
        s_note_enabled[index] = enabled[index] ? 1U : 0U;
        s_note_phase_steps[index] = midi_note_phase_step(notes[index]);
        s_note_requested[index] = false;
    }
    ++s_note_mapping_generation;
}

static void config_snapshot(persisted_config_t *config, uint32_t revision)
{
    memset(config, 0, sizeof(*config));
    config->version = CONFIG_VERSION;
    config->length = sizeof(*config);
    config->revision = revision;
    config->volume = s_volume_percent;
    config->bpm = s_bpm;
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
        config->notes[index] = s_note_midi[index];
        config->enabled[index] = s_note_enabled[index];
        config->colors[index] = s_colors[index];
    }
    config->crc32 = config_crc32(config, offsetof(persisted_config_t, crc32));
}

static esp_err_t config_save(void)
{
    persisted_config_t config;
    config_snapshot(&config, s_config_revision + 1U);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, CONFIG_KEY, &config, sizeof(config));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) s_config_revision = config.revision;
    return err;
}

static bool config_valid(const persisted_config_t *config, size_t length)
{
    if (length != sizeof(*config) || config->version != CONFIG_VERSION ||
        config->length != sizeof(*config) || config->volume > AUDIO_MAX_VOLUME ||
        config->bpm < AUDIO_MIN_BPM || config->bpm > AUDIO_MAX_BPM ||
        config->crc32 != config_crc32(config, offsetof(persisted_config_t, crc32))) return false;
    for (uint32_t index = 0; index < NOTE_COUNT; ++index)
        if (config->notes[index] < 36U || config->notes[index] > 84U ||
            config->enabled[index] > 1U || config->colors[index].brightness > 100U) return false;
    return true;
}

static esp_err_t config_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    persisted_config_t config;
    size_t length = sizeof(config);
    err = nvs_get_blob(handle, CONFIG_KEY, &config, &length);
    nvs_close(handle);
    if (err != ESP_OK || !config_valid(&config, length)) return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    s_config_revision = config.revision;
    s_bpm = config.bpm;
    s_volume_percent = config.volume;
    note_mapping_apply(config.notes, config.enabled);
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) s_colors[index] = config.colors[index];
    return ESP_OK;
}

static void config_init(void)
{
    config_defaults();
    const esp_err_t err = config_load();
    if (err == ESP_OK) ESP_LOGI(TAG, "loaded configuration revision %" PRIu32, s_config_revision);
    else ESP_LOGI(TAG, "using safe defaults (%s)", esp_err_to_name(err));
}

static void led_build_frame(void)
{
    memset(s_led_frame, 0, sizeof(s_led_frame));
    int32_t shared_key = -1;
    for (uint32_t index = 5U; index < NOTE_COUNT; ++index)
        if ((s_led_active_mask & (1U << index)) != 0U) shared_key = (int32_t)index;
    for (uint32_t index = 0; index < WS2812_COUNT; ++index) {
        rgb_color_t color = {0};
        if (shared_key >= 0) color = s_colors[(uint32_t)shared_key];
        else if ((s_led_active_mask & (1U << index)) != 0U) color = s_colors[index];
        s_led_frame[index * 3U] = (uint8_t)(((uint16_t)color.g * color.brightness) / 100U);
        s_led_frame[index * 3U + 1U] = (uint8_t)(((uint16_t)color.r * color.brightness) / 100U);
        s_led_frame[index * 3U + 2U] = (uint8_t)(((uint16_t)color.b * color.brightness) / 100U);
    }
}

static void led_refresh(void)
{
    if (!s_led_ready) return;
    led_build_frame();
    const rmt_transmit_config_t tx_config = {.loop_count = 0};
    if (rmt_transmit(s_led_rmt_channel, s_led_bytes_encoder, s_led_frame, sizeof(s_led_frame), &tx_config) == ESP_OK)
        (void)rmt_tx_wait_all_done(s_led_rmt_channel, -1);
    gpio_set_level(BEATBOX_GPIO_STATUS_LED, (s_led_active_mask & 0x60U) != 0U);
}

static esp_err_t led_init(void)
{
    const gpio_config_t status_config = {.pin_bit_mask = 1ULL << BEATBOX_GPIO_STATUS_LED, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&status_config), TAG, "status LED configure failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BEATBOX_GPIO_STATUS_LED, 0), TAG, "status LED safe state failed");
    const rmt_tx_channel_config_t channel_config = {.gpio_num = BEATBOX_GPIO_WS2812_DATA,
        .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000U, .mem_block_symbols = 64U,
        .trans_queue_depth = 1U, .flags = {.invert_out = false, .with_dma = false}};
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&channel_config, &s_led_rmt_channel), TAG, "WS2812 RMT channel failed");
    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {.duration0 = 3U, .level0 = 1U, .duration1 = 9U, .level1 = 0U},
        .bit1 = {.duration0 = 7U, .level0 = 1U, .duration1 = 5U, .level1 = 0U}, .flags = {.msb_first = 1U}};
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&encoder_config, &s_led_bytes_encoder), TAG, "WS2812 encoder failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_led_rmt_channel), TAG, "WS2812 RMT enable failed");
    s_led_ready = true;
    led_refresh();
    return ESP_OK;
}

static uint64_t frames_per_tick_q32(uint32_t bpm)
{
    return (((uint64_t)AUDIO_SAMPLE_RATE_HZ * 60ULL) << 32) / ((uint64_t)bpm * AUDIO_PPQN);
}

static void transport_start_at_block_boundary(void)
{
    s_sample_cursor_q32 = 0; s_next_tick_sample_q32 = 0; s_tick_index = 0;
    s_frames_per_tick_q32 = frames_per_tick_q32(s_bpm); s_click_phase = 0; s_click_remaining = 0;
    s_transport_running = true;
}

static void transport_stop_at_block_boundary(void)
{
    s_transport_running = false; s_click_remaining = 0;
}

static uint32_t click_phase_step(uint32_t frequency_hz)
{
    return (uint32_t)(((uint64_t)frequency_hz << 32) / AUDIO_SAMPLE_RATE_HZ);
}

static void click_prepare_lut(void)
{
    for (uint32_t index = 0; index < CLICK_LUT_SIZE; ++index)
        s_click_lut[index] = (int16_t)(sinf((6.28318530717958647692f * index) / CLICK_LUT_SIZE) * 32767.0f);
}

static void click_trigger(uint64_t tick_index)
{
    if (!s_click_enabled || (tick_index % AUDIO_PPQN) != 0U) return;
    const bool downbeat = (tick_index % (AUDIO_PPQN * 4U)) == 0U;
    s_click_phase = 0; s_click_phase_step = click_phase_step(downbeat ? 2600U : 1900U);
    s_click_amplitude = downbeat ? 22000U : 16000U; s_click_remaining = CLICK_LENGTH_FRAMES;
}

static int16_t click_render_sample(void)
{
    if (s_click_remaining == 0U) return 0;
    const int32_t fundamental = ((int32_t)s_click_lut[s_click_phase >> 24] * (int32_t)s_click_amplitude) >> 15;
    const int32_t harmonic = ((int32_t)s_click_lut[(s_click_phase >> 23) & 0xffU] * (int32_t)(s_click_amplitude >> 1)) >> 15;
    int32_t sample = fundamental + harmonic;
    if (sample > INT16_MAX) sample = INT16_MAX;
    if (sample < INT16_MIN) sample = INT16_MIN;
    s_click_phase += s_click_phase_step; s_click_amplitude = (s_click_amplitude * 245U) >> 8; --s_click_remaining;
    return (int16_t)sample;
}

static int32_t note_render_mix_sample(bool render_physical, bool render_remote)
{
    const uint32_t target = NOTE_VOICE_AMPLITUDE << 16;
    const uint32_t attack = target / NOTE_ATTACK_FRAMES, release = target / NOTE_RELEASE_FRAMES;
    int32_t mix = 0;
    if (render_physical) for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
        note_voice_t *voice = &s_note_voices[index]; const bool requested = s_note_requested[index];
        if (requested && !voice->active) { voice->phase = 0; voice->phase_step = s_note_phase_steps[index]; voice->amplitude_q16 = 0; voice->active = true; }
        if (!voice->active) continue;
        voice->phase_step = s_note_phase_steps[index];
        if (requested) { const uint32_t remaining = target - voice->amplitude_q16; voice->amplitude_q16 += remaining < attack ? remaining : attack; }
        else if (voice->amplitude_q16 <= release) { voice->amplitude_q16 = 0; voice->active = false; continue; }
        else voice->amplitude_q16 -= release;
        mix += (int32_t)(((int64_t)s_click_lut[voice->phase >> 24] * voice->amplitude_q16) >> 16); voice->phase += voice->phase_step;
    }
    if (render_remote) for (uint32_t index = 0; index < REMOTE_VOICE_COUNT; ++index) {
        note_voice_t *voice = &s_remote_voices[index]; const bool requested = s_remote_requested[index];
        const uint32_t remote_target = s_remote_target_amplitude_q16[index], remote_attack = remote_target / NOTE_ATTACK_FRAMES;
        if (requested && !voice->active) { voice->phase = 0; voice->phase_step = s_remote_phase_steps[index]; voice->amplitude_q16 = 0; voice->active = true; }
        if (!voice->active) continue;
        if (requested) { const uint32_t remaining = remote_target - voice->amplitude_q16; voice->amplitude_q16 += remaining < remote_attack ? remaining : remote_attack; }
        else if (voice->amplitude_q16 <= release) { voice->amplitude_q16 = 0; voice->active = false; continue; }
        else voice->amplitude_q16 -= release;
        mix += (int32_t)(((int64_t)s_click_lut[voice->phase >> 24] * voice->amplitude_q16) >> 16); voice->phase += voice->phase_step;
    }
    return mix;
}

static void click_advance_sample_cursor(void)
{
    while (s_sample_cursor_q32 >= s_next_tick_sample_q32) { click_trigger(s_tick_index++); s_next_tick_sample_q32 += s_frames_per_tick_q32; }
    s_sample_cursor_q32 += 1ULL << 32;
}

static void beatbox_audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_start_requested) { s_start_requested = false; s_stop_requested = false; transport_start_at_block_boundary(); }
        else if (s_stop_requested) { s_stop_requested = false; transport_stop_at_block_boundary(); }
        if (s_transport_running) s_frames_per_tick_q32 = frames_per_tick_q32(s_bpm);
        for (uint32_t frame = 0; frame < AUDIO_BLOCK_FRAMES; ++frame) {
            int32_t sample_value = note_render_mix_sample(true, true);
            if (s_transport_running) { sample_value += click_render_sample(); click_advance_sample_cursor(); }
            sample_value = scale_audio_sample(sample_value);
            if (sample_value > INT16_MAX) sample_value = INT16_MAX;
            if (sample_value < INT16_MIN) sample_value = INT16_MIN;
            s_audio_block[frame * 2U] = (int16_t)sample_value;
            s_audio_block[frame * 2U + 1U] = (int16_t)sample_value;
        }
        size_t bytes_written = 0;
        if (i2s_channel_write(s_i2s_tx, s_audio_block, sizeof(s_audio_block), &bytes_written, portMAX_DELAY) != ESP_OK || bytes_written != sizeof(s_audio_block)) vTaskDelete(NULL);
    }
}

static esp_err_t beatbox_audio_start(void)
{
    click_prepare_lut(); s_transport_running = false; s_start_requested = false; s_stop_requested = false; s_click_enabled = true;
    s_frames_per_tick_q32 = frames_per_tick_q32(s_bpm);
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER); channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_i2s_tx, NULL), TAG, "I2S channel failed");
    const i2s_std_config_t std_config = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = BEATBOX_GPIO_SPK_BCLK, .ws = BEATBOX_GPIO_SPK_WS,
                     .dout = BEATBOX_GPIO_SPK_DOUT, .din = I2S_GPIO_UNUSED, .invert_flags = {false, false, false}}};
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_config), TAG, "I2S init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S enable failed");
    if (xTaskCreateStatic(beatbox_audio_task, "beatbox_audio", AUDIO_TASK_STACK_WORDS, NULL, 5, s_audio_task_stack, &s_audio_task_tcb) == NULL) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static esp_err_t beatbox_input_init(void)
{
    uint64_t input_mask = (1ULL << BEATBOX_GPIO_S8) | (1ULL << BEATBOX_GPIO_ENCODER_A) | (1ULL << BEATBOX_GPIO_ENCODER_B) | (1ULL << BEATBOX_GPIO_ENCODER_PRESS);
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) input_mask |= 1ULL << s_note_pins[index];
    const gpio_config_t input_config = {.pin_bit_mask = input_mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    return gpio_config(&input_config);
}

static void transport_toggle_request(void) { if (s_transport_running) s_stop_requested = true; else s_start_requested = true; }

static bool parse_uint_array(const char *line, const char *field, uint32_t count, uint32_t minimum, uint32_t maximum, uint32_t *values)
{
    const char *cursor = strstr(line, field); if (cursor == NULL) return false; cursor = strchr(cursor, '['); if (cursor == NULL) return false;
    for (uint32_t index = 0; index < count; ++index) { char *end = NULL; const unsigned long value = strtoul(cursor + 1, &end, 10); if (end == cursor + 1 || value < minimum || value > maximum) return false; values[index] = (uint32_t)value; cursor = end; if (index + 1U < count && *cursor != ',') return false; }
    return strchr(cursor, ']') != NULL;
}
static bool parse_notes(const char *line, uint8_t notes[NOTE_COUNT])
{
    uint32_t values[NOTE_COUNT]; if (!parse_uint_array(line, "\"notes\"", NOTE_COUNT, 36U, 84U, values)) return false;
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
        notes[index] = (uint8_t)values[index];
    }
    return true;
}
static bool parse_enabled(const char *line, uint8_t enabled[NOTE_COUNT])
{
    uint32_t values[NOTE_COUNT]; if (strstr(line, "\"enabled\"") == NULL) { memset(enabled, 1, NOTE_COUNT); return true; }
    if (!parse_uint_array(line, "\"enabled\"", NOTE_COUNT, 0U, 1U, values)) return false;
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
        enabled[index] = (uint8_t)values[index];
    }
    return true;
}
static uint8_t hex_digit(char value)
{
    if (value >= '0' && value <= '9') return (uint8_t)(value - '0');
    if (value >= 'a' && value <= 'f') return (uint8_t)(value - 'a' + 10);
    if (value >= 'A' && value <= 'F') return (uint8_t)(value - 'A' + 10);
    return 255U;
}
static bool parse_colors(const char *line, rgb_color_t colors[NOTE_COUNT])
{
    const char *cursor = strstr(line, "\"colors\""); if (cursor == NULL) return false; cursor = strchr(cursor, '['); if (cursor == NULL) return false;
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) { cursor = strchr(cursor, '#'); if (cursor == NULL) return false; const uint8_t r1 = hex_digit(cursor[1]), r2 = hex_digit(cursor[2]), g1 = hex_digit(cursor[3]), g2 = hex_digit(cursor[4]), b1 = hex_digit(cursor[5]), b2 = hex_digit(cursor[6]); if (r1 > 15U || r2 > 15U || g1 > 15U || g2 > 15U || b1 > 15U || b2 > 15U) return false; colors[index] = (rgb_color_t){(uint8_t)((r1 << 4U) | r2), (uint8_t)((g1 << 4U) | g2), (uint8_t)((b1 << 4U) | b2), 100U}; cursor += 7; }
    return true;
}
static bool parse_brightness(const char *line, rgb_color_t colors[NOTE_COUNT])
{
    uint32_t values[NOTE_COUNT]; if (strstr(line, "\"brightness\"") == NULL) return true; if (!parse_uint_array(line, "\"brightness\"", NOTE_COUNT, 0U, 100U, values)) return false;
    for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
        colors[index].brightness = (uint8_t)values[index];
    }
    return true;
}
static bool parse_uint_field(const char *line, const char *field, uint32_t minimum, uint32_t maximum, uint32_t *value_out)
{
    const char *cursor = strstr(line, field); if (cursor == NULL) return false; cursor = strchr(cursor, ':'); if (cursor == NULL) return false; char *end = NULL; const unsigned long value = strtoul(cursor + 1, &end, 10); if (end == cursor + 1 || value < minimum || value > maximum) return false; *value_out = (uint32_t)value; return true;
}

static void protocol_send_state(void)
{
    printf("{\"t\":\"state\",\"notes\":[%u,%u,%u,%u,%u,%u,%u],\"enabled\":[%u,%u,%u,%u,%u,%u,%u],\"colors\":[\"#%02x%02x%02x\",\"#%02x%02x%02x\",\"#%02x%02x%02x\",\"#%02x%02x%02x\",\"#%02x%02x%02x\",\"#%02x%02x%02x\",\"#%02x%02x%02x\"],\"brightness\":[%u,%u,%u,%u,%u,%u,%u],\"volume\":%u,\"bpm\":%u,\"run\":%u,\"playing\":%s,\"click\":%s,\"rev\":%" PRIu32 "}\n",
           s_note_midi[0], s_note_midi[1], s_note_midi[2], s_note_midi[3], s_note_midi[4], s_note_midi[5], s_note_midi[6], s_note_enabled[0], s_note_enabled[1], s_note_enabled[2], s_note_enabled[3], s_note_enabled[4], s_note_enabled[5], s_note_enabled[6], s_colors[0].r, s_colors[0].g, s_colors[0].b, s_colors[1].r, s_colors[1].g, s_colors[1].b, s_colors[2].r, s_colors[2].g, s_colors[2].b, s_colors[3].r, s_colors[3].g, s_colors[3].b, s_colors[4].r, s_colors[4].g, s_colors[4].b, s_colors[5].r, s_colors[5].g, s_colors[5].b, s_colors[6].r, s_colors[6].g, s_colors[6].b, s_colors[0].brightness, s_colors[1].brightness, s_colors[2].brightness, s_colors[3].brightness, s_colors[4].brightness, s_colors[5].brightness, s_colors[6].brightness, (unsigned)s_volume_percent, (unsigned)s_bpm, s_transport_running ? 1U : 0U, s_transport_running ? "true" : "false", s_click_enabled ? "true" : "false", s_config_revision);
    fflush(stdout);
}
static void protocol_send_mapping_ack(bool ok, bool saved)
{
    printf("{\"t\":\"mapping_ack\",\"ok\":%u,\"saved\":%u,\"notes\":[%u,%u,%u,%u,%u,%u,%u],\"rev\":%" PRIu32 "}\n", ok ? 1U : 0U, saved ? 1U : 0U, s_note_midi[0], s_note_midi[1], s_note_midi[2], s_note_midi[3], s_note_midi[4], s_note_midi[5], s_note_midi[6], s_config_revision);
    fflush(stdout);
}
static void protocol_send_config_result(bool ok, bool saved)
{
    if (ok) printf("{\"t\":\"config_ack\",\"ok\":1,\"saved\":%u,\"rev\":%" PRIu32 "}\n", saved ? 1U : 0U, s_config_revision);
    else printf("{\"t\":\"config_error\",\"ok\":0,\"reason\":\"nvs_write_failed\"}\n");
    fflush(stdout);
}

static bool parse_note_message(const char *line, bool *is_note_on, uint8_t *midi_note, uint8_t *velocity)
{
    const char *type = strstr(line, "\"t\":\"note_"); const char *note = strstr(line, "\"n\""); if (type == NULL || note == NULL) return false;
    *is_note_on = strncmp(type + 5, "note_on", 7) == 0; if (!*is_note_on && strncmp(type + 5, "note_off", 8) != 0) return false;
    char *end = NULL; const unsigned long parsed_note = strtoul(strchr(note, ':') + 1, &end, 10); if (end == NULL || parsed_note < 36UL || parsed_note > 84UL) return false; *midi_note = (uint8_t)parsed_note; *velocity = 100U;
    if (*is_note_on && strstr(line, "\"v\"") != NULL) { const char *value = strstr(line, "\"v\""); const unsigned long parsed_velocity = strtoul(strchr(value, ':') + 1, &end, 10); if (parsed_velocity < 1UL || parsed_velocity > 127UL) return false; *velocity = (uint8_t)parsed_velocity; }
    return true;
}
static void remote_note_on(uint8_t midi_note, uint8_t velocity)
{
    int32_t free_voice = -1;
    for (uint32_t index = 0; index < REMOTE_VOICE_COUNT; ++index) { if (s_remote_requested[index] && s_remote_midi[index] == midi_note) { s_remote_target_amplitude_q16[index] = ((NOTE_VOICE_AMPLITUDE << 16) * velocity) / 127U; return; } if (free_voice < 0 && !s_remote_requested[index] && !s_remote_voices[index].active) free_voice = (int32_t)index; }
    if (free_voice < 0) return;
    const uint32_t index = (uint32_t)free_voice;
    s_remote_midi[index] = midi_note;
    s_remote_phase_steps[index] = midi_note_phase_step(midi_note);
    s_remote_target_amplitude_q16[index] = ((NOTE_VOICE_AMPLITUDE << 16) * velocity) / 127U;
    s_remote_requested[index] = true;
}
static void remote_note_off(uint8_t midi_note) { for (uint32_t index = 0; index < REMOTE_VOICE_COUNT; ++index) if (s_remote_requested[index] && s_remote_midi[index] == midi_note) s_remote_requested[index] = false; }

static esp_err_t beatbox_protocol_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) { usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT(); ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&config), TAG, "USB serial init failed"); }
    usb_serial_jtag_vfs_use_driver(); return ESP_OK;
}

static void beatbox_protocol_task(void *arg)
{
    (void)arg; char line[512];
    for (;;) {
        size_t length = 0; char received = 0; if (usb_serial_jtag_read_bytes(&received, 1, portMAX_DELAY) <= 0) continue;
        while (received != '\n') { if (length < sizeof(line) - 1U) line[length++] = received; else length = 0; if (usb_serial_jtag_read_bytes(&received, 1, portMAX_DELAY) <= 0) break; }
        line[length] = '\0'; if (length == 0U) continue;
        if (strstr(line, "\"t\":\"state_get\"") != NULL) { protocol_send_state(); continue; }
        uint32_t value = 0;
        if (strstr(line, "\"t\":\"tempo\"") != NULL && parse_uint_field(line, "\"bpm\"", AUDIO_MIN_BPM, AUDIO_MAX_BPM, &value)) { s_bpm = value; printf("{\"t\":\"tempo_ack\",\"bpm\":%u}\n", (unsigned)s_bpm); fflush(stdout); continue; }
        if ((strstr(line, "\"t\":\"volume_set\"") != NULL || strstr(line, "\"t\":\"volume\"") != NULL) && parse_uint_field(line, "\"value\"", AUDIO_MIN_VOLUME, AUDIO_MAX_VOLUME, &value)) { s_volume_percent = value; const esp_err_t save_err = config_save(); printf("{\"t\":\"volume_ack\",\"value\":%u,\"rev\":%" PRIu32 "}\n", (unsigned)s_volume_percent, s_config_revision); fflush(stdout); if (save_err != ESP_OK) protocol_send_config_result(false, false); continue; }
        if (strstr(line, "\"t\":\"click\"") != NULL) { if (strstr(line, "\"on\":true") != NULL) s_click_enabled = true; if (strstr(line, "\"on\":false") != NULL) s_click_enabled = false; continue; }
        if (strstr(line, "\"t\":\"transport\",\"cmd\":\"start\"") != NULL) { s_start_requested = true; continue; }
        if (strstr(line, "\"t\":\"transport\",\"cmd\":\"stop\"") != NULL) { s_stop_requested = true; continue; }
        bool is_note_on = false; uint8_t midi_note = 0, velocity = 100U;
        if (parse_note_message(line, &is_note_on, &midi_note, &velocity)) { if (is_note_on) remote_note_on(midi_note, velocity); else remote_note_off(midi_note); continue; }
        uint8_t notes[NOTE_COUNT], enabled[NOTE_COUNT]; rgb_color_t colors[NOTE_COUNT];
        const bool has_notes = parse_notes(line, notes), has_enabled = has_notes && parse_enabled(line, enabled);
        if ((strstr(line, "\"t\":\"mapping_apply\"") != NULL || strstr(line, "\"t\":\"mapping_save\"") != NULL || strstr(line, "\"t\":\"pitch_map\"") != NULL) && has_notes && has_enabled) {
            note_mapping_apply(notes, enabled); const bool save = strstr(line, "\"t\":\"mapping_save\"") != NULL || strstr(line, "\"t\":\"pitch_map\"") != NULL; const esp_err_t save_err = save ? config_save() : ESP_OK; protocol_send_mapping_ack(save_err == ESP_OK, save && save_err == ESP_OK); if (save && save_err != ESP_OK) protocol_send_config_result(false, false); continue;
        }
        if ((strstr(line, "\"t\":\"led_apply\"") != NULL || strstr(line, "\"t\":\"led_save\"") != NULL) && parse_colors(line, colors) && parse_brightness(line, colors)) {
            memcpy(s_colors, colors, sizeof(s_colors)); led_refresh(); const bool save = strstr(line, "\"t\":\"led_save\"") != NULL; const esp_err_t save_err = save ? config_save() : ESP_OK; protocol_send_config_result(save_err == ESP_OK, save && save_err == ESP_OK); continue;
        }
    }
}

static void input_note_led(uint32_t index, bool active) { if (active) s_led_active_mask |= (uint8_t)(1U << index); else s_led_active_mask &= (uint8_t)~(1U << index); led_refresh(); }

static void beatbox_input_task(void *arg)
{
    (void)arg; bool s8_pressed = false, encoder_pressed = false, note_pressed[NOTE_COUNT] = {false}; uint8_t note_active_midi[NOTE_COUNT] = {0}; uint8_t s8_stable = 0, encoder_stable = 0, note_stable[NOTE_COUNT] = {0}; uint32_t s8_age = 0, encoder_age = 0, generation = s_note_mapping_generation; uint8_t encoder_state = (uint8_t)((gpio_get_level(BEATBOX_GPIO_ENCODER_A) << 1) | gpio_get_level(BEATBOX_GPIO_ENCODER_B)); int8_t quadrature = 0;
    static const int8_t transitions[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
    for (;;) {
        const bool s8_down = gpio_get_level(BEATBOX_GPIO_S8) == 0, encoder_down = gpio_get_level(BEATBOX_GPIO_ENCODER_PRESS) == 0; bool play_stop = false;
        if (generation != s_note_mapping_generation) { generation = s_note_mapping_generation; memset(note_pressed, 0, sizeof(note_pressed)); memset(note_stable, 0, sizeof(note_stable)); s_led_active_mask &= 0x60U; led_refresh(); }
        for (uint32_t index = 0; index < NOTE_COUNT; ++index) {
            const bool down = gpio_get_level(s_note_pins[index]) == 0;
            if (down == note_pressed[index]) note_stable[index] = 0;
            else if (++note_stable[index] >= INPUT_DEBOUNCE_POLLS) { note_stable[index] = 0; note_pressed[index] = down; s_note_requested[index] = down && s_note_enabled[index]; input_note_led(index, down); if (down) note_active_midi[index] = s_note_midi[index]; printf(down ? "{\"t\":\"hw_note_on\",\"k\":%u,\"n\":%u,\"v\":100,\"volume\":%u}\n" : "{\"t\":\"hw_note_off\",\"k\":%u,\"n\":%u,\"volume\":%u}\n", (unsigned)(index + 1U), (unsigned)note_active_midi[index], (unsigned)s_volume_percent); fflush(stdout); }
        }
        if (s8_down == s8_pressed) s8_stable = 0; else if (++s8_stable >= INPUT_DEBOUNCE_POLLS) { s8_stable = 0; if (s8_down) { s8_pressed = true; s8_age = 0; } else { s8_pressed = false; if (s8_age <= 1000U / INPUT_POLL_MS) play_stop = true; } }
        if (encoder_down == encoder_pressed) encoder_stable = 0; else if (++encoder_stable >= INPUT_DEBOUNCE_POLLS) { encoder_stable = 0; if (encoder_down) { encoder_pressed = true; encoder_age = 0; } else { encoder_pressed = false; if (encoder_age <= 1000U / INPUT_POLL_MS) play_stop = true; } }
        if (s8_pressed) ++s8_age;
        if (encoder_pressed) ++encoder_age;
        const uint8_t new_state = (uint8_t)((gpio_get_level(BEATBOX_GPIO_ENCODER_A) << 1) | gpio_get_level(BEATBOX_GPIO_ENCODER_B)); quadrature += transitions[(encoder_state << 2) | new_state]; encoder_state = new_state;
        if (quadrature >= 4 || quadrature <= -4) { int32_t next = (int32_t)s_volume_percent + (quadrature > 0 ? (int32_t)VOLUME_STEP_PERCENT : -(int32_t)VOLUME_STEP_PERCENT); if (next < 0) next = 0; if (next > 100) next = 100; s_volume_percent = (uint32_t)next; const esp_err_t save_err = config_save(); printf("{\"t\":\"volume\",\"value\":%u,\"saved\":%u,\"rev\":%" PRIu32 "}\n", (unsigned)s_volume_percent, save_err == ESP_OK ? 1U : 0U, s_config_revision); fflush(stdout); quadrature = 0; }
        if (play_stop) transport_toggle_request();
        vTaskDelay(INPUT_POLL_TICKS);
    }
}

static esp_err_t beatbox_shared_power_cold_start(void)
{
    const gpio_config_t power_config = {.pin_bit_mask = 1ULL << BEATBOX_GPIO_SHARED_POWER, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_set_level(BEATBOX_GPIO_SHARED_POWER, 0), TAG, "power preload failed"); ESP_RETURN_ON_ERROR(gpio_config(&power_config), TAG, "power config failed");
    const uint64_t command_mask = (1ULL << BEATBOX_GPIO_MIC_BCLK) | (1ULL << BEATBOX_GPIO_MIC_WS) | (1ULL << BEATBOX_GPIO_WS2812_DATA) | (1ULL << BEATBOX_GPIO_SPK_WS) | (1ULL << BEATBOX_GPIO_SPK_BCLK) | (1ULL << BEATBOX_GPIO_SPK_DOUT);
    const gpio_config_t command_config = {.pin_bit_mask = command_mask, .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    for (gpio_num_t pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if ((command_mask & (1ULL << pin)) != 0U) gpio_set_level(pin, 0);
    }
    ESP_RETURN_ON_ERROR(gpio_config(&command_config), TAG, "command pins config failed");
    const gpio_config_t mic_config = {.pin_bit_mask = 1ULL << BEATBOX_GPIO_MIC_DATA, .mode = GPIO_MODE_DISABLE, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&mic_config), TAG, "mic floating config failed"); ESP_RETURN_ON_ERROR(gpio_set_level(BEATBOX_GPIO_SHARED_POWER, 1), TAG, "power enable failed"); vTaskDelay(pdMS_TO_TICKS(BEATBOX_SHARED_POWER_SETTLE_MS)); return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = beatbox_shared_power_cold_start(); if (err != ESP_OK) { ESP_LOGE(TAG, "power start failed: %s", esp_err_to_name(err)); return; }
    err = nvs_flash_init(); if (err != ESP_OK) ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err)); config_init();
    err = led_init(); if (err != ESP_OK) ESP_LOGE(TAG, "LED init failed: %s", esp_err_to_name(err));
    err = beatbox_audio_start(); if (err != ESP_OK) { ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(err)); return; }
    err = beatbox_input_init(); if (err != ESP_OK) { ESP_LOGE(TAG, "input init failed: %s", esp_err_to_name(err)); return; }
    if (xTaskCreateStatic(beatbox_input_task, "beatbox_input", INPUT_TASK_STACK_WORDS, NULL, 4, s_input_task_stack, &s_input_task_tcb) == NULL) return;
    err = beatbox_protocol_init(); if (err != ESP_OK) ESP_LOGE(TAG, "USB protocol disabled: %s", esp_err_to_name(err));
    if (xTaskCreateStatic(beatbox_protocol_task, "beatbox_protocol", PROTOCOL_TASK_STACK_WORDS, NULL, 2, s_protocol_task_stack, &s_protocol_task_tcb) == NULL) return;
    ESP_LOGI(TAG, "stopped at %" PRIu32 " BPM, volume %" PRIu32 "%%, revision %" PRIu32, s_bpm, s_volume_percent, s_config_revision);
}
