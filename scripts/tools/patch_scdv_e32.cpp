#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "e32common.h"
#include "e32compressor.h"

using bytes = std::vector<char>;

static bytes read_file(const char *path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(std::string("Unable to open ") + path);
    }

    return bytes(std::istreambuf_iterator<char>(stream), {});
}

static void write_file(const char *path, const bytes &data) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(std::string("Unable to create ") + path);
    }

    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
}

static void replace_checked(bytes &data, std::size_t offset,
    std::initializer_list<std::uint8_t> expected,
    std::initializer_list<std::uint8_t> replacement) {
    if ((expected.size() != replacement.size()) || (offset + expected.size() > data.size())) {
        throw std::runtime_error("Invalid patch range");
    }

    const auto *current = reinterpret_cast<const std::uint8_t *>(data.data() + offset);
    if (!std::equal(expected.begin(), expected.end(), current)) {
        throw std::runtime_error("Base DLL bytes differ at offset " + std::to_string(offset));
    }

    std::copy(replacement.begin(), replacement.end(),
        reinterpret_cast<std::uint8_t *>(data.data() + offset));
}

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: patch_scdv_e32 BASE_DLL SURFACE_STUB OUTPUT_DLL\n";
        return 2;
    }

    try {
        bytes packed = read_file(argv[1]);
        bytes image = DeCompressE32Image(packed);
        bytes surface_stub = read_file(argv[2]);

        if (image.size() < sizeof(E32ImageHeader)) {
            throw std::runtime_error("Base DLL has no complete E32 header");
        }

        auto *header = reinterpret_cast<E32ImageHeader *>(image.data());
        const std::size_t code = header->iCodeOffset;
        if (code + 0x50B0 > image.size()) {
            throw std::runtime_error("Base DLL code section is too small");
        }

        // Extend the existing display-mode dispatch through EColor16MAP and
        // route it through the 32-bit alpha implementation while retaining the
        // requested premultiplied display-mode value.
        replace_checked(image, code + 0x1BDE,
            { 0x0C, 0x2E, 0x00, 0xD1 },
            { 0x0D, 0x2E, 0x00, 0xD8 });
        replace_checked(image, code + 0x1D72,
            { 0x04, 0x48, 0xFE, 0xF7, 0xA0, 0xFB },
            { 0x0E, 0x9B, 0x63, 0x63, 0x7B, 0xE7 });

        // The assembly is deliberately position-independent. Its first block
        // replaces CFbsThirtyTwoBitsDrawDevice::GetInterface; the second block
        // occupies an unused diagnostic-string range 0x2B94 bytes later.
        if (surface_stub.size() != 0x2C34) {
            throw std::runtime_error("Unexpected surface stub size");
        }

        std::copy(surface_stub.begin(), surface_stub.begin() + 0x34,
            image.begin() + code + 0x247C);
        std::copy(surface_stub.begin() + 0x2B94, surface_stub.end(),
            image.begin() + code + 0x5010);

        bytes output = CompressE32Image(image);
        SetE32ImageCrc(output.data());
        write_file(argv[3], output);
    } catch (const std::exception &error) {
        std::cerr << "patch_scdv_e32: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
