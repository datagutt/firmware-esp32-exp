#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include "esp_err.h"

struct WebpDecoderInfo {
    uint32_t canvas_width = 0;
    uint32_t canvas_height = 0;
    uint32_t frame_count = 0;
    bool has_alpha = false;
    bool is_animated = false;
};

class WebpDecoder {
public:
    WebpDecoder();
    ~WebpDecoder();

    WebpDecoder(const WebpDecoder&) = delete;
    WebpDecoder& operator=(const WebpDecoder&) = delete;
    WebpDecoder(WebpDecoder&&) noexcept;
    WebpDecoder& operator=(WebpDecoder&&) noexcept;

    /// Initialize from WebP data. Data must remain valid for lifetime of decoder.
    esp_err_t init(const uint8_t* data, size_t size);

    /// Get info about the loaded WebP.
    WebpDecoderInfo get_info() const;

    /// Decode the next frame. On success *pixels_out points to the decoded
    /// RGBA canvas (canvas_width * canvas_height * 4 bytes). The buffer is
    /// owned by the decoder and stays valid until the next get_next_frame()
    /// call, reset(), or destruction. Auto-loops for animations.
    esp_err_t get_next_frame(const uint8_t** pixels_out);

    /// Get delay of last decoded frame in ms. 0 for static images.
    uint32_t get_frame_delay() const;

    /// Reset to first frame.
    esp_err_t reset();

    /// Check if decoder is initialized.
    bool is_valid() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
