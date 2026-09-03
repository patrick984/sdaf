#include "sdaf_internal.h"

#include <errno.h>
#include <stdio.h>

static sdaf_status encoder_fail(sdaf_encoder *encoder, sdaf_status status, const char *message)
{
    encoder->status = status; sdaf_set_error(encoder->error, sizeof(encoder->error), "%s", message); return status;
}

sdaf_status sdaf_encoder_append(sdaf_encoder *encoder, const void *data, size_t size)
{
    size_t needed, capacity; uint8_t *grown;
    if (!sdaf_add_size(encoder->size, size, &needed)) return encoder_fail(encoder, SDAF_ERROR_LIMIT, "encoder size overflow");
    if (needed > encoder->capacity) {
        capacity = encoder->capacity == 0u ? 256u : encoder->capacity;
        while (capacity < needed) { if (capacity > SIZE_MAX / 2u) { capacity = needed; break; } capacity *= 2u; }
        grown = (uint8_t *)realloc(encoder->data, capacity); if (grown == NULL) return encoder_fail(encoder, SDAF_ERROR_MEMORY, "out of memory"); encoder->data = grown; encoder->capacity = capacity;
    }
    if (size != 0u) memcpy(encoder->data + encoder->size, data, size);
    encoder->size = needed;
    return SDAF_OK;
}

sdaf_status sdaf_encoder_init(sdaf_encoder *encoder, int64_t created_unix_ns, const uint8_t file_uuid[16])
{
    uint8_t header[64]; sdaf_status status;
    if (encoder == NULL) return SDAF_ERROR_ARGUMENT;
    memset(encoder, 0, sizeof(*encoder)); memset(header, 0, sizeof(header));
    memcpy(header, "SDAF\r\n\x1a\n", 8u); header[8] = 1u; sdaf_put_u16(header + 10u, 64u); sdaf_put_u32(header + 12u, UINT32_C(0x12345678)); sdaf_put_i64(header + 24u, created_unix_ns); if (file_uuid != NULL) memcpy(header + 32u, file_uuid, 16u); sdaf_put_u64(header + 48u, 64u); sdaf_put_u32(header + 56u, sdaf_crc32c(header, sizeof(header)));
    status = sdaf_encoder_append(encoder, header, sizeof(header)); if (status == SDAF_OK) encoder->status = SDAF_OK; return status;
}

void sdaf_encoder_free(sdaf_encoder *encoder) { if (encoder != NULL) { free(encoder->data); memset(encoder, 0, sizeof(*encoder)); } }

sdaf_status sdaf_encoder_record(sdaf_encoder *encoder, uint16_t type, uint16_t type_flags, const uint8_t *type_header, size_t type_header_size, const uint8_t *payload, size_t payload_size, int trailer_crc)
{
    uint8_t *header; size_t header_size; uint32_t payload_crc; sdaf_status status;
    if (encoder == NULL || encoder->data == NULL || (type_header == NULL && type_header_size != 0u) || (payload == NULL && payload_size != 0u) || type_header_size > UINT16_MAX - 32u) return SDAF_ERROR_ARGUMENT;
    header_size = 32u + type_header_size; header = (uint8_t *)calloc(header_size, 1u); if (header == NULL) return encoder_fail(encoder, SDAF_ERROR_MEMORY, "out of memory");
    memcpy(header, "SDRC", 4u); sdaf_put_u16(header + 4u, type); sdaf_put_u16(header + 6u, (uint16_t)(type_flags | (trailer_crc ? 1u : 0u))); sdaf_put_u16(header + 8u, (uint16_t)header_size); header[10] = 1u; sdaf_put_u32(header + 12u, encoder->next_sequence); sdaf_put_u64(header + 16u, payload_size); payload_crc = sdaf_crc32c(payload, payload_size); if (!trailer_crc) sdaf_put_u32(header + 28u, payload_crc); if (type_header_size != 0u) memcpy(header + 32u, type_header, type_header_size); sdaf_put_u32(header + 24u, sdaf_crc32c(header, header_size));
    status = sdaf_encoder_append(encoder, header, header_size); free(header); if (status != SDAF_OK) return status;
    status = sdaf_encoder_append(encoder, payload, payload_size); if (status != SDAF_OK) return status;
    if (trailer_crc) { uint8_t trailer[16]; memset(trailer, 0, sizeof(trailer)); memcpy(trailer, "SDCT", 4u); sdaf_put_u16(trailer + 4u, 16u); trailer[6] = 1u; sdaf_put_u32(trailer + 8u, encoder->next_sequence); sdaf_put_u32(trailer + 12u, payload_crc); status = sdaf_encoder_append(encoder, trailer, sizeof(trailer)); if (status != SDAF_OK) return status; }
    ++encoder->next_sequence; ++encoder->record_count; return SDAF_OK;
}

