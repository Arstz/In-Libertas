#pragma once

#include <cstddef>
#include <cstdint>

#ifdef INLIBERTAS_JACKET_CODEC_BUILD
#define JACKET_CODEC_API __declspec(dllexport)
#else
#define JACKET_CODEC_API __declspec(dllimport)
#endif

extern "C" {
JACKET_CODEC_API std::uint8_t* JacketEncode(const std::uint8_t* rgba, int width, int height,
    int stride, std::size_t* size);
JACKET_CODEC_API std::uint8_t* JacketDecode(const std::uint8_t* data, std::size_t size,
    int* width, int* height);
JACKET_CODEC_API void JacketFree(void* data);
}
