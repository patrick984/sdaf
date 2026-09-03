/*
 * SDAF transform benchmark
 *
 * Build without Zstandard:
 *   cc -O3 -std=c11 -Wall -Wextra -pedantic sdaf-bench.c -lm -o sdaf-bench
 *
 * Build with Zstandard:
 *   cc -O3 -std=c11 -Wall -Wextra -pedantic -DSDAF_HAVE_ZSTD \
 *      sdaf-bench.c -lm -lzstd -o sdaf-bench
 *
 * Run the resulting binary on each target CPU.  This is an experimental
 * benchmark, not a normative definition of SDAF transforms 1--4.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef SDAF_HAVE_ZSTD
#include <zstd.h>
#endif

typedef enum {
    DATA_DC,
    DATA_SMOOTH,
    DATA_STEPPED,
    DATA_NOISE
} dataset_kind;

typedef enum {
    PACK_ALIGNED,
    PACK_DENSE,
    PACK_SHUFFLE,
    PACK_BITPLANE
} packing_kind;

typedef struct {
    const char *name;
    packing_kind packing;
    int delta;
    int zigzag;
} pipeline;

typedef struct {
    unsigned width;
    size_t samples;
    size_t channels;
    size_t value_count;
    size_t block_values;
    uint32_t mask;
    const uint32_t *source;
    uint32_t *work_values;
    uint32_t *decoded_values;
    uint32_t *previous_values;
    uint8_t *work_bytes;
} bench_context;

static volatile uint64_t benchmark_sink;

static const pipeline pipelines[] = {
    {"aligned",                     PACK_ALIGNED,  0, 0},
    {"dense",                       PACK_DENSE,    0, 0},
    {"byte-shuffle",                PACK_SHUFFLE,  0, 0},
    {"bit-plane",                   PACK_BITPLANE, 0, 0},
    {"delta+dense",                 PACK_DENSE,    1, 0},
    {"delta+byte-shuffle",          PACK_SHUFFLE,  1, 0},
    {"delta+bit-plane",             PACK_BITPLANE, 1, 0},
    {"delta+zigzag+dense",          PACK_DENSE,    1, 1},
    {"delta+zigzag+byte-shuffle",   PACK_SHUFFLE,  1, 1},
    {"delta+zigzag+bit-plane",      PACK_BITPLANE, 1, 1}
};

static const char *compiler_name(void)
{
#if defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "unknown compiler";
#endif
}

static double now_seconds(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC)
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    return 0.0;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static double uniform_open01(uint32_t *state)
{
    return ((double)(xorshift32(state) >> 8) + 1.0) / 16777217.0;
}

static double gaussian(uint32_t *state)
{
    const double two_pi = 6.28318530717958647693;
    const double u1 = uniform_open01(state);
    const double u2 = uniform_open01(state);
    return sqrt(-2.0 * log(u1)) * cos(two_pi * u2);
}

static const char *dataset_name(dataset_kind kind)
{
    switch (kind) {
    case DATA_DC:      return "dc";
    case DATA_SMOOTH:  return "smooth";
    case DATA_STEPPED: return "stepped";
    case DATA_NOISE:   return "noise";
    }
    return "unknown";
}

static void generate_dataset(uint32_t *values, size_t samples, size_t channels,
                             unsigned width, dataset_kind kind, double noise_rms)
{
    const double two_pi = 6.28318530717958647693;
    const uint32_t mask = ((uint32_t)1u << width) - 1u;
    uint32_t rng = 0x6d2b79f5u ^ (uint32_t)width ^ ((uint32_t)kind << 24);
    size_t s;

    for (s = 0; s < samples; ++s) {
        const double common_noise = noise_rms * 0.25 * gaussian(&rng);
        size_t c;
        for (c = 0; c < channels; ++c) {
            const size_t i = s * channels + c;
            const double centre = (double)mask * (double)(c + 1u) /
                                  (double)(channels + 1u);
            double ideal;
            double measured;
            switch (kind) {
            case DATA_DC:
                ideal = centre;
                break;
            case DATA_SMOOTH:
                ideal = centre + (double)mask * 0.12 *
                        sin(two_pi * ((double)s / (4096.0 + 257.0 * (double)c) +
                                      0.13 * (double)c));
                ideal += (double)mask * 0.01 *
                         sin(two_pi * (double)s / 65536.0);
                break;
            case DATA_STEPPED:
                ideal = centre + ((double)((s / 4096u + c) % 5u) - 2.0) *
                                 (double)mask * 0.04;
                break;
            case DATA_NOISE:
                values[i] = xorshift32(&rng) & mask;
                continue;
            default:
                ideal = centre;
                break;
            }

            measured = ideal + common_noise + noise_rms * 0.9682458366 * gaussian(&rng);
            if (measured <= 0.0)
                values[i] = 0;
            else if (measured >= (double)mask)
                values[i] = mask;
            else
                values[i] = (uint32_t)llround(measured);
        }
    }
}

static size_t aligned_size(size_t count, unsigned width)
{
    return count * ((width + 7u) / 8u);
}

static size_t dense_size(size_t count, unsigned width)
{
    return (count * (size_t)width + 7u) / 8u;
}

static size_t bitplane_size(size_t count, unsigned width, size_t block_values)
{
    size_t total = 0;
    while (count != 0u) {
        const size_t n = count < block_values ? count : block_values;
        const size_t lane_bytes = n / 8u + (n % 8u != 0u);
        size_t block_bytes;
        if (lane_bytes > SIZE_MAX / width)
            return SIZE_MAX;
        block_bytes = lane_bytes * width;
        if (block_bytes > SIZE_MAX - total)
            return SIZE_MAX;
        total += block_bytes;
        count -= n;
    }
    return total;
}

static void encode_aligned(const uint32_t *values, size_t count, unsigned width,
                           uint8_t *output)
{
    const unsigned bytes = (width + 7u) / 8u;
    size_t i;
    for (i = 0; i < count; ++i) {
        uint32_t v = values[i];
        unsigned b;
        for (b = 0; b < bytes; ++b) {
            *output++ = (uint8_t)v;
            v >>= 8;
        }
    }
}

static void decode_aligned(const uint8_t *input, size_t count, unsigned width,
                           uint32_t *values)
{
    const unsigned bytes = (width + 7u) / 8u;
    size_t i;
    for (i = 0; i < count; ++i) {
        uint32_t v = 0;
        unsigned b;
        for (b = 0; b < bytes; ++b)
            v |= (uint32_t)(*input++) << (8u * b);
        values[i] = v;
    }
}

static void encode_dense(const uint32_t *values, size_t count, unsigned width,
                         uint8_t *output)
{
    uint64_t accumulator = 0;
    unsigned bits = 0;
    size_t i;
    for (i = 0; i < count; ++i) {
        accumulator |= (uint64_t)values[i] << bits;
        bits += width;
        while (bits >= 8u) {
            *output++ = (uint8_t)accumulator;
            accumulator >>= 8;
            bits -= 8u;
        }
    }
    if (bits != 0u)
        *output = (uint8_t)accumulator;
}

static void decode_dense(const uint8_t *input, size_t count, unsigned width,
                         uint32_t mask, uint32_t *values)
{
    uint64_t accumulator = 0;
    unsigned bits = 0;
    size_t i;
    for (i = 0; i < count; ++i) {
        while (bits < width) {
            accumulator |= (uint64_t)(*input++) << bits;
            bits += 8u;
        }
        values[i] = (uint32_t)accumulator & mask;
        accumulator >>= width;
        bits -= width;
    }
}

static int run_self_test(void)
{
    static const uint32_t values[] = {0xabcu, 0x123u};
    static const uint8_t expected[] = {0xbcu, 0x3au, 0x12u};
    uint8_t encoded[sizeof(expected)] = {0};
    uint32_t decoded[2] = {0, 0};

    encode_dense(values, 2u, 12u, encoded);
    decode_dense(encoded, 2u, 12u, 0xfffu, decoded);
    return memcmp(encoded, expected, sizeof(expected)) == 0 &&
           memcmp(values, decoded, sizeof(values)) == 0;
}

static void byte_shuffle(const uint8_t *input, size_t count, unsigned width,
                         size_t block_values, uint8_t *output)
{
    const unsigned bytes = (width + 7u) / 8u;
    size_t base;
    for (base = 0; base < count; base += block_values) {
        const size_t n = count - base < block_values ? count - base : block_values;
        unsigned b;
        for (b = 0; b < bytes; ++b) {
            size_t i;
            for (i = 0; i < n; ++i)
                *output++ = input[(base + i) * bytes + b];
        }
    }
}

static void byte_unshuffle(const uint8_t *input, size_t count, unsigned width,
                           size_t block_values, uint8_t *output)
{
    const unsigned bytes = (width + 7u) / 8u;
    size_t base;
    for (base = 0; base < count; base += block_values) {
        const size_t n = count - base < block_values ? count - base : block_values;
        unsigned b;
        for (b = 0; b < bytes; ++b) {
            size_t i;
            for (i = 0; i < n; ++i)
                output[(base + i) * bytes + b] = *input++;
        }
    }
}

static void encode_bitplanes(const uint32_t *values, size_t count, unsigned width,
                             size_t block_values, uint8_t *output)
{
    size_t base;
    for (base = 0; base < count; base += block_values) {
        const size_t n = count - base < block_values ? count - base : block_values;
        unsigned bit;
        for (bit = 0; bit < width; ++bit) {
            size_t i;
            for (i = 0; i < n; i += 8u) {
                uint8_t packed = 0;
                unsigned lane;
                for (lane = 0; lane < 8u && i + lane < n; ++lane)
                    packed |= (uint8_t)(((values[base + i + lane] >> bit) & 1u) << lane);
                *output++ = packed;
            }
        }
    }
}

static void decode_bitplanes(const uint8_t *input, size_t count, unsigned width,
                             size_t block_values, uint32_t *values)
{
    size_t base;
    memset(values, 0, count * sizeof(*values));
    for (base = 0; base < count; base += block_values) {
        const size_t n = count - base < block_values ? count - base : block_values;
        unsigned bit;
        for (bit = 0; bit < width; ++bit) {
            size_t i;
            for (i = 0; i < n; i += 8u) {
                const uint8_t packed = *input++;
                unsigned lane;
                for (lane = 0; lane < 8u && i + lane < n; ++lane)
                    values[base + i + lane] |= (uint32_t)((packed >> lane) & 1u) << bit;
            }
        }
    }
}

static void delta_encode(const uint32_t *input, uint32_t *output,
                         size_t samples, size_t channels, uint32_t mask,
                         uint32_t *previous)
{
    size_t s;
    memset(previous, 0, channels * sizeof(*previous));
    for (s = 0; s < samples; ++s) {
        size_t c;
        for (c = 0; c < channels; ++c) {
            const size_t i = s * channels + c;
            const uint32_t current = input[i];
            output[i] = (current - previous[c]) & mask;
            previous[c] = current;
        }
    }
}

static void delta_decode(uint32_t *values, size_t samples, size_t channels,
                         uint32_t mask, uint32_t *previous)
{
    size_t s;
    memset(previous, 0, channels * sizeof(*previous));
    for (s = 0; s < samples; ++s) {
        size_t c;
        for (c = 0; c < channels; ++c) {
            const size_t i = s * channels + c;
            previous[c] = (previous[c] + values[i]) & mask;
            values[i] = previous[c];
        }
    }
}

static void zigzag_encode(uint32_t *values, size_t count, unsigned width)
{
    const uint32_t sign_bit = (uint32_t)1u << (width - 1u);
    const int64_t modulus = (int64_t)1 << width;
    size_t i;
    for (i = 0; i < count; ++i) {
        const uint32_t raw = values[i];
        const int64_t signed_value = (raw & sign_bit) != 0u
                                   ? (int64_t)raw - modulus
                                   : (int64_t)raw;
        values[i] = signed_value < 0
                  ? (uint32_t)(-2 * signed_value - 1)
                  : (uint32_t)(2 * signed_value);
    }
}

static void zigzag_decode(uint32_t *values, size_t count, uint32_t mask)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        const uint32_t encoded = values[i];
        const int64_t signed_value = (encoded & 1u) != 0u
                                   ? -(int64_t)(encoded / 2u) - 1
                                   : (int64_t)(encoded / 2u);
        values[i] = (uint32_t)signed_value & mask;
    }
}

static size_t pipeline_size(const bench_context *ctx, const pipeline *p)
{
    if (p->packing == PACK_DENSE)
        return dense_size(ctx->value_count, ctx->width);
    if (p->packing == PACK_BITPLANE)
        return bitplane_size(ctx->value_count, ctx->width, ctx->block_values);
    return aligned_size(ctx->value_count, ctx->width);
}

static void pipeline_encode(bench_context *ctx, const pipeline *p, uint8_t *output)
{
    const uint32_t *values = ctx->source;
    if (p->delta) {
        delta_encode(ctx->source, ctx->work_values, ctx->samples,
                     ctx->channels, ctx->mask, ctx->previous_values);
        if (p->zigzag)
            zigzag_encode(ctx->work_values, ctx->value_count, ctx->width);
        values = ctx->work_values;
    }

    switch (p->packing) {
    case PACK_ALIGNED:
        encode_aligned(values, ctx->value_count, ctx->width, output);
        break;
    case PACK_DENSE:
        encode_dense(values, ctx->value_count, ctx->width, output);
        break;
    case PACK_SHUFFLE:
        encode_aligned(values, ctx->value_count, ctx->width, ctx->work_bytes);
        byte_shuffle(ctx->work_bytes, ctx->value_count, ctx->width,
                     ctx->block_values, output);
        break;
    case PACK_BITPLANE:
        encode_bitplanes(values, ctx->value_count, ctx->width,
                         ctx->block_values, output);
        break;
    }
}

static void pipeline_decode(bench_context *ctx, const pipeline *p,
                            const uint8_t *input)
{
    switch (p->packing) {
    case PACK_ALIGNED:
        decode_aligned(input, ctx->value_count, ctx->width, ctx->decoded_values);
        break;
    case PACK_DENSE:
        decode_dense(input, ctx->value_count, ctx->width, ctx->mask,
                     ctx->decoded_values);
        break;
    case PACK_SHUFFLE:
        byte_unshuffle(input, ctx->value_count, ctx->width, ctx->block_values,
                       ctx->work_bytes);
        decode_aligned(ctx->work_bytes, ctx->value_count, ctx->width,
                       ctx->decoded_values);
        break;
    case PACK_BITPLANE:
        decode_bitplanes(input, ctx->value_count, ctx->width,
                         ctx->block_values, ctx->decoded_values);
        break;
    }
    if (p->zigzag)
        zigzag_decode(ctx->decoded_values, ctx->value_count, ctx->mask);
    if (p->delta)
        delta_decode(ctx->decoded_values, ctx->samples, ctx->channels, ctx->mask,
                     ctx->previous_values);
}

static double time_pipeline(bench_context *ctx, const pipeline *p, uint8_t *encoded,
                            size_t encoded_bytes, double minimum_seconds, int decode)
{
    const size_t input_bytes = aligned_size(ctx->value_count, ctx->width);
    size_t iterations = 0;
    double start;
    double elapsed;

    if (decode)
        pipeline_decode(ctx, p, encoded);
    else
        pipeline_encode(ctx, p, encoded);

    start = now_seconds();
    do {
        unsigned batch;
        for (batch = 0; batch < 4u; ++batch) {
            if (decode)
                pipeline_decode(ctx, p, encoded);
            else
                pipeline_encode(ctx, p, encoded);
            benchmark_sink += decode ? ctx->decoded_values[iterations % ctx->value_count]
                                     : encoded[iterations % encoded_bytes];
            ++iterations;
        }
        elapsed = now_seconds() - start;
    } while (elapsed < minimum_seconds);

    return ((double)input_bytes * (double)iterations / (1024.0 * 1024.0)) / elapsed;
}

#ifdef SDAF_HAVE_ZSTD
static double time_zstd_compress(ZSTD_CCtx *cctx, const uint8_t *input,
                                 size_t input_size, uint8_t *output,
                                 size_t output_capacity, int level,
                                 double minimum_seconds, size_t *compressed_size)
{
    size_t iterations = 0;
    double start = now_seconds();
    double elapsed;
    do {
        size_t result = ZSTD_compressCCtx(cctx, output, output_capacity,
                                          input, input_size, level);
        if (ZSTD_isError(result)) {
            fprintf(stderr, "Zstandard compression failed: %s\n", ZSTD_getErrorName(result));
            exit(EXIT_FAILURE);
        }
        *compressed_size = result;
        benchmark_sink += output[iterations % result];
        ++iterations;
        elapsed = now_seconds() - start;
    } while (elapsed < minimum_seconds);
    return ((double)input_size * (double)iterations / (1024.0 * 1024.0)) / elapsed;
}

static double time_zstd_decompress(ZSTD_DCtx *dctx, const uint8_t *input,
                                   size_t input_size, uint8_t *output,
                                   size_t output_size, double minimum_seconds)
{
    size_t iterations = 0;
    double start = now_seconds();
    double elapsed;
    do {
        size_t result = ZSTD_decompressDCtx(dctx, output, output_size,
                                            input, input_size);
        if (ZSTD_isError(result) || result != output_size) {
            fprintf(stderr, "Zstandard decompression failed\n");
            exit(EXIT_FAILURE);
        }
        benchmark_sink += output[iterations % output_size];
        ++iterations;
        elapsed = now_seconds() - start;
    } while (elapsed < minimum_seconds);
    return ((double)output_size * (double)iterations / (1024.0 * 1024.0)) / elapsed;
}
#endif

static size_t parse_size(const char *text, const char *option)
{
    char *end = NULL;
    unsigned long long value;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || end == text || *end != '\0' || value == 0 || value > SIZE_MAX) {
        fprintf(stderr, "Invalid value for %s: %s\n", option, text);
        exit(EXIT_FAILURE);
    }
    return (size_t)value;
}

static double parse_seconds(const char *text)
{
    char *end = NULL;
    double value;
    errno = 0;
    value = strtod(text, &end);
    if (errno || end == text || *end != '\0' || value <= 0.0) {
        fprintf(stderr, "Invalid value for --seconds: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return value;
}

static double parse_noise(const char *text, const char *option)
{
    char *end = NULL;
    double value;
    errno = 0;
    value = strtod(text, &end);
    if (errno || end == text || *end != '\0' || !isfinite(value) || value < 0.0) {
        fprintf(stderr, "Invalid value for %s: %s\n", option, text);
        exit(EXIT_FAILURE);
    }
    return value;
}

static void usage(const char *program)
{
    printf("Usage: %s [--samples N] [--channels N] [--block-values N]\n"
           "          [--seconds S] [--zstd-level N] [--noise-rms N]\n"
           "          [--noise-rms-12 N] [--noise-rms-16 N] [--noise-rms-24 N]\n\n"
           "Defaults: 262144 samples, 4 channels, 128 values per transform\n"
           "block, 0.15 seconds per timed operation, Zstandard level 3, and\n"
           "Gaussian noise of 1, 2 and 16 LSB RMS for 12, 16 and 24 bits.\n"
           "--noise-rms sets the same RMS noise for all three widths.\n",
           program);
}

int main(int argc, char **argv)
{
    size_t samples = 262144u;
    size_t channels = 4u;
    size_t block_values = 128u;
    double minimum_seconds = 0.15;
    double noise_rms[] = {1.0, 2.0, 16.0};
    int zstd_level = 3;
    uint32_t *source = NULL;
    uint32_t *work_values = NULL;
    uint32_t *decoded_values = NULL;
    uint32_t *previous_values = NULL;
    uint8_t *encoded = NULL;
    uint8_t *work_bytes = NULL;
#ifdef SDAF_HAVE_ZSTD
    uint8_t *compressed = NULL;
    uint8_t *decompressed = NULL;
    ZSTD_CCtx *cctx = NULL;
    ZSTD_DCtx *dctx = NULL;
#endif
    size_t value_count;
    size_t max_bytes;
    size_t max_bitplane_bytes;
    int arg;

    for (arg = 1; arg < argc; ++arg) {
        if (strcmp(argv[arg], "--help") == 0) {
            usage(argv[0]);
            return EXIT_SUCCESS;
        } else if (arg + 1 >= argc) {
            fprintf(stderr, "Missing value after %s\n", argv[arg]);
            return EXIT_FAILURE;
        } else if (strcmp(argv[arg], "--samples") == 0) {
            samples = parse_size(argv[++arg], "--samples");
        } else if (strcmp(argv[arg], "--channels") == 0) {
            channels = parse_size(argv[++arg], "--channels");
        } else if (strcmp(argv[arg], "--block-values") == 0) {
            block_values = parse_size(argv[++arg], "--block-values");
        } else if (strcmp(argv[arg], "--seconds") == 0) {
            minimum_seconds = parse_seconds(argv[++arg]);
        } else if (strcmp(argv[arg], "--zstd-level") == 0) {
            zstd_level = atoi(argv[++arg]);
        } else if (strcmp(argv[arg], "--noise-rms") == 0) {
            const double value = parse_noise(argv[++arg], "--noise-rms");
            noise_rms[0] = value;
            noise_rms[1] = value;
            noise_rms[2] = value;
        } else if (strcmp(argv[arg], "--noise-rms-12") == 0) {
            noise_rms[0] = parse_noise(argv[++arg], "--noise-rms-12");
        } else if (strcmp(argv[arg], "--noise-rms-16") == 0) {
            noise_rms[1] = parse_noise(argv[++arg], "--noise-rms-16");
        } else if (strcmp(argv[arg], "--noise-rms-24") == 0) {
            noise_rms[2] = parse_noise(argv[++arg], "--noise-rms-24");
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[arg]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

#ifndef SDAF_HAVE_ZSTD
    (void)zstd_level;
#endif

    if (samples > SIZE_MAX / channels) {
        fprintf(stderr, "Sample count times channel count is too large\n");
        return EXIT_FAILURE;
    }
    if (!run_self_test()) {
        fprintf(stderr, "Canonical 12-bit dense-packing self-test failed\n");
        return EXIT_FAILURE;
    }
    value_count = samples * channels;
    if (value_count > SIZE_MAX / sizeof(uint32_t) || value_count > SIZE_MAX / 3u) {
        fprintf(stderr, "Requested benchmark is too large\n");
        return EXIT_FAILURE;
    }
    max_bytes = value_count * 3u;
    max_bitplane_bytes = bitplane_size(value_count, 24u, block_values);
    if (max_bitplane_bytes == SIZE_MAX) {
        fprintf(stderr, "Requested transform block layout is too large\n");
        return EXIT_FAILURE;
    }
    if (max_bitplane_bytes > max_bytes)
        max_bytes = max_bitplane_bytes;

    source = (uint32_t *)malloc(value_count * sizeof(*source));
    work_values = (uint32_t *)malloc(value_count * sizeof(*work_values));
    decoded_values = (uint32_t *)malloc(value_count * sizeof(*decoded_values));
    previous_values = (uint32_t *)malloc(channels * sizeof(*previous_values));
    encoded = (uint8_t *)malloc(max_bytes);
    work_bytes = (uint8_t *)malloc(max_bytes);
    if (!source || !work_values || !decoded_values || !previous_values ||
        !encoded || !work_bytes) {
        fprintf(stderr, "Unable to allocate benchmark buffers\n");
        return EXIT_FAILURE;
    }

#ifdef SDAF_HAVE_ZSTD
    compressed = (uint8_t *)malloc(ZSTD_compressBound(max_bytes));
    decompressed = (uint8_t *)malloc(max_bytes);
    cctx = ZSTD_createCCtx();
    dctx = ZSTD_createDCtx();
    if (!compressed || !decompressed || !cctx || !dctx) {
        fprintf(stderr, "Unable to allocate Zstandard state\n");
        return EXIT_FAILURE;
    }
#endif

    printf("SDAF transform benchmark\n");
    printf("compiler: %s; pointer width: %zu bits\n",
           compiler_name(), sizeof(void *) * CHAR_BIT);
    printf("samples/channel: %zu; channels: %zu; total values: %zu\n",
           samples, channels, value_count);
    printf("transform block: %zu values; minimum timing: %.3f s\n",
           block_values, minimum_seconds);
    printf("Gaussian noise RMS: 12-bit %.3f LSB; 16-bit %.3f LSB; "
           "24-bit %.3f LSB\n", noise_rms[0], noise_rms[1], noise_rms[2]);
#ifdef SDAF_HAVE_ZSTD
    printf("Zstandard: %s; level: %d\n", ZSTD_versionString(), zstd_level);
#else
    printf("Zstandard: disabled (rebuild with -DSDAF_HAVE_ZSTD and -lzstd)\n");
#endif
    printf("Rates are MiB/s of byte-aligned source values. Ratio is stored/aligned.\n");
    printf("Experimental layouts are block-local and are not normative SDAF encodings.\n\n");

    {
        const unsigned widths[] = {12u, 16u, 24u};
        dataset_kind kind;
        size_t wi;
        for (kind = DATA_DC; kind <= DATA_NOISE; kind = (dataset_kind)(kind + 1)) {
            for (wi = 0; wi < sizeof(widths) / sizeof(widths[0]); ++wi) {
                bench_context ctx;
                const unsigned width = widths[wi];
                const size_t baseline = aligned_size(value_count, width);
                size_t pi;

                generate_dataset(source, samples, channels, width, kind,
                                 noise_rms[wi]);
                ctx.width = width;
                ctx.samples = samples;
                ctx.channels = channels;
                ctx.value_count = value_count;
                ctx.block_values = block_values;
                ctx.mask = ((uint32_t)1u << width) - 1u;
                ctx.source = source;
                ctx.work_values = work_values;
                ctx.decoded_values = decoded_values;
                ctx.previous_values = previous_values;
                ctx.work_bytes = work_bytes;

                if (kind == DATA_NOISE)
                    printf("Dataset: %-7s  width: %2u-bit  noise: full-scale uniform  "
                           "aligned input: %zu bytes\n",
                           dataset_name(kind), width, baseline);
                else
                    printf("Dataset: %-7s  width: %2u-bit  noise: %.3f LSB RMS  "
                           "aligned input: %zu bytes\n",
                           dataset_name(kind), width, noise_rms[wi], baseline);
#ifdef SDAF_HAVE_ZSTD
                printf("  %-27s %10s %9s %10s %10s %10s %9s %10s %10s\n",
                       "pipeline", "bytes", "ratio", "enc MiB/s", "dec MiB/s",
                       "zstd bytes", "zratio", "zenc MiB/s", "zdec MiB/s");
#else
                printf("  %-27s %10s %9s %10s %10s\n",
                       "pipeline", "bytes", "ratio", "enc MiB/s", "dec MiB/s");
#endif
                for (pi = 0; pi < sizeof(pipelines) / sizeof(pipelines[0]); ++pi) {
                    const pipeline *p = &pipelines[pi];
                    const size_t bytes = pipeline_size(&ctx, p);
                    double encode_rate;
                    double decode_rate;

                    pipeline_encode(&ctx, p, encoded);
                    pipeline_decode(&ctx, p, encoded);
                    if (memcmp(source, decoded_values,
                               value_count * sizeof(*source)) != 0) {
                        fprintf(stderr, "Round-trip failure: %s, %u-bit, %s\n",
                                dataset_name(kind), width, p->name);
                        return EXIT_FAILURE;
                    }
                    encode_rate = time_pipeline(&ctx, p, encoded, bytes,
                                                minimum_seconds, 0);
                    decode_rate = time_pipeline(&ctx, p, encoded, bytes,
                                                minimum_seconds, 1);
#ifdef SDAF_HAVE_ZSTD
                    {
                        size_t compressed_size = 0;
                        const size_t bound = ZSTD_compressBound(bytes);
                        double zencode_rate = time_zstd_compress(
                            cctx, encoded, bytes, compressed, bound, zstd_level,
                            minimum_seconds, &compressed_size);
                        double zdecode_rate = time_zstd_decompress(
                            dctx, compressed, compressed_size, decompressed, bytes,
                            minimum_seconds);
                        if (memcmp(encoded, decompressed, bytes) != 0) {
                            fprintf(stderr, "Zstandard round-trip failure\n");
                            return EXIT_FAILURE;
                        }
                        printf("  %-27s %10zu %9.4f %10.1f %10.1f %10zu %9.4f %10.1f %10.1f\n",
                               p->name, bytes, (double)bytes / (double)baseline,
                               encode_rate, decode_rate, compressed_size,
                               (double)compressed_size / (double)baseline,
                               zencode_rate, zdecode_rate);
                    }
#else
                    printf("  %-27s %10zu %9.4f %10.1f %10.1f\n",
                           p->name, bytes, (double)bytes / (double)baseline,
                           encode_rate, decode_rate);
#endif
                }
                putchar('\n');
            }
        }
    }

    printf("All transform and compression round trips verified.\n");
    printf("benchmark checksum: %" PRIu64 "\n", benchmark_sink);

#ifdef SDAF_HAVE_ZSTD
    ZSTD_freeCCtx(cctx);
    ZSTD_freeDCtx(dctx);
    free(compressed);
    free(decompressed);
#endif
    free(source);
    free(work_values);
    free(decoded_values);
    free(previous_values);
    free(encoded);
    free(work_bytes);
    return EXIT_SUCCESS;
}
