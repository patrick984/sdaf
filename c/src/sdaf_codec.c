#include "sdaf_internal.h"

#include <math.h>
#include <stdio.h>

#ifdef SDAF_HAVE_ZSTD
#include <zstd.h>
#endif

static uint64_t bit_mask(unsigned bits)
{
    return bits == 64u ? UINT64_MAX : (UINT64_C(1) << bits) - 1u;
}

static int64_t sign_extend(uint64_t value, unsigned bits)
{
    uint64_t sign;
    int64_t result;
    if (bits == 64u) {
        memcpy(&result, &value, sizeof(result));
        return result;
    }
    sign = UINT64_C(1) << (bits - 1u);
    value = (value ^ sign) - sign;
    memcpy(&result, &value, sizeof(result));
    return result;
}

static uint64_t read_bits(
    const uint8_t* input, size_t input_size, size_t* bit_offset, unsigned width, int* ok)
{
    uint64_t value = 0u;
    unsigned bit;
    for (bit = 0u; bit < width; ++bit) {
        size_t byte = *bit_offset >> 3u;
        if (byte >= input_size) {
            *ok = 0;
            return 0u;
        }
        if ((input[byte] & (uint8_t)(1u << (*bit_offset & 7u))) != 0u)
            value |= UINT64_C(1) << bit;
        ++*bit_offset;
    }
    return value;
}

static uint64_t read_aligned(
    const uint8_t* input, size_t input_size, size_t* offset, unsigned bits, int* ok)
{
    unsigned bytes = (bits + 7u) / 8u;
    uint64_t value = 0u;
    unsigned i;
    if (*offset > input_size || bytes > input_size - *offset) {
        *ok = 0;
        return 0u;
    }
    for (i = 0u; i < bytes; ++i)
        value |= (uint64_t)input[*offset + i] << (i * 8u);
    *offset += bytes;
    return value;
}

static int channel_start_lane(const sdaf_channel_info* channels, size_t channel_index, size_t* lane)
{
    size_t i, result = 0u;
    for (i = 0u; i < channel_index; ++i)
        if (!sdaf_add_size(result, channels[i].elements, &result))
            return 0;
    *lane = result;
    return 1;
}

static size_t order_count(const sdaf_data_record* data, size_t lane_count)
{
    size_t result;
    return sdaf_mul_size(data->sample_count, lane_count, &result) ? result : SIZE_MAX;
}

static int order_item(const sdaf_channel_info* channels, size_t channel_count, size_t lane_count,
    const sdaf_data_record* data, size_t position, size_t* sample, size_t* channel_index,
    uint32_t* element, size_t* lane)
{
    size_t c;
    if (data->layout == SDAF_LAYOUT_INTERLEAVED) {
        size_t within = position % lane_count;
        *sample = position / lane_count;
        for (c = 0u; c < channel_count; ++c) {
            if (within < channels[c].elements) {
                size_t start;
                if (!channel_start_lane(channels, c, &start))
                    return 0;
                *channel_index = c;
                *element = (uint32_t)within;
                *lane = start + within;
                return 1;
            }
            within -= channels[c].elements;
        }
    } else {
        size_t base = 0u;
        for (c = 0u; c < channel_count; ++c) {
            size_t channel_values;
            if (!sdaf_mul_size(data->sample_count, channels[c].elements, &channel_values))
                return 0;
            if (position - base < channel_values) {
                size_t relative = position - base, start;
                if (!channel_start_lane(channels, c, &start))
                    return 0;
                *sample = relative / channels[c].elements;
                *element = (uint32_t)(relative % channels[c].elements);
                *channel_index = c;
                *lane = start + *element;
                return 1;
            }
            base += channel_values;
        }
    }
    return 0;
}

static int mul_div_fraction(
    uint64_t remainder, uint64_t multiplier, uint64_t denominator, uint64_t* quotient)
{
    uint64_t part_q = 0u, part_r = remainder, result_q = 0u, result_r = 0u;
    while (multiplier != 0u) {
        if ((multiplier & 1u) != 0u) {
            uint64_t carry = result_r >= denominator - part_r ? 1u : 0u;
            if (part_q > UINT64_MAX - carry || result_q > UINT64_MAX - part_q - carry)
                return 0;
            result_q += part_q + carry;
            result_r = carry != 0u ? result_r - (denominator - part_r) : result_r + part_r;
        }
        multiplier >>= 1u;
        if (multiplier != 0u) {
            uint64_t carry = part_r >= denominator - part_r ? 1u : 0u;
            if (part_q > (UINT64_MAX - carry) / 2u)
                return 0;
            part_q = part_q * 2u + carry;
            part_r = carry != 0u ? part_r - (denominator - part_r) : part_r * 2u;
        }
    }
    *quotient = result_q;
    return 1;
}

