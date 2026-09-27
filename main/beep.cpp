/**
 * beep.cpp
 *
 * Touch feedback tone for Tidbyt Gen 2.
 *
 * The speaker amplifier sits on I2S: BCLK=GPIO12, LRCLK=GPIO13, DOUT=GPIO14,
 * no MCLK and no enable pin (same pinout as the Tidbyt HDK). The HUB75 driver
 * owns I2S1 on the ESP32, so audio uses I2S0.
 */

#include "beep.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <driver/i2s_std.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "beep";

constexpr int SAMPLE_RATE = 16000;
constexpr float AMPLITUDE = 5000.0f;  // out of 32767: a small speaker needs little
constexpr int ATTACK_MS = 3;          // ramps at both ends keep the amp from popping
constexpr int PAD_MS = 20;            // silence around the tone, flushes the DMA

struct Tone {
  uint16_t hz;
  uint16_t ms;
};

// Indexed by beep_kind_t
constexpr Tone TONES[] = {
    {.hz = 2000, .ms = 40},   // BEEP_TAP
    {.hz = 1000, .ms = 120},  // BEEP_HOLD
};
static_assert(sizeof(TONES) / sizeof(TONES[0]) == BEEP_HOLD + 1,
              "TONES must have one entry per beep_kind_t");

TaskHandle_t s_task = nullptr;
i2s_chan_handle_t s_tx = nullptr;
bool s_failed = false;

esp_err_t i2s_setup() {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  // HUB75 already takes most of the DMA-capable RAM
  chan_cfg.dma_desc_num = 4;
  chan_cfg.dma_frame_num = 256;

  esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
  if (err != ESP_OK) {
    return err;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                  I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = GPIO_NUM_12,
              .ws = GPIO_NUM_13,
              .dout = GPIO_NUM_14,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {},
          },
  };
  err = i2s_channel_init_std_mode(s_tx, &std_cfg);
  if (err != ESP_OK) {
    i2s_del_channel(s_tx);
    s_tx = nullptr;
  }
  return err;
}

void play(const Tone& tone) {
  const int pad = SAMPLE_RATE * PAD_MS / 1000;
  const int body = SAMPLE_RATE * tone.ms / 1000;
  const int attack = SAMPLE_RATE * ATTACK_MS / 1000;
  const int frames = pad + body + pad;

  // Stereo frames, zeroed so the padding stays silent
  auto* buf = static_cast<int16_t*>(calloc(frames * 2, sizeof(int16_t)));
  if (buf == nullptr) {
    ESP_LOGW(TAG, "No memory for tone");
    return;
  }

  for (int i = 0; i < body; i++) {
    float envelope = 1.0f - static_cast<float>(i) / body;  // linear decay to zero
    if (i < attack) {
      envelope *= static_cast<float>(i) / attack;
    }
    float phase = 2.0f * static_cast<float>(M_PI) * tone.hz * i / SAMPLE_RATE;
    auto sample = static_cast<int16_t>(AMPLITUDE * envelope * sinf(phase));
    buf[(pad + i) * 2] = sample;
    buf[(pad + i) * 2 + 1] = sample;
  }

  // The clocks only run while a tone plays: the amp idles without BCLK
  if (i2s_channel_enable(s_tx) == ESP_OK) {
    size_t written = 0;
    i2s_channel_write(s_tx, buf, frames * 2 * sizeof(int16_t), &written,
                      pdMS_TO_TICKS(500));
    // Let the DMA drain what it buffered before stopping the clocks
    vTaskDelay(pdMS_TO_TICKS(PAD_MS));
    i2s_channel_disable(s_tx);
  }
  free(buf);
}

void beep_task(void*) {
  while (true) {
    uint32_t kind = 0;
    xTaskNotifyWait(0, UINT32_MAX, &kind, portMAX_DELAY);

    if (s_tx == nullptr && !s_failed) {
      esp_err_t err = i2s_setup();
      if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S setup failed: %s (touch stays silent)",
                 esp_err_to_name(err));
        s_failed = true;
      }
    }
    if (s_tx != nullptr && kind < sizeof(TONES) / sizeof(TONES[0])) {
      play(TONES[kind]);
    }
  }
}

}  // namespace

void beep_play(beep_kind_t kind) {
  if (s_task == nullptr) {
    if (xTaskCreate(beep_task, "beep", 3072, nullptr, 3, &s_task) != pdPASS) {
      s_task = nullptr;
      return;
    }
  }
  xTaskNotify(s_task, static_cast<uint32_t>(kind), eSetValueWithOverwrite);
}
