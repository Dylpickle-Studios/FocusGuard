// SPDX-License-Identifier: AGPL-3.0-or-later
// Writes the Windows .ico files in data/ from the same renderer the Linux tray
// icons use (src/icon.hpp). The .ico files are committed, so this only needs
// running after the icon changes:
//
//   g++ -O2 -std=c++17 -Isrc tools/make_icons.cpp -lz -o make_icons && ./make_icons data
//
// Each size is stored as a PNG inside the .ico (supported since Windows Vista).
#include <zlib.h>

#include <cstdio>
#include <string>
#include <vector>

#include "icon.hpp"

namespace {

void put32(std::string& s, uint32_t v) {
    for (int shift : {24, 16, 8, 0}) s += (char)(v >> shift);
}

void chunk(std::string& png, const char* type, const std::string& data) {
    put32(png, (uint32_t)data.size());
    std::string body = std::string(type, 4) + data;
    png += body;
    put32(png, (uint32_t)crc32(0, reinterpret_cast<const Bytef*>(body.data()), (uInt)body.size()));
}

std::string png(const std::vector<uint32_t>& px, int size) {
    std::string raw;  // filter byte 0, then RGBA per pixel
    for (int y = 0; y < size; ++y) {
        raw += '\0';
        for (int x = 0; x < size; ++x) {
            const uint32_t p = px[(size_t)y * size + x];
            raw += (char)(p >> 16), raw += (char)(p >> 8), raw += (char)p, raw += (char)(p >> 24);
        }
    }
    uLongf len = compressBound((uLong)raw.size());
    std::string z(len, '\0');
    compress2(reinterpret_cast<Bytef*>(&z[0]), &len, reinterpret_cast<const Bytef*>(raw.data()), (uLong)raw.size(), 9);
    z.resize(len);
    std::string ihdr;
    put32(ihdr, size);
    put32(ihdr, size);
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);  // 8-bit RGBA
    std::string out = "\x89PNG\r\n\x1a\n";
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", "");
    return out;
}

void le16(std::string& s, uint16_t v) { s += (char)v, s += (char)(v >> 8); }
void le32(std::string& s, uint32_t v) { le16(s, (uint16_t)v), le16(s, (uint16_t)(v >> 16)); }

bool write_ico(const std::string& path, fg::icon::Style st, const std::vector<int>& sizes) {
    std::vector<std::string> images;
    for (int s : sizes) images.push_back(png(fg::icon::render(st, s), s));
    std::string ico;
    le16(ico, 0), le16(ico, 1), le16(ico, (uint16_t)sizes.size());  // ICONDIR
    uint32_t offset = 6 + 16 * (uint32_t)sizes.size();
    for (size_t i = 0; i < sizes.size(); ++i) {  // ICONDIRENTRY
        ico += (char)(sizes[i] >= 256 ? 0 : sizes[i]);
        ico += (char)(sizes[i] >= 256 ? 0 : sizes[i]);
        ico += '\0', ico += '\0';
        le16(ico, 1), le16(ico, 32);
        le32(ico, (uint32_t)images[i].size());
        le32(ico, offset);
        offset += (uint32_t)images[i].size();
    }
    for (const std::string& img : images) ico += img;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(ico.data(), 1, ico.size(), f) == ico.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "data";
    const std::vector<int> app = {16, 20, 24, 32, 40, 48, 64, 128, 256}, tray = {16, 20, 24, 32, 40, 48, 64};
    const bool ok = write_ico(dir + "/focusguard.ico", fg::icon::Style::Active, app) &&
                    write_ico(dir + "/focusguard-paused.ico", fg::icon::Style::Paused, tray) &&
                    write_ico(dir + "/focusguard-dryrun.ico", fg::icon::Style::DryRun, tray);
    std::puts(ok ? "wrote the .ico files" : "failed to write the .ico files");
    return ok ? 0 : 1;
}