static int add_positive_ticks(int64_t start, uint64_t ticks, int64_t* result)
{
    if (start >= 0) {
        if (ticks > (uint64_t)(INT64_MAX - start))
            return 0;
        *result = start + (int64_t)ticks;
        return 1;
    }
    {
        uint64_t magnitude = (uint64_t)(-(start + 1)) + 1u;
        if (ticks < magnitude) {
            uint64_t remaining = magnitude - ticks;
            *result = remaining == (UINT64_C(1) << 63u) ? INT64_MIN : -(int64_t)remaining;
        } else {
            uint64_t positive = ticks - magnitude;
            if (positive > (uint64_t)INT64_MAX)
                return 0;
            *result = (int64_t)positive;
        }
    }
    return 1;
}

static int64_t sample_time(
    const sdaf_data_record* data, const uint8_t* decoded, size_t sample, int* has_time, int* ok)
{
    int64_t value = 0;
    *has_time = 1;
    if (data->timestamp_mode == SDAF_TIMESTAMP_NONE) {
        *has_time = 0;
        return 0;
    }
    if (data->timestamp_mode == SDAF_TIMESTAMP_DELTA) {
        int64_t delta = sdaf_get_i64(decoded + sample * 8u);
        if ((delta > 0 && data->start_time_ticks > INT64_MAX - delta)
            || (delta < 0 && data->start_time_ticks < INT64_MIN - delta)) {
            *ok = 0;
            return 0;
        }
        return data->start_time_ticks + delta;
    }
    if (data->timestamp_mode == SDAF_TIMESTAMP_EXPLICIT)
        return sdaf_get_i64(decoded + sample * 8u);
    if (data->timestamp_mode == SDAF_TIMESTAMP_PERIODIC) {
        uint64_t n = sample;
        uint64_t quotient = data->period_numerator / data->period_denominator;
        uint64_t remainder = data->period_numerator % data->period_denominator;
        uint64_t ticks, fraction;
        if ((quotient != 0u && n > UINT64_MAX / quotient)
            || !mul_div_fraction(remainder, n, data->period_denominator, &fraction)) {
            *ok = 0;
            return 0;
        }
        ticks = n * quotient;
        if (ticks > UINT64_MAX - fraction
            || !add_positive_ticks(data->start_time_ticks, ticks + fraction, &value)) {
            *ok = 0;
            return 0;
        }
    }
    return value;
}

static sdaf_status read_fixed_bytes(const uint8_t* input, size_t input_size, size_t* bit_offset,
    size_t* byte_offset, const sdaf_channel_info* channel, uint8_t packing, uint8_t** bytes)
{
    size_t count = channel->storage_bits / 8u;
    uint8_t* result = (uint8_t*)calloc(count == 0u ? 1u : count, 1u);
    if (result == NULL)
        return SDAF_ERROR_MEMORY;
    if (packing == SDAF_PACKING_BYTE_ALIGNED) {
        if (*byte_offset > input_size || count > input_size - *byte_offset) {
            free(result);
            return SDAF_ERROR_FORMAT;
        }
        memcpy(result, input + *byte_offset, count);
        *byte_offset += count;
    } else {
        size_t bit;
        for (bit = 0u; bit < channel->storage_bits; ++bit) {
            size_t source_byte = *bit_offset >> 3u;
            if (source_byte >= input_size) {
                free(result);
                return SDAF_ERROR_FORMAT;
            }
            if ((input[source_byte] & (uint8_t)(1u << (*bit_offset & 7u))) != 0u)
                result[bit >> 3u] |= (uint8_t)(1u << (bit & 7u));
            ++*bit_offset;
        }
    }
    *bytes = result;
    return SDAF_OK;
}