static sdaf_status schema_payload_size(const sdaf_schema *schema, size_t *size)
{
    size_t total = 0u, i;
    for (i = 0u; i < schema->object_count; ++i) {
        size_t object_size = 12u, j;
        for (j = 0u; j < schema->objects[i].tlv_count; ++j) { size_t tlv_size; if (!sdaf_add_size(8u, schema->objects[i].tlvs[j].size, &tlv_size) || !sdaf_add_size(object_size, tlv_size, &object_size)) return SDAF_ERROR_LIMIT; }
        if (!sdaf_add_size(total, object_size, &total) || object_size > UINT32_MAX) return SDAF_ERROR_LIMIT;
    }
    *size = total; return SDAF_OK;
}

sdaf_status sdaf_encoder_write_schema(sdaf_encoder *encoder, const sdaf_schema *schema, int trailer_crc)
{
    sdaf_limits limits; size_t payload_size, offset = 0u, i; uint8_t *payload, h[16]; sdaf_status status;
    if (encoder == NULL || schema == NULL) return SDAF_ERROR_ARGUMENT;
    sdaf_limits_default(&limits); status = sdaf_validate_schema(schema, &limits, encoder->error, sizeof(encoder->error)); if (status != SDAF_OK) { encoder->status = status; return status; }
    status = schema_payload_size(schema, &payload_size); if (status != SDAF_OK) return encoder_fail(encoder, status, "schema size overflow");
    payload = (uint8_t *)malloc(payload_size == 0u ? 1u : payload_size); if (payload == NULL) return encoder_fail(encoder, SDAF_ERROR_MEMORY, "out of memory");
    for (i = 0u; i < schema->object_count; ++i) {
        const sdaf_schema_object *object = &schema->objects[i]; size_t object_start = offset, j;
        memset(payload + offset, 0, 12u); payload[offset] = object->kind; sdaf_put_u16(payload + offset + 2u, 12u); sdaf_put_u32(payload + offset + 8u, object->id); offset += 12u;
        for (j = 0u; j < object->tlv_count; ++j) { const sdaf_tlv *tlv = &object->tlvs[j]; memset(payload + offset, 0, 8u); sdaf_put_u16(payload + offset, tlv->tag); payload[offset + 2u] = tlv->wire_type; sdaf_put_u32(payload + offset + 4u, tlv->size); if (tlv->size != 0u) memcpy(payload + offset + 8u, tlv->value, tlv->size); offset += 8u + tlv->size; }
        sdaf_put_u32(payload + object_start + 4u, (uint32_t)(offset - object_start));
    }
    memset(h, 0, sizeof(h)); sdaf_put_u32(h, schema->id); sdaf_put_u32(h + 4u, schema->revision); sdaf_put_u32(h + 8u, (uint32_t)schema->object_count);
    status = sdaf_encoder_record(encoder, SDAF_RECORD_SCHEMA, 0u, h, sizeof(h), payload, payload_size, trailer_crc); free(payload); return status;
}

static sdaf_status validate_data_info(const sdaf_data_info *info, char *error, size_t error_size)
{
    uint64_t timestamp_size = (uint64_t)info->sample_count * 8u;
    if (info->schema_id == 0u || info->schema_revision == 0u || info->stream_id == 0u || info->timestamp_mode < 1u || info->timestamp_mode > 4u || info->layout < 1u || info->layout > 2u || info->packing < 1u || info->packing > 2u) goto invalid;
    if (info->timestamp_mode == SDAF_TIMESTAMP_PERIODIC && (info->period_denominator == 0u || info->timestamp_bytes != 0u)) goto invalid;
    if ((info->timestamp_mode == SDAF_TIMESTAMP_DELTA || info->timestamp_mode == SDAF_TIMESTAMP_EXPLICIT) && info->timestamp_bytes != timestamp_size) goto invalid;
    if (info->timestamp_mode == SDAF_TIMESTAMP_NONE && (info->start_time_ticks != 0 || info->period_numerator != 0u || info->period_denominator != 0u || info->timestamp_bytes != 0u)) goto invalid;
    return SDAF_OK;
invalid: sdaf_set_error(error, error_size, "invalid DATA options"); return SDAF_ERROR_ARGUMENT;
}

