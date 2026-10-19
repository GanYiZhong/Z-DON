#include "png_fast.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(ZDON_HAVE_ZLIB)
#include <zlib.h>
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace png_fast {

#if !defined(ZDON_HAVE_ZLIB)

    bool enabled() {
        return false;
    }
    bool decode(const uint8_t*, size_t, ray::Image&) {
        return false;
    }

#else

    namespace {

        uint32_t be32(const uint8_t* p) {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        }

        constexpr uint32_t chunk_type(char a, char b, char c, char d) {
            return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) |
                   uint32_t(uint8_t(d));
        }

        inline uint8_t paeth(int a, int b, int c) {
            const int pa = std::abs(b - c);
            const int pb = std::abs(a - c);
            const int pc = std::abs(a + b - 2 * c);
            if (pa <= pb && pa <= pc) {
                return static_cast<uint8_t>(a);
            }
            return static_cast<uint8_t>(pb <= pc ? b : c);
        }

        // Undoes one row's filter in place; prev is the previous (already unfiltered) row, or zeros.
        template<int BPP>
        bool unfilter_row(int filter, uint8_t* row, const uint8_t* prev, size_t n) {
            switch (filter) {
            case 0:
                return true;
            case 1:
                for (size_t i = BPP; i < n; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + row[i - BPP]);
                }
                return true;
            case 2:
                for (size_t i = 0; i < n; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + prev[i]);
                }
                return true;
            case 3:
                for (size_t i = 0; i < BPP; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + (prev[i] >> 1));
                }
                for (size_t i = BPP; i < n; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + ((row[i - BPP] + prev[i]) >> 1));
                }
                return true;
            case 4:
                for (size_t i = 0; i < BPP; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + prev[i]); // paeth(0, b, 0) = b
                }
                for (size_t i = BPP; i < n; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + paeth(row[i - BPP], prev[i], prev[i - BPP]));
                }
                return true;
            default:
                return false;
            }
        }

#if defined(__SSE2__)
        // Sub / Avg / Paeth one pixel at a time in 16-bit lanes (the scheme libpng's SSE2 filters
        // use): the same integer arithmetic as the scalar code, several bytes at once.
        template<int BPP>
        inline __m128i load_px(const uint8_t* p) {
            int v = 0;
            std::memcpy(&v, p, BPP);
            return _mm_cvtsi32_si128(v);
        }
        template<int BPP>
        inline void store_px(uint8_t* p, __m128i v) {
            const int x = _mm_cvtsi128_si32(v);
            std::memcpy(p, &x, BPP);
        }

        template<int BPP>
        bool unfilter_row_sse2(int filter, uint8_t* row, const uint8_t* prev, size_t n) {
            const __m128i zero = _mm_setzero_si128();
            if (filter == 1) {
                __m128i a = zero;
                for (size_t i = 0; i < n; i += BPP) {
                    a = _mm_add_epi8(a, load_px<BPP>(row + i));
                    store_px<BPP>(row + i, a);
                }
                return true;
            }
            if (filter == 3) {
                const __m128i one = _mm_set1_epi8(1);
                __m128i a = zero;
                for (size_t i = 0; i < n; i += BPP) {
                    const __m128i b = load_px<BPP>(prev + i);
                    // floor((a + b) / 2): avg_epu8 rounds up, so take the odd bit back off
                    __m128i avg = _mm_avg_epu8(a, b);
                    avg = _mm_sub_epi8(avg, _mm_and_si128(_mm_xor_si128(a, b), one));
                    a = _mm_add_epi8(load_px<BPP>(row + i), avg);
                    store_px<BPP>(row + i, a);
                }
                return true;
            }
            if (filter == 4) {
                __m128i a = zero, c = zero;
                for (size_t i = 0; i < n; i += BPP) {
                    const __m128i b = _mm_unpacklo_epi8(load_px<BPP>(prev + i), zero);
                    const __m128i x = _mm_unpacklo_epi8(load_px<BPP>(row + i), zero);
                    __m128i pa = _mm_sub_epi16(b, c);
                    __m128i pb = _mm_sub_epi16(a, c);
                    __m128i pc = _mm_add_epi16(pa, pb);
                    pa = _mm_max_epi16(pa, _mm_sub_epi16(zero, pa));
                    pb = _mm_max_epi16(pb, _mm_sub_epi16(zero, pb));
                    pc = _mm_max_epi16(pc, _mm_sub_epi16(zero, pc));
                    const __m128i smallest = _mm_min_epi16(pc, _mm_min_epi16(pa, pb));
                    const __m128i is_a = _mm_cmpeq_epi16(smallest, pa);
                    const __m128i is_b = _mm_cmpeq_epi16(smallest, pb);
                    const __m128i b_or_c = _mm_or_si128(_mm_and_si128(is_b, b), _mm_andnot_si128(is_b, c));
                    const __m128i nearest = _mm_or_si128(_mm_and_si128(is_a, a), _mm_andnot_si128(is_a, b_or_c));
                    // modulo 256 per byte; the high bytes stay 0
                    a = _mm_and_si128(_mm_add_epi8(x, nearest), _mm_set1_epi16(0xff));
                    c = b;
                    store_px<BPP>(row + i, _mm_packus_epi16(a, a));
                }
                return true;
            }
            return unfilter_row<BPP>(filter, row, prev, n);
        }