static sdaf_status make_value(sdaf_sample_value* value, const sdaf_channel_info* channel,
    uint32_t element, uint64_t stored, uint8_t* fixed, size_t fixed_size, char* error,
    size_t error_size)
{
    uint64_t raw = stored;
    value->channel_id = channel->id;
    value->channel_name = channel->name;
    value->element_index = element;
    value->logical_type = channel->logical_type;
    value->unit = channel->unit;
    if (fixed != NULL) {
        value->bytes = fixed;
        value->byte_count = fixed_size;
        value->numeric_value = NAN;
        value->physical_value = NAN;
        return SDAF_OK;
    }
    if (channel->storage_bits > channel->logical_bits) {
        uint64_t logical_mask = bit_mask(channel->logical_bits);
        uint64_t high_mask = bit_mask(channel->storage_bits) & ~logical_mask;
        uint64_t expected = channel->logical_type == SDAF_LOGICAL_SIGNED
                && (stored & (UINT64_C(1) << (channel->logical_bits - 1u))) != 0u
            ? high_mask
            : 0u;
        if ((stored & high_mask) != expected) {
            sdaf_set_error(
                error, error_size, "channel %u has invalid unused high bits", channel->id);
            return SDAF_ERROR_FORMAT;
        }
        raw &= logical_mask;
    }
    value->raw_unsigned = raw;
    value->raw_signed = channel->logical_type == SDAF_LOGICAL_SIGNED
        ? sign_extend(raw, channel->logical_bits)
        : 0;
    if (channel->logical_type == SDAF_LOGICAL_FLOAT) {
        if (channel->logical_bits == 32u) {
            uint32_t bits = (uint32_t)raw;
            float f;
            memcpy(&f, &bits, sizeof(f));
            value->numeric_value = f;
        } else {
            double d;
            memcpy(&d, &raw, sizeof(d));
            value->numeric_value = d;
        }
    } else if (channel->logical_type == SDAF_LOGICAL_SIGNED)
        value->numeric_value = (double)value->raw_signed;
    else
        value->numeric_value = (double)raw;
    value->physical_value = value->numeric_value * channel->scale + channel->offset;
    return SDAF_OK;
}

sdaf_status sdaf_decode_data_samples(sdaf_data_record* data, const sdaf_schema* schema,
    const sdaf_limits* limits, char* error, size_t error_size)
{
    sdaf_channel_info* channels = NULL;
    size_t channel_count = 0u, lane_count = 0u, value_count, sample;
    size_t bit_offset = 0u, byte_offset = 0u, position;
    const uint8_t* sample_area;
    size_t sample_area_size;
    sdaf_status status = sdaf_channels_for_stream(
        schema, data->stream_id, limits, &channels, &channel_count, &lane_count, error, error_size);
    if (status != SDAF_OK)
        return status;
    if (data->sample_count != 0u
        && data->first_sample_index > UINT64_MAX - ((uint64_t)data->sample_count - 1u)) {
        free(channels);
        sdaf_set_error(error, error_size, "sample index overflow");
        return SDAF_ERROR_FORMAT;
    }
    value_count = order_count(data, lane_count);
    if (value_count == SIZE_MAX) {
        free(channels);
        return SDAF_ERROR_LIMIT;
    }
    data->samples = (sdaf_sample*)sdaf_calloc_array(data->sample_count, sizeof(*data->samples));
    if (data->samples == NULL) {
        free(channels);
        return SDAF_ERROR_MEMORY;
    }
    for (sample = 0u; sample < data->sample_count; ++sample) {
        sdaf_sample* s = &data->samples[sample];
        int ok = 1;
        s->index = data->first_sample_index + sample;
        s->value_count = lane_count;
        s->values = (sdaf_sample_value*)sdaf_calloc_array(lane_count, sizeof(*s->values));
        if (s->values == NULL) {
            free(channels);
            return SDAF_ERROR_MEMORY;
        }
        s->time_ticks = sample_time(data, data->decoded_payload, sample, &s->has_time, &ok);
        if (!ok) {
            free(channels);
            sdaf_set_error(error, error_size, "sample timestamp overflow");
            return SDAF_ERROR_FORMAT;
        }
    }
    sample_area = data->decoded_payload + data->timestamp_bytes;
    sample_area_size = data->decoded_payload_size - data->timestamp_bytes;
    for (position = 0u; position < value_count; ++position) {
        size_t channel_index, lane;
        uint32_t element;
        uint64_t raw = 0u;
        int ok = 1;
        uint8_t* fixed = NULL;
        if (!order_item(channels, channel_count, lane_count, data, position, &sample,
                &channel_index, &element, &lane)) {
            status = SDAF_ERROR_FORMAT;
            break;
        }
        if (channels[channel_index].logical_type == SDAF_LOGICAL_FIXED_BYTES)
            status = read_fixed_bytes(sample_area, sample_area_size, &bit_offset, &byte_offset,
                &channels[channel_index], data->packing, &fixed);
        else
            raw = data->packing == SDAF_PACKING_LSB0_DENSE
                ? read_bits(sample_area, sample_area_size, &bit_offset,
                      channels[channel_index].storage_bits, &ok)
                : read_aligned(sample_area, sample_area_size, &byte_offset,
                      channels[channel_index].storage_bits, &ok);
        if (status != SDAF_OK || !ok) {
            free(fixed);
            status = SDAF_ERROR_FORMAT;
            break;
        }
        status = make_value(&data->samples[sample].values[lane], &channels[channel_index], element,
            raw, fixed, channels[channel_index].storage_bits / 8u, error, error_size);
        if (status != SDAF_OK) {
            free(fixed);
            break;
        }
    }
    if (status == SDAF_OK && data->packing == SDAF_PACKING_LSB0_DENSE && (bit_offset & 7u) != 0u
        && sample_area_size != 0u
        && (sample_area[sample_area_size - 1u] >> (bit_offset & 7u)) != 0u) {
        sdaf_set_error(error, error_size, "dense payload has nonzero padding bits");
        status = SDAF_ERROR_FORMAT;
    }
    free(channels);
    return status;
}