static sdaf_status add_descriptors(const uint16_t *ids, size_t count, const uint8_t *encoded, size_t encoded_size, uint8_t **payload, size_t *payload_size)
{
    size_t descriptor_size, total, i; uint8_t *result;
    if (!sdaf_mul_size(count, 8u, &descriptor_size) || !sdaf_add_size(descriptor_size, encoded_size, &total)) return SDAF_ERROR_LIMIT;
    result = (uint8_t *)calloc(total == 0u ? 1u : total, 1u); if (result == NULL) return SDAF_ERROR_MEMORY;
    for (i = 0u; i < count; ++i) { sdaf_put_u16(result + i * 8u, ids[i]); result[i * 8u + 2u] = 1u; }
    if (encoded_size != 0u) memcpy(result + descriptor_size, encoded, encoded_size);
    *payload = result; *payload_size = total; return SDAF_OK;
}

sdaf_status sdaf_encoder_write_data(sdaf_encoder *encoder, const sdaf_schema *schema, const sdaf_data_info *info, const void *canonical_payload, size_t payload_size)
{
    sdaf_limits limits; sdaf_channel_info *channels = NULL; size_t channel_count = 0u, lane_count = 0u, expected; uint8_t *encoded = NULL, *payload = NULL; size_t encoded_size = 0u, stored_size = 0u; uint16_t ids[4]; size_t transform_count = 0u; uint8_t h[64]; sdaf_status status; uint8_t kind = 1u; const sdaf_schema_object *stream;
    if (encoder == NULL || schema == NULL || info == NULL || (canonical_payload == NULL && payload_size != 0u)) return SDAF_ERROR_ARGUMENT;
    status = validate_data_info(info, encoder->error, sizeof(encoder->error)); if (status != SDAF_OK) return status;
    if (schema->id != info->schema_id || schema->revision != info->schema_revision) return encoder_fail(encoder, SDAF_ERROR_ARGUMENT, "DATA schema does not match options");
    stream = sdaf_schema_find_object(schema, SDAF_OBJECT_STREAM, info->stream_id); if (stream == NULL) return encoder_fail(encoder, SDAF_ERROR_ARGUMENT, "DATA stream does not exist"); (void)sdaf_tlv_u8(sdaf_object_find_tlv(stream, 106u, 0u), &kind); if ((kind != 1u && kind != 4u) || (kind == 4u && info->timestamp_mode != SDAF_TIMESTAMP_NONE)) return encoder_fail(encoder, SDAF_ERROR_ARGUMENT, "DATA stream kind is incompatible");
    sdaf_limits_default(&limits); status = sdaf_channels_for_stream(schema, info->stream_id, &limits, &channels, &channel_count, &lane_count, encoder->error, sizeof(encoder->error)); if (status != SDAF_OK) return status;
    status = sdaf_expected_payload_size(channels, channel_count, info->sample_count, info->timestamp_bytes, info->packing, &expected); if (status != SDAF_OK || expected != payload_size) { free(channels); return encoder_fail(encoder, SDAF_ERROR_ARGUMENT, "canonical DATA payload size does not match schema"); }
    if (info->compression == SDAF_COMPRESSION_NONE) { encoded = sdaf_memdup((const uint8_t *)canonical_payload, payload_size); encoded_size = payload_size; }
    else if (info->compression == SDAF_COMPRESSION_ZSTANDARD) { ids[0] = 16u; transform_count = 1u; status = sdaf_zstd_compress((const uint8_t *)canonical_payload, payload_size, &encoded, &encoded_size, encoder->error, sizeof(encoder->error)); if (status != SDAF_OK) { free(channels); return status; } }
    else if (info->compression == SDAF_COMPRESSION_NUMERIC) { ids[0] = 1u; ids[1] = 2u; ids[2] = 3u; ids[3] = 16u; transform_count = 4u; status = sdaf_encode_numeric((const uint8_t *)canonical_payload, payload_size, channels, channel_count, lane_count, info, &encoded, &encoded_size, encoder->error, sizeof(encoder->error)); if (status != SDAF_OK) { free(channels); return status; } }
    else { free(channels); return encoder_fail(encoder, SDAF_ERROR_ARGUMENT, "unknown DATA compression mode"); }
    free(channels); if (encoded == NULL) return encoder_fail(encoder, SDAF_ERROR_MEMORY, "out of memory");
    status = add_descriptors(ids, transform_count, encoded, encoded_size, &payload, &stored_size); free(encoded); if (status != SDAF_OK) return status;
    memset(h, 0, sizeof(h)); sdaf_put_u32(h, info->schema_id); sdaf_put_u32(h + 4u, info->schema_revision); sdaf_put_u32(h + 8u, info->stream_id); sdaf_put_u32(h + 12u, info->sample_count); sdaf_put_u64(h + 16u, info->first_sample_index); sdaf_put_i64(h + 24u, info->start_time_ticks); sdaf_put_u64(h + 32u, info->period_numerator); sdaf_put_u64(h + 40u, info->period_denominator); h[48] = info->timestamp_mode; h[49] = info->layout; h[50] = info->packing; h[51] = (uint8_t)transform_count; sdaf_put_u32(h + 52u, info->timestamp_bytes); sdaf_put_u64(h + 56u, payload_size);
    status = sdaf_encoder_record(encoder, SDAF_RECORD_DATA, 0u, h, sizeof(h), payload, stored_size, info->payload_crc_in_trailer); free(payload); if (status == SDAF_OK) ++encoder->data_record_count; return status;
}

