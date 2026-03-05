#include "webp_decoder.h"

#include <cstring>
#include <vector>

#include <esp_log.h>
#include <webp/decode.h>
#include <webp/demux.h>

static const char* TAG = "webp_decoder";

struct WebpDecoder::Impl {
    // Common
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    WebpDecoderInfo info = {};

    // Animated
    WebPAnimDecoder* anim_decoder = nullptr;
    WebPData webp_data = {nullptr, 0};
    int last_timestamp = 0;
    uint32_t current_frame_delay_ms = 0;

    // Static
    std::vector<uint8_t> still_rgba;

    ~Impl() {
        if (anim_decoder) {
            WebPAnimDecoderDelete(anim_decoder);
        }
    }
};

WebpDecoder::WebpDecoder() = default;
WebpDecoder::~WebpDecoder() = default;
WebpDecoder::WebpDecoder(WebpDecoder&&) noexcept = default;
WebpDecoder& WebpDecoder::operator=(WebpDecoder&&) noexcept = default;

esp_err_t WebpDecoder::init(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        ESP_LOGE(TAG, "No WebP data");
        return ESP_ERR_INVALID_ARG;
    }

    auto p = std::make_unique<Impl>();
    p->data = data;
    p->data_size = size;

    // Detect static vs animated using WebPGetFeatures
    WebPBitstreamFeatures features;
    VP8StatusCode status = WebPGetFeatures(data, size, &features);
    if (status != VP8_STATUS_OK) {
        ESP_LOGE(TAG, "WebPGetFeatures failed: %d", status);
        return ESP_ERR_INVALID_ARG;
    }

    p->info.canvas_width = static_cast<uint32_t>(features.width);
    p->info.canvas_height = static_cast<uint32_t>(features.height);
    p->info.has_alpha = features.has_alpha != 0;
    p->info.is_animated = features.has_animation != 0;

    if (p->info.canvas_width == 0 || p->info.canvas_height == 0) {
        ESP_LOGE(TAG, "Invalid dimensions: %ux%u",
                 p->info.canvas_width, p->info.canvas_height);
        return ESP_ERR_INVALID_ARG;
    }

    if (p->info.is_animated) {
        // Animated path: use WebPAnimDecoder
        p->webp_data.bytes = data;
        p->webp_data.size = size;

        WebPAnimDecoderOptions opts;
        WebPAnimDecoderOptionsInit(&opts);
        opts.color_mode = MODE_RGBA;

        p->anim_decoder = WebPAnimDecoderNew(&p->webp_data, &opts);
        if (!p->anim_decoder) {
            ESP_LOGE(TAG, "Failed to create anim decoder");
            return ESP_FAIL;
        }

        WebPAnimInfo anim_info;
        if (!WebPAnimDecoderGetInfo(p->anim_decoder, &anim_info)) {
            ESP_LOGE(TAG, "Failed to get anim info");
            return ESP_FAIL;
        }

        p->info.frame_count = anim_info.frame_count;
        p->last_timestamp = 0;
        p->current_frame_delay_ms = 0;

        ESP_LOGI(TAG, "Animated: %u frames, %ux%u",
                 p->info.frame_count, p->info.canvas_width,
                 p->info.canvas_height);
    } else {
        // Static path: pre-decode into buffer
        p->info.frame_count = 1;

        size_t frame_size = static_cast<size_t>(p->info.canvas_width) *
                            p->info.canvas_height * 4;
        p->still_rgba.resize(frame_size);

        if (!WebPDecodeRGBAInto(data, size, p->still_rgba.data(),
                                frame_size,
                                static_cast<int>(p->info.canvas_width * 4))) {
            ESP_LOGE(TAG, "Failed to decode static WebP");
            return ESP_FAIL;
        }

        p->current_frame_delay_ms = 0;

        ESP_LOGI(TAG, "Static: %ux%u",
                 p->info.canvas_width, p->info.canvas_height);
    }

    impl_ = std::move(p);
    return ESP_OK;
}

WebpDecoderInfo WebpDecoder::get_info() const {
    if (!impl_) return {};
    return impl_->info;
}

esp_err_t WebpDecoder::get_next_frame(uint8_t* rgba_out) {
    if (!impl_ || !rgba_out) return ESP_ERR_INVALID_STATE;

    if (!impl_->info.is_animated) {
        // Static: just memcpy the pre-decoded buffer
        size_t frame_size = static_cast<size_t>(impl_->info.canvas_width) *
                            impl_->info.canvas_height * 4;
        memcpy(rgba_out, impl_->still_rgba.data(), frame_size);
        impl_->current_frame_delay_ms = 0;
        return ESP_OK;
    }

    // Animated: auto-loop
    if (!WebPAnimDecoderHasMoreFrames(impl_->anim_decoder)) {
        WebPAnimDecoderReset(impl_->anim_decoder);
        impl_->last_timestamp = 0;
    }

    uint8_t* pix = nullptr;
    int timestamp = 0;
    if (!WebPAnimDecoderGetNext(impl_->anim_decoder, &pix, &timestamp)) {
        ESP_LOGE(TAG, "WebPAnimDecoderGetNext failed");
        return ESP_FAIL;
    }

    size_t frame_size = static_cast<size_t>(impl_->info.canvas_width) *
                        impl_->info.canvas_height * 4;
    memcpy(rgba_out, pix, frame_size);

    int delay = timestamp - impl_->last_timestamp;
    impl_->current_frame_delay_ms =
        static_cast<uint32_t>(delay > 0 ? delay : 1);
    impl_->last_timestamp = timestamp;

    return ESP_OK;
}

uint32_t WebpDecoder::get_frame_delay() const {
    if (!impl_) return 0;
    return impl_->current_frame_delay_ms;
}

esp_err_t WebpDecoder::reset() {
    if (!impl_) return ESP_ERR_INVALID_STATE;

    if (impl_->info.is_animated && impl_->anim_decoder) {
        WebPAnimDecoderReset(impl_->anim_decoder);
    }
    impl_->last_timestamp = 0;
    impl_->current_frame_delay_ms = 0;
    return ESP_OK;
}

bool WebpDecoder::is_valid() const {
    return impl_ != nullptr;
}