sdaf_status sdaf_zstd_compress(const uint8_t* source, size_t source_size, uint8_t** output,
    size_t* output_size, char* error, size_t error_size)
{
#ifdef SDAF_HAVE_ZSTD
    size_t bound = ZSTD_compressBound(source_size), result;
    uint8_t* buffer = (uint8_t*)malloc(bound == 0u ? 1u : bound);
    if (buffer == NULL)
        return SDAF_ERROR_MEMORY;
    result = ZSTD_compress(buffer, bound, source, source_size, 3);
    if (ZSTD_isError(result)) {
        sdaf_set_error(error, error_size, "Zstandard: %s", ZSTD_getErrorName(result));
        free(buffer);
        return SDAF_ERROR_FORMAT;
    }
    *output = buffer;
    *output_size = result;
    return SDAF_OK;
#else
    (void)source;
    (void)source_size;
    (void)output;
    (void)output_size;
    sdaf_set_error(error, error_size, "Zstandard support was not built");
    return SDAF_ERROR_UNSUPPORTED;
#endif
}

sdaf_status sdaf_zstd_decompress(const uint8_t* source, size_t source_size, size_t expected_size,
    uint8_t** output, char* error, size_t error_size)
{
#ifdef SDAF_HAVE_ZSTD
    size_t result;
    uint8_t* buffer = (uint8_t*)malloc(expected_size == 0u ? 1u : expected_size);
    if (buffer == NULL)
        return SDAF_ERROR_MEMORY;
    result = ZSTD_decompress(buffer, expected_size, source, source_size);
    if (ZSTD_isError(result)) {
        sdaf_set_error(error, error_size, "Zstandard: %s", ZSTD_getErrorName(result));
        free(buffer);
        return SDAF_ERROR_FORMAT;
    }
    if (result != expected_size) {
        sdaf_set_error(error, error_size, "Zstandard output is %lu bytes, expected %lu",
            (unsigned long)result, (unsigned long)expected_size);
        free(buffer);
        return SDAF_ERROR_FORMAT;
    }
    *output = buffer;
    return SDAF_OK;
#else
    (void)source;
    (void)source_size;
    (void)expected_size;
    (void)output;
    sdaf_set_error(error, error_size, "Zstandard support was not built");
    return SDAF_ERROR_UNSUPPORTED;
#endif
}