#endif

        bool unfilter_row(int bpp, int filter, uint8_t* row, const uint8_t* prev, size_t n) {
            switch (bpp) {
            case 1:
                return unfilter_row<1>(filter, row, prev, n);
            case 2:
                return unfilter_row<2>(filter, row, prev, n);
#if defined(__SSE2__)
            case 3:
                return unfilter_row_sse2<3>(filter, row, prev, n);
            default:
                return unfilter_row_sse2<4>(filter, row, prev, n);
#else
            case 3:
                return unfilter_row<3>(filter, row, prev, n);
            default:
                return unfilter_row<4>(filter, row, prev, n);
#endif
            }
        }

        struct ZStream {
            z_stream z{};
            bool open = false;
            ~ZStream() {
                if (open) {
                    inflateEnd(&z);
                }
            }
        };

    } // namespace

    bool enabled() {
        static const bool on = [] {
            const char* v = std::getenv("YATAIDON_FASTPNG");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    bool decode(const uint8_t* data, size_t size, ray::Image& image) {
        static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (!data || size < 8 + 25 || std::memcmp(data, sig, 8) != 0) {
            return false;
        }
        size_t pos = 8;
        uint32_t w = 0, h = 0;
        int color = -1;
        int in_n = 0; // channels in the file
        uint8_t palette[256 * 4];
        uint32_t pal_len = 0;
        bool pal_trns = false;
        bool has_trans = false; // gray / RGB with a tRNS colour key
        uint8_t tc[3] = {0, 0, 0};
        bool first = true;
        bool seen_idat = false;
        bool idat_done = false; // a non-IDAT chunk followed the IDATs
        bool iend = false;

        ZStream zs;
        uint8_t* raw = nullptr; // filter byte + row, per row
        size_t raw_len = 0;
        struct Free {
            uint8_t*& p;
            ~Free() { std::free(p); }
        } free_raw{raw};

        while (pos + 12 <= size) {
            const uint32_t len = be32(data + pos);
            const uint32_t type = be32(data + pos + 4);
            if (len > (1u << 30) || pos + 12 + size_t(len) > size) {
                return false;
            }
            const uint8_t* c = data + pos + 8;
            pos += 12 + size_t(len);
            if (first && type != chunk_type('I', 'H', 'D', 'R')) {
                return false;
            }
            if (seen_idat && type != chunk_type('I', 'D', 'A', 'T')) {
                idat_done = true;
            }
            switch (type) {
            case chunk_type('I', 'H', 'D', 'R'): {
                if (!first || len != 13) {
                    return false;
                }
                first = false;
                w = be32(c);
                h = be32(c + 4);
                const int depth = c[8];
                color = c[9];
                if (depth != 8 || c[10] != 0 || c[11] != 0 || c[12] != 0) {
                    return false; // 16-bit / sub-byte, unknown method, interlaced: raylib's decoder
                }
                switch (color) {
                case 0:
                    in_n = 1;
                    break;
                case 2:
                    in_n = 3;
                    break;
                case 3:
                    in_n = 1;
                    break;
                case 4:
                    in_n = 2;
                    break;
                case 6:
                    in_n = 4;
                    break;
                default:
                    return false;
                }
                if (w == 0 || h == 0 || w > (1u << 24) || h > (1u << 24)) {
                    return false;
                }
                // stb_image's own size limit
                if ((1u << 30) / w / uint32_t(color == 3 ? 4 : in_n) < h) {
                    return false;
                }
                break;
            }
            case chunk_type('P', 'L', 'T', 'E'): {
                if (seen_idat || len > 256 * 3 || len % 3 != 0) {
                    return false;
                }
                pal_len = len / 3;
                for (uint32_t i = 0; i < pal_len; ++i) {
                    palette[i * 4 + 0] = c[i * 3 + 0];
                    palette[i * 4 + 1] = c[i * 3 + 1];
                    palette[i * 4 + 2] = c[i * 3 + 2];
                    palette[i * 4 + 3] = 255;
                }
                break;
            }
            case chunk_type('t', 'R', 'N', 'S'): {
                if (seen_idat) {
                    return false;
                }
                if (color == 3) {
                    if (pal_len == 0 || len > pal_len) {
                        return false;
                    }
                    pal_trns = true;
                    for (uint32_t i = 0; i < len; ++i) {
                        palette[i * 4 + 3] = c[i];
                    }
                } else if (color == 0 || color == 2) {
                    if (len != uint32_t(in_n) * 2) {
                        return false;
                    }
                    has_trans = true;
                    for (int k = 0; k < in_n; ++k) {
                        tc[k] = c[k * 2 + 1]; // (16-bit value & 255) for 8-bit images, as stb_image reads it
                    }
                } else {
                    return false; // tRNS with alpha
                }
                break;
            }
            case chunk_type('I', 'D', 'A', 'T'): {
                if (idat_done || (color == 3 && pal_len == 0)) {
                    return false;
                }
                if (!seen_idat) {
                    seen_idat = true;
                    raw_len = size_t(h) * (size_t(w) * size_t(in_n) + 1);
                    raw = static_cast<uint8_t*>(std::malloc(raw_len));
                    if (!raw || inflateInit(&zs.z) != Z_OK) {
                        return false;
                    }
                    zs.open = true;
                    zs.z.next_out = raw;
                    zs.z.avail_out = static_cast<uInt>(raw_len);
                }
                if (zs.z.avail_out == 0 || len == 0) {
                    break; // all pixels are in; stb_image ignores what follows too
                }
                zs.z.next_in = const_cast<Bytef*>(c);
                zs.z.avail_in = len;
                while (zs.z.avail_in > 0 && zs.z.avail_out > 0) {
                    const int r = inflate(&zs.z, Z_NO_FLUSH);
                    if (r == Z_STREAM_END) {
                        break;
                    }
                    if (r != Z_OK) {
                        return false;
                    }
                }
                break;
            }
            case chunk_type('I', 'E', 'N', 'D'):
                iend = true;
                break;
            default:
                if ((type & (1u << 29)) == 0) {
                    return false; // unknown critical chunk (CgBI among them)
                }
                break;
            }
            if (iend) {
                break;
            }
        }
        if (!iend || !seen_idat || zs.z.avail_out != 0) {
            return false; // truncated, or not enough pixels
        }

        // unfilter in place, then drop the filter bytes
        const size_t stride = size_t(w) * size_t(in_n);
        std::vector<uint8_t> zeros(stride, 0);
        const uint8_t* prev = zeros.data();
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = raw + size_t(y) * (stride + 1);
            if (!unfilter_row(in_n, row[0], row + 1, prev, stride)) {
                return false;
            }
            prev = row + 1;
        }
        for (uint32_t y = 0; y < h; ++y) {
            std::memmove(raw + size_t(y) * stride, raw + size_t(y) * (stride + 1) + 1, stride);
        }
        const size_t pixels = size_t(w) * size_t(h);

        uint8_t* out = nullptr;
        int out_n = in_n;
        if (color == 3) {
            out_n = pal_trns ? 4 : 3;
            out = static_cast<uint8_t*>(std::malloc(pixels * size_t(out_n)));
            if (!out) {
                return false;
            }
            uint8_t* o = out;
            for (size_t i = 0; i < pixels; ++i) {
                const uint8_t idx = raw[i];
                if (idx >= pal_len) {
                    std::free(out); // stb_image reads past the palette here: leave the file to it
                    return false;
                }
                const uint8_t* p = palette + size_t(idx) * 4;
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                if (out_n == 4) {
                    o[3] = p[3];
                }
                o += out_n;
            }
        } else if (has_trans) {
            out_n = in_n + 1;
            out = static_cast<uint8_t*>(std::malloc(pixels * size_t(out_n)));
            if (!out) {
                return false;
            }
            const uint8_t* s = raw;
            uint8_t* o = out;
            for (size_t i = 0; i < pixels; ++i) {
                if (in_n == 1) {
                    o[0] = s[0];
                    o[1] = s[0] == tc[0] ? 0 : 255;
                } else {
                    o[0] = s[0];
                    o[1] = s[1];
                    o[2] = s[2];
                    o[3] = (s[0] == tc[0] && s[1] == tc[1] && s[2] == tc[2]) ? 0 : 255;
                }
                s += in_n;
                o += out_n;
            }
        } else {
            out = static_cast<uint8_t*>(std::realloc(raw, pixels * size_t(in_n)));
            if (!out) {
                return false;
            }
            raw = nullptr; // now owned by out
        }

        image.data = out;
        image.width = static_cast<int>(w);
        image.height = static_cast<int>(h);
        image.mipmaps = 1;
        switch (out_n) {
        case 1:
            image.format = ray::PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
            break;
        case 2:
            image.format = ray::PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
            break;
        case 3:
            image.format = ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8;
            break;
        default:
            image.format = ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            break;
        }
        return true;
    }

#endif

} // namespace png_fast
