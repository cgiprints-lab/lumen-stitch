// colorprobe: pixel statistics for Lumen captures.
//
// Prints per-channel percentiles and a luminance histogram for one or more
// images, optionally applying a transfer change first, so capture-path colour
// differences (linear vs display-referred, effect-strip brightness) can be
// measured instead of eyeballed. Also able to write a transformed PNG for a
// visual A/B.
//
//   colorprobe img.png [img2.png ...] [--srgb | --gamma <f> | --gain <f>]
//                      [--center <frac>] [--save <out.png>]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

double srgb_encode(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

double srgb_decode(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

constexpr double kPcts[8] = {0.01, 0.05, 0.25, 0.50, 0.75, 0.95, 0.99, 1.0};

struct Stats {
    double mean[4] = {};
    double pct[4][8] = {};  // 1,5,25,50,75,95,99,100 percentiles
    double lum_mean = 0;
    int hist[16] = {};
    long long n = 0;
};

void report(const char* name, Stats& s) {
    std::printf("%s  (%lld px)\n", name, s.n);
    const char* cn[3] = {"R", "G", "B"};
    for (int c = 0; c < 3; ++c) {
        std::printf("  %s mean=%.3f  p1=%.3f p5=%.3f p25=%.3f med=%.3f p75=%.3f p95=%.3f p99=%.3f\n",
            cn[c], s.mean[c] / 255.0, s.pct[c][0] / 255.0, s.pct[c][1] / 255.0, s.pct[c][2] / 255.0,
            s.pct[c][3] / 255.0, s.pct[c][4] / 255.0, s.pct[c][5] / 255.0, s.pct[c][6] / 255.0);
    }
    std::printf("  lum mean=%.3f  hist[16]:", s.lum_mean / 255.0);
    for (int i = 0; i < 16; ++i)
        std::printf(" %d", s.hist[i]);
    std::printf("\n");
    // Compact "how much of the image is in the bottom quarter of brightness".
    int low = 0;
    for (int i = 0; i < 4; ++i)
        low += s.hist[i];
    std::printf("  dark25%%=%.1f%%  bright75%%=%.1f%%\n", 100.0 * low / s.n,
        100.0 * (s.n - low) / s.n);
}

}  // namespace

int main(int argc, char** argv) {
    enum Mode { MODE_NONE, MODE_SRGB_ENC, MODE_SRGB_DEC, MODE_GAMMA, MODE_GAIN };
    Mode mode = MODE_NONE;
    double param = 2.2;
    std::string save;
    double center = 0.0;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--srgb")
            mode = MODE_SRGB_ENC;
        else if (a == "--srgbdec")
            mode = MODE_SRGB_DEC;
        else if (a == "--gamma" && i + 1 < argc)
            mode = MODE_GAMMA, param = std::atof(argv[++i]);
        else if (a == "--gain" && i + 1 < argc)
            mode = MODE_GAIN, param = std::atof(argv[++i]);
        else if (a == "--center" && i + 1 < argc)
            center = std::atof(argv[++i]);
        else if (a == "--save" && i + 1 < argc)
            save = argv[++i];
        else
            files.push_back(a);
    }
    if (files.empty()) {
        std::printf("usage: colorprobe img... [--srgb|--srgbdec|--gamma f|--gain f] "
                    "[--center frac] [--save out.png]\n");
        return 2;
    }

    for (const auto& f : files) {
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load(f.c_str(), &w, &h, &ch, 3);
        if (!px) {
            std::printf("%s: load failed (%s)\n", f.c_str(), stbi_failure_reason());
            continue;
        }
        int x0 = 0, y0 = 0, x1 = w, y1 = h;
        if (center > 0.0) {
            const int cw = int(w * center), chh = int(h * center);
            x0 = (w - cw) / 2;
            y0 = (h - chh) / 2;
            x1 = x0 + cw;
            y1 = y0 + chh;
        }
        Stats s;
        std::vector<std::vector<unsigned char>> cols(3);
        for (auto& c : cols)
            c.reserve(size_t(w) * size_t(h));
        std::vector<unsigned char> out;
        if (!save.empty())
            out.resize(size_t(w) * size_t(h) * 3);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                unsigned char v[3];
                for (int c = 0; c < 3; ++c) {
                    double d = px[(size_t(y) * size_t(w) + size_t(x)) * 3 + c] / 255.0;
                    if (mode == MODE_SRGB_ENC)
                        d = srgb_encode(d);
                    else if (mode == MODE_SRGB_DEC)
                        d = srgb_decode(d);
                    else if (mode == MODE_GAMMA)
                        d = std::pow(d, 1.0 / param);
                    else if (mode == MODE_GAIN)
                        d = d * param;
                    const int q = int(std::clamp(d, 0.0, 1.0) * 255.0 + 0.5);
                    v[c] = static_cast<unsigned char>(q);
                    cols[c].push_back(static_cast<unsigned char>(q));
                    s.mean[c] += q;
                }
                const double lum = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
                s.lum_mean += lum;
                s.hist[std::min(15, int(lum / 256.0 * 16))]++;
                s.n++;
                if (!save.empty()) {
                    const size_t o = (size_t(y) * size_t(w) + size_t(x)) * 3;
                    out[o] = v[0];
                    out[o + 1] = v[1];
                    out[o + 2] = v[2];
                }
            }
        }
        for (int c = 0; c < 3; ++c) {
            std::sort(cols[c].begin(), cols[c].end());
            for (int k = 0; k < 8; ++k)
                s.pct[c][k] = cols[c][size_t(kPcts[k] * double(cols[c].size() - 1))];
            s.mean[c] /= double(s.n);
        }
        s.lum_mean /= double(s.n);
        report(f.c_str(), s);
        if (!save.empty()) {
            if (stbi_write_png(save.c_str(), w, h, 3, out.data(), w * 3))
                std::printf("  wrote %s\n", save.c_str());
            else
                std::printf("  FAILED to write %s\n", save.c_str());
        }
        stbi_image_free(px);
        px = nullptr;
    }
    return 0;
}