static sdaf_status unshuffle(const uint8_t* input, size_t input_size, size_t count, unsigned width,
    unsigned bits, uint64_t** values)
{
    uint64_t* result = (uint64_t*)sdaf_calloc_array(count, sizeof(*result));
    size_t offset = 0u, first;
    uint64_t mask = bit_mask(bits);
    if (result == NULL)
        return SDAF_ERROR_MEMORY;
    for (first = 0u; first < count; first += 256u) {
        size_t block = count - first < 256u ? count - first : 256u;
        unsigned byte_lane;
        for (byte_lane = 0u; byte_lane < width; ++byte_lane) {
            size_t i;
            for (i = 0u; i < block; ++i) {
                if (offset >= input_size) {
                    free(result);
                    return SDAF_ERROR_FORMAT;
                }
                result[first + i] |= (uint64_t)input[offset++] << (byte_lane * 8u);
            }
        }
    }
    if (offset != input_size) {
        free(result);
        return SDAF_ERROR_FORMAT;
    }
    for (first = 0u; first < count; ++first)
        if ((result[first] & ~mask) != 0u) {
            free(result);
            return SDAF_ERROR_FORMAT;
        }
    *values = result;
    return SDAF_OK;
}

static sdaf_status write_canonical(const uint64_t* raw, size_t value_count,
    const sdaf_channel_info* channels, size_t channel_count, size_t lane_count,
    const sdaf_data_record* data, uint8_t* output, size_t output_size)
{
    size_t position, bit_offset = 0u, byte_offset = 0u;
    memset(output, 0, output_size);
    for (position = 0u; position < value_count; ++position) {
        size_t sample, channel_index, lane;
        uint32_t element;
        uint64_t value;
        unsigned bit;
        if (!order_item(channels, channel_count, lane_count, data, position, &sample,
                &channel_index, &element, &lane))
            return SDAF_ERROR_FORMAT;
        (void)sample;
        (void)element;
        (void)lane;
        value = raw[position] & bit_mask(channels[channel_index].logical_bits);
        if (channels[channel_index].logical_type == SDAF_LOGICAL_SIGNED
            && channels[channel_index].storage_bits > channels[channel_index].logical_bits
            && (value & (UINT64_C(1) << (channels[channel_index].logical_bits - 1u))) != 0u)
            value |= bit_mask(channels[channel_index].storage_bits)
                & ~bit_mask(channels[channel_index].logical_bits);
        if (data->packing == SDAF_PACKING_LSB0_DENSE) {
            for (bit = 0u; bit < channels[channel_index].storage_bits; ++bit) {
                if ((value & (UINT64_C(1) << bit)) != 0u)
                    output[bit_offset >> 3u] |= (uint8_t)(1u << (bit_offset & 7u));
                ++bit_offset;
            }
        } else {
            unsigned bytes = (channels[channel_index].storage_bits + 7u) / 8u, b;
            for (b = 0u; b < bytes; ++b)
                output[byte_offset++] = (uint8_t)(value >> (b * 8u));
        }
    }
    return SDAF_OK;
}