sdaf_status sdaf_encoder_write_text(sdaf_encoder *encoder, const sdaf_text_info *info, const char *message, size_t message_size)
{
    uint8_t h[32]; uint16_t flags = 0u;
    if (encoder == NULL || info == NULL || (message == NULL && message_size != 0u) || info->severity > 6u || !sdaf_utf8_valid((const uint8_t *)message, message_size)) return SDAF_ERROR_ARGUMENT;
    if (info->schema_id == 0u) { if (info->schema_revision != 0u || info->stream_id != 0u || info->has_source_id || info->has_event_code) return encoder_fail(encoder, SDAF_ERROR_ARGUMENT, "invalid global TEXT options"); }
    else if (info->schema_revision == 0u || info->stream_id == 0u) return encoder_fail(encoder, SDAF_ERROR_ARGUMENT, "incomplete stream TEXT references");
    memset(h, 0, sizeof(h)); if (info->has_source_id) flags |= 0x0100u; if (info->has_event_code) flags |= 0x0200u; sdaf_put_u32(h, info->schema_id); sdaf_put_u32(h + 4u, info->stream_id); sdaf_put_i64(h + 8u, info->time_ticks); h[16] = info->severity; h[17] = 1u; if (info->has_source_id) sdaf_put_u32(h + 20u, info->source_id); if (info->has_event_code) sdaf_put_i32(h + 24u, info->event_code); sdaf_put_u32(h + 28u, info->schema_revision);
    return sdaf_encoder_record(encoder, SDAF_RECORD_TEXT, flags, h, sizeof(h), (const uint8_t *)message, message_size, info->payload_crc_in_trailer);
}

sdaf_status sdaf_encoder_write_blob(sdaf_encoder *encoder, const sdaf_blob_info *info, const void *decoded_payload, size_t payload_size)
{
    uint8_t h[48], *encoded = NULL, *stored = NULL; size_t encoded_size = 0u, stored_size = 0u; uint16_t id = 16u; size_t count = 0u; sdaf_status status;
    if (encoder == NULL || info == NULL || (decoded_payload == NULL && payload_size != 0u) || info->schema_id == 0u || info->schema_revision == 0u || info->stream_id == 0u || info->compression == SDAF_COMPRESSION_NUMERIC) return SDAF_ERROR_ARGUMENT;
    if (info->compression == SDAF_COMPRESSION_ZSTANDARD) { count = 1u; status = sdaf_zstd_compress((const uint8_t *)decoded_payload, payload_size, &encoded, &encoded_size, encoder->error, sizeof(encoder->error)); if (status != SDAF_OK) return status; }
    else if (info->compression == SDAF_COMPRESSION_NONE) { encoded = sdaf_memdup((const uint8_t *)decoded_payload, payload_size); encoded_size = payload_size; }
    else return SDAF_ERROR_ARGUMENT;
    if (encoded == NULL) return SDAF_ERROR_MEMORY;
    status = add_descriptors(&id, count, encoded, encoded_size, &stored, &stored_size); free(encoded); if (status != SDAF_OK) return status;
    memset(h, 0, sizeof(h)); sdaf_put_u32(h, info->schema_id); sdaf_put_u32(h + 4u, info->schema_revision); sdaf_put_u32(h + 8u, info->stream_id); sdaf_put_u64(h + 16u, info->item_index); sdaf_put_i64(h + 24u, info->time_ticks); sdaf_put_u64(h + 32u, payload_size); h[40] = (uint8_t)count;
    status = sdaf_encoder_record(encoder, SDAF_RECORD_BLOB, 0u, h, sizeof(h), stored, stored_size, info->payload_crc_in_trailer); free(stored); return status;
}

