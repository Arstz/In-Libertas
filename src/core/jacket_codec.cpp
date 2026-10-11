#include "jacket_codec.h"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstring>
#include <algorithm>

std::uint8_t* JacketEncode(const std::uint8_t* rgba, int width, int height, int stride, std::size_t* size) {
    if (size == nullptr) return nullptr;
    *size = 0;
    if (rgba == nullptr || width <= 0 || height <= 0 || width > 2048 || height > 2048 || stride < width * 4)
        return nullptr;
    WebPConfig config;
    WebPPicture picture;
    if (!WebPConfigInit(&config) || !WebPPictureInit(&picture)) return nullptr;
    config.lossless = 1;
    config.quality = 75;
    config.method = 1;
    config.exact = 1;
    picture.use_argb = 1;
    picture.width = width;
    picture.height = height;
    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite;
    picture.custom_ptr = &writer;
    const bool success = WebPValidateConfig(&config) && WebPPictureImportRGBA(&picture, rgba, stride)
        && WebPEncode(&config, &picture);
    WebPPictureFree(&picture);
    if (!success) {
        WebPMemoryWriterClear(&writer);
        return nullptr;
    }
    if (writer.size < sizeof(WebPBitstreamFeatures)) {
        const std::size_t paddedSize = writer.size + std::max<std::size_t>(8, sizeof(WebPBitstreamFeatures) - writer.size);
        auto* padded = static_cast<std::uint8_t*>(WebPMalloc(paddedSize));
        if (padded == nullptr) {
            WebPMemoryWriterClear(&writer);
            return nullptr;
        }
        std::memcpy(padded, writer.mem, writer.size);
        std::memset(padded + writer.size, 0, paddedSize - writer.size);
        std::memcpy(padded + writer.size, "JUNK", 4);
        const std::uint32_t chunkSize = static_cast<std::uint32_t>(paddedSize - writer.size - 8);
        const std::uint32_t riffSize = static_cast<std::uint32_t>(paddedSize - 8);
        for (int byte = 0; byte < 4; ++byte) {
            padded[writer.size + 4 + byte] = static_cast<std::uint8_t>(chunkSize >> (8 * byte));
            padded[4 + byte] = static_cast<std::uint8_t>(riffSize >> (8 * byte));
        }
        WebPMemoryWriterClear(&writer);
        *size = paddedSize;
        return padded;
    }
    *size = writer.size;
    return writer.mem;
}

std::uint8_t* JacketDecode(const std::uint8_t* data, std::size_t size, int* width, int* height) {
    if (data == nullptr || width == nullptr || height == nullptr || size < 12
        || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WEBP", 4) != 0) return nullptr;
    const std::uint32_t declaredSize = std::uint32_t(data[4]) | (std::uint32_t(data[5]) << 8)
        | (std::uint32_t(data[6]) << 16) | (std::uint32_t(data[7]) << 24);
    WebPBitstreamFeatures features;
    if (std::size_t(declaredSize) + 8 != size || WebPGetFeatures(data, size, &features) != VP8_STATUS_OK
        || features.format != 2 || features.has_animation || features.width <= 0 || features.height <= 0
        || features.width > 2048 || features.height > 2048) return nullptr;
    return WebPDecodeRGBA(data, size, width, height);
}

void JacketFree(void* data) {
    WebPFree(data);
}