sdaf_status sdaf_decode_transforms(const uint8_t* encoded, size_t encoded_size,
    const sdaf_transform* transforms, size_t transform_count, const sdaf_channel_info* channels,
    size_t channel_count, size_t lane_count, const sdaf_data_record* data, uint8_t** decoded,
    size_t* decoded_size, char* error, size_t error_size)
{
    size_t expected = (size_t)data->decoded_sample_bytes;
    if (transform_count == 0u) {
        *decoded = sdaf_memdup(encoded, encoded_size);
        if (*decoded == NULL)
            return SDAF_ERROR_MEMORY;
        *decoded_size = encoded_size;
        return SDAF_OK;
    }
    if (transform_count == 1u && transforms[0].id == SDAF_TRANSFORM_ZSTD) {
        sdaf_status s
            = sdaf_zstd_decompress(encoded, encoded_size, expected, decoded, error, error_size);
        if (s == SDAF_OK)
            *decoded_size = expected;
        return s;
    }
    if (transform_count == 4u && transforms[0].id == SDAF_TRANSFORM_DELTA
        && transforms[1].id == SDAF_TRANSFORM_ZIGZAG && transforms[2].id == SDAF_TRANSFORM_SHUFFLE
        && transforms[3].id == SDAF_TRANSFORM_ZSTD) {
        size_t value_count, width, intermediate_size, sample_bytes;
        uint8_t *intermediate = NULL, *canonical = NULL;
        uint64_t *zigzag = NULL, *previous = NULL, *raw = NULL;
        size_t position;
        unsigned bits;
        sdaf_status status;
        if (channel_count == 0u)
            return SDAF_ERROR_FORMAT;
        bits = channels[0].logical_bits;
        for (position = 0u; position < channel_count; ++position)
            if ((channels[position].logical_type != SDAF_LOGICAL_UNSIGNED
                    && channels[position].logical_type != SDAF_LOGICAL_SIGNED)
                || channels[position].logical_bits != bits) {
                sdaf_set_error(
                    error, error_size, "numeric profile requires homogeneous integer channels");
                return SDAF_ERROR_FORMAT;
            }
        if (!sdaf_mul_size(data->sample_count, lane_count, &value_count))
            return SDAF_ERROR_LIMIT;
        width = (bits + 7u) / 8u;
        if (!sdaf_mul_size(value_count, width, &sample_bytes)
            || !sdaf_add_size(data->timestamp_bytes, sample_bytes, &intermediate_size))
            return SDAF_ERROR_LIMIT;
        status = sdaf_zstd_decompress(
            encoded, encoded_size, intermediate_size, &intermediate, error, error_size);
        if (status != SDAF_OK)
            return status;
        status = unshuffle(intermediate + data->timestamp_bytes, sample_bytes, value_count,
            (unsigned)width, bits, &zigzag);
        if (status != SDAF_OK) {
            free(intermediate);
            sdaf_set_error(error, error_size, "invalid shuffled numeric payload");
            return status;
        }
        previous = (uint64_t*)sdaf_calloc_array(lane_count, sizeof(*previous));
        raw = (uint64_t*)sdaf_calloc_array(value_count, sizeof(*raw));
        canonical = (uint8_t*)malloc(expected == 0u ? 1u : expected);
        if (previous == NULL || raw == NULL || canonical == NULL) {
            free(intermediate);
            free(zigzag);
            free(previous);
            free(raw);
            free(canonical);
            return SDAF_ERROR_MEMORY;
        }
        memcpy(canonical, intermediate, data->timestamp_bytes);
        for (position = 0u; position < value_count; ++position) {
            size_t sample, channel_index, lane;
            uint32_t element;
            uint64_t z = zigzag[position];
            uint64_t delta = ((z >> 1u) ^ (uint64_t)-(int64_t)(z & 1u)) & bit_mask(bits);
            if (!order_item(channels, channel_count, lane_count, data, position, &sample,
                    &channel_index, &element, &lane)) {
                status = SDAF_ERROR_FORMAT;
                break;
            }
            (void)sample;
            (void)channel_index;
            (void)element;
            raw[position] = previous[lane] = (previous[lane] + delta) & bit_mask(bits);
        }
        if (status == SDAF_OK)
            status = write_canonical(raw, value_count, channels, channel_count, lane_count, data,
                canonical + data->timestamp_bytes, expected - data->timestamp_bytes);
        free(intermediate);
        free(zigzag);
        free(previous);
        free(raw);
        if (status != SDAF_OK) {
            free(canonical);
            return status;
        }
        *decoded = canonical;
        *decoded_size = expected;
        return SDAF_OK;
    }
    return SDAF_ERROR_UNSUPPORTED;
}

static sdaf_status extract_raw(const uint8_t* canonical, size_t canonical_size,
    const sdaf_channel_info* channels, size_t channel_count, size_t lane_count,
    const sdaf_data_info* info, uint64_t** values, size_t* count)
{
    sdaf_data_record data;
    size_t value_count, position, bit_offset = 0u, byte_offset = 0u;
    int ok = 1;
    uint64_t* result;
    memset(&data, 0, sizeof(data));
    data.sample_count = info->sample_count;
    data.layout = info->layout;
    data.packing = info->packing;
    if (!sdaf_mul_size(info->sample_count, lane_count, &value_count))
        return SDAF_ERROR_LIMIT;
    result = (uint64_t*)sdaf_calloc_array(value_count, sizeof(*result));
    if (result == NULL)
        return SDAF_ERROR_MEMORY;
    for (position = 0u; position < value_count; ++position) {
        size_t sample, channel_index, lane;
        uint32_t element;
        if (!order_item(channels, channel_count, lane_count, &data, position, &sample,
                &channel_index, &element, &lane)) {
            free(result);
            return SDAF_ERROR_FORMAT;
        }
        (void)sample;
        (void)element;
        (void)lane;
        result[position] = info->packing == SDAF_PACKING_LSB0_DENSE
            ? read_bits(
                  canonical, canonical_size, &bit_offset, channels[channel_index].storage_bits, &ok)
            : read_aligned(canonical, canonical_size, &byte_offset,
                  channels[channel_index].storage_bits, &ok);
        if (!ok) {
            free(result);
            return SDAF_ERROR_FORMAT;
        }
        result[position] &= bit_mask(channels[channel_index].logical_bits);
    }
    *values = result;
    *count = value_count;
    return SDAF_OK;
}