sdaf_status sdaf_encoder_write_note(sdaf_encoder *encoder, const char *message, size_t message_size, int trailer_crc)
{
    if (encoder == NULL || (message == NULL && message_size != 0u) || !sdaf_utf8_valid((const uint8_t *)message, message_size)) return SDAF_ERROR_ARGUMENT;
    return sdaf_encoder_record(encoder, SDAF_RECORD_NOTE, 0u, NULL, 0u, (const uint8_t *)message, message_size, trailer_crc);
}

sdaf_status sdaf_encoder_write_index(sdaf_encoder *encoder, const sdaf_index_entry *entries, size_t entry_count, int trailer_crc)
{
    uint8_t h[16], *payload; size_t payload_size, i; sdaf_status status;
    if (encoder == NULL || (entries == NULL && entry_count != 0u) || entry_count > UINT32_MAX || !sdaf_mul_size(entry_count, 48u, &payload_size)) return SDAF_ERROR_ARGUMENT;
    payload = (uint8_t *)calloc(payload_size == 0u ? 1u : payload_size, 1u); if (payload == NULL) return SDAF_ERROR_MEMORY;
    for (i = 0u; i < entry_count; ++i) { uint8_t *p = payload + i * 48u; sdaf_put_u64(p, entries[i].record_offset); sdaf_put_u32(p + 8u, entries[i].sequence); sdaf_put_u32(p + 12u, entries[i].stream_id); sdaf_put_u64(p + 16u, entries[i].first_sample_index); sdaf_put_u32(p + 24u, entries[i].sample_count); sdaf_put_i64(p + 32u, entries[i].first_time_ticks); sdaf_put_i64(p + 40u, entries[i].last_time_ticks); }
    memset(h, 0, sizeof(h)); sdaf_put_u32(h, (uint32_t)entry_count); status = sdaf_encoder_record(encoder, SDAF_RECORD_INDEX, 0u, h, sizeof(h), payload, payload_size, trailer_crc); free(payload); return status;
}

sdaf_status sdaf_encoder_write_end(sdaf_encoder *encoder, uint64_t last_index_offset)
{
    uint8_t h[32]; if (encoder == NULL) return SDAF_ERROR_ARGUMENT; memset(h, 0, sizeof(h)); sdaf_put_u64(h, encoder->record_count + 1u); sdaf_put_u64(h + 8u, encoder->data_record_count); sdaf_put_u64(h + 16u, last_index_offset); return sdaf_encoder_record(encoder, SDAF_RECORD_END, 0u, h, sizeof(h), NULL, 0u, 0);
}

sdaf_status sdaf_encoder_write_private(sdaf_encoder *encoder, uint16_t record_type, const void *type_header, size_t type_header_size, const void *payload, size_t payload_size, int trailer_crc)
{
    if (record_type < 0x8000u) return SDAF_ERROR_ARGUMENT;
    return sdaf_encoder_record(encoder, record_type, 0u, (const uint8_t *)type_header, type_header_size, (const uint8_t *)payload, payload_size, trailer_crc);
}

sdaf_status sdaf_encoder_write_file(const sdaf_encoder *encoder, const char *path)
{
    FILE *file; size_t written;
    if (encoder == NULL || path == NULL || encoder->data == NULL) return SDAF_ERROR_ARGUMENT;
    file = fopen(path, "wb"); if (file == NULL) return SDAF_ERROR_IO; written = fwrite(encoder->data, 1u, encoder->size, file); if (fclose(file) != 0 || written != encoder->size) return SDAF_ERROR_IO; return SDAF_OK;
}