sdaf_status sdaf_encode_numeric(const uint8_t* canonical, size_t canonical_size,
    const sdaf_channel_info* channels, size_t channel_count, size_t lane_count,
    const sdaf_data_info* info, uint8_t** encoded, size_t* encoded_size, char* error,
    size_t error_size)
{
    uint64_t *raw = NULL, *previous = NULL, *zigzag = NULL;
    size_t count = 0u, position, width, shuffled_size, output_offset = 0u;
    uint8_t* intermediate;
    unsigned bits;
    sdaf_data_record data;
    sdaf_status status;
    if (channel_count == 0u)
        return SDAF_ERROR_ARGUMENT;
    bits = channels[0].logical_bits;
    for (position = 0u; position < channel_count; ++position)
        if ((channels[position].logical_type != SDAF_LOGICAL_UNSIGNED
                && channels[position].logical_type != SDAF_LOGICAL_SIGNED)
            || channels[position].logical_bits != bits) {
            sdaf_set_error(
                error, error_size, "numeric profile requires homogeneous integer channels");
            return SDAF_ERROR_ARGUMENT;
        }
    status = extract_raw(canonical + info->timestamp_bytes, canonical_size - info->timestamp_bytes,
        channels, channel_count, lane_count, info, &raw, &count);
    if (status != SDAF_OK)
        return status;
    previous = (uint64_t*)sdaf_calloc_array(lane_count, sizeof(*previous));
    zigzag = (uint64_t*)sdaf_calloc_array(count, sizeof(*zigzag));
    if (previous == NULL || zigzag == NULL) {
        free(raw);
        free(previous);
        free(zigzag);
        return SDAF_ERROR_MEMORY;
    }
    memset(&data, 0, sizeof(data));
    data.sample_count = info->sample_count;
    data.layout = info->layout;
    for (position = 0u; position < count; ++position) {
        size_t sample, channel_index, lane;
        uint32_t element;
        uint64_t delta;
        int64_t signed_delta;
        if (!order_item(channels, channel_count, lane_count, &data, position, &sample,
                &channel_index, &element, &lane)) {
            free(raw);
            free(previous);
            free(zigzag);
            return SDAF_ERROR_FORMAT;
        }
        (void)sample;
        (void)channel_index;
        (void)element;
        delta = (raw[position] - previous[lane]) & bit_mask(bits);
        previous[lane] = raw[position];
        signed_delta = sign_extend(delta, bits);
        zigzag[position]
            = (((uint64_t)signed_delta << 1u) ^ (uint64_t)(signed_delta >> 63u)) & bit_mask(bits);
    }
    width = (bits + 7u) / 8u;
    if (!sdaf_mul_size(count, width, &shuffled_size)) {
        free(raw);
        free(previous);
        free(zigzag);
        return SDAF_ERROR_LIMIT;
    }
    intermediate = (uint8_t*)malloc(info->timestamp_bytes + shuffled_size);
    if (intermediate == NULL) {
        free(raw);
        free(previous);
        free(zigzag);
        return SDAF_ERROR_MEMORY;
    }
    memcpy(intermediate, canonical, info->timestamp_bytes);
    output_offset = info->timestamp_bytes;
    for (position = 0u; position < count; position += 256u) {
        size_t block = count - position < 256u ? count - position : 256u, byte_lane;
        for (byte_lane = 0u; byte_lane < width; ++byte_lane) {
            size_t i;
            for (i = 0u; i < block; ++i)
                intermediate[output_offset++] = (uint8_t)(zigzag[position + i] >> (byte_lane * 8u));
        }
    }
    status
        = sdaf_zstd_compress(intermediate, output_offset, encoded, encoded_size, error, error_size);
    free(raw);
    free(previous);
    free(zigzag);
    free(intermediate);
    return status;
}
