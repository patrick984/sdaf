#include "sdaf_internal.h"

#include <errno.h>
#include <stdio.h>

static const uint8_t sdaf_magic[8] = { 0x53u, 0x44u, 0x41u, 0x46u, 0x0du, 0x0au, 0x1au, 0x0au };

static sdaf_status fail_at(
    sdaf_document* doc, sdaf_status status, uint64_t offset, const char* message)
{
    doc->status = status;
    doc->error_offset = offset;
    sdaf_set_error(doc->error, sizeof(doc->error), "%s", message);
    return status;
}

static void free_record(sdaf_record* record)
{
    size_t i;
    switch (record->envelope.type) {
    case SDAF_RECORD_SCHEMA:
        sdaf_free_schema(&record->value.schema);
        break;
    case SDAF_RECORD_DATA:
        sdaf_free_transforms(record->value.data.transforms, record->value.data.transform_count);
        free(record->value.data.stored_payload);
        free(record->value.data.decoded_payload);
        if (record->value.data.samples != NULL)
            for (i = 0u; i < record->value.data.sample_count; ++i) {
                size_t j;
                for (j = 0u; j < record->value.data.samples[i].value_count; ++j)
                    free(record->value.data.samples[i].values[j].bytes);
                free(record->value.data.samples[i].values);
            }
        free(record->value.data.samples);
        break;
    case SDAF_RECORD_TEXT:
        free(record->value.text.message);
        break;
    case SDAF_RECORD_BLOB:
        sdaf_free_transforms(record->value.blob.transforms, record->value.blob.transform_count);
        free(record->value.blob.stored_payload);
        free(record->value.blob.decoded_payload);
        break;
    case SDAF_RECORD_INDEX:
        free(record->value.index.entries);
        break;
    case SDAF_RECORD_END:
        break;
    case SDAF_RECORD_NOTE:
        free(record->value.note.message);
        break;
    default:
        free(record->value.unknown.type_header);
        free(record->value.unknown.payload);
        break;
    }
    memset(record, 0, sizeof(*record));
}

void sdaf_document_free(sdaf_document* document)
{
    size_t i;
    if (document == NULL)
        return;
    for (i = 0u; i < document->record_count; ++i)
        free_record(&document->records[i]);
    free(document->records);
    memset(document, 0, sizeof(*document));
}

static sdaf_status append_record(sdaf_document* doc, sdaf_record* record, const sdaf_limits* limits)
{
    sdaf_record* grown;
    size_t count, bytes;
    if (doc->record_count >= limits->max_records || !sdaf_add_size(doc->record_count, 1u, &count)
        || !sdaf_mul_size(count, sizeof(*grown), &bytes))
        return SDAF_ERROR_LIMIT;
    grown = (sdaf_record*)realloc(doc->records, bytes);
    if (grown == NULL)
        return SDAF_ERROR_MEMORY;
    doc->records = grown;
    doc->records[doc->record_count] = *record;
    doc->record_count = count;
    memset(record, 0, sizeof(*record));
    return SDAF_OK;
}

static sdaf_status copy_tlv_value(sdaf_tlv* tlv, const uint8_t* value)
{
    size_t allocation = tlv->size;
    if (tlv->wire_type == SDAF_WIRE_UTF8) {
        if (!sdaf_add_size(allocation, 1u, &allocation))
            return SDAF_ERROR_LIMIT;
        tlv->value = (uint8_t*)malloc(allocation);
        if (tlv->value != NULL)
            tlv->value[tlv->size] = 0u;
    } else
        tlv->value = (uint8_t*)malloc(allocation == 0u ? 1u : allocation);
    if (tlv->value == NULL)
        return SDAF_ERROR_MEMORY;
    if (tlv->size != 0u)
        memcpy(tlv->value, value, tlv->size);
    return SDAF_OK;
}

static sdaf_status parse_tlvs(const uint8_t* body, size_t body_size, sdaf_schema_object* object,
    char* error, size_t error_size)
{
    size_t offset = 0u, count = 0u, index = 0u;
    while (offset < body_size) {
        uint32_t size;
        if (body_size - offset < 8u) {
            sdaf_set_error(error, error_size, "TLV header overruns its object");
            return SDAF_ERROR_FORMAT;
        }
        size = sdaf_get_u32(body + offset + 4u);
        if (body[offset + 3u] != 0u || (uint64_t)size > body_size - offset - 8u) {
            sdaf_set_error(error, error_size, "TLV value overruns its object or has flags set");
            return SDAF_ERROR_FORMAT;
        }
        ++count;
        offset += 8u + size;
    }
    object->tlvs = (sdaf_tlv*)sdaf_calloc_array(count, sizeof(*object->tlvs));
    if (object->tlvs == NULL)
        return SDAF_ERROR_MEMORY;
    object->tlv_count = count;
    offset = 0u;
    while (offset < body_size) {
        sdaf_tlv* tlv = &object->tlvs[index++];
        sdaf_status status;
        tlv->tag = sdaf_get_u16(body + offset);
        tlv->wire_type = body[offset + 2u];
        tlv->size = sdaf_get_u32(body + offset + 4u);
        status = copy_tlv_value(tlv, body + offset + 8u);
        if (status != SDAF_OK)
            return status;
        offset += 8u + tlv->size;
    }
    return SDAF_OK;
}

static int schemas_equal(const sdaf_schema* left, const sdaf_schema* right)
{
    size_t i;
    if (left->id != right->id || left->revision != right->revision
        || left->object_count != right->object_count)
        return 0;
    for (i = 0u; i < left->object_count; ++i) {
        const sdaf_schema_object *a = &left->objects[i], *b = &right->objects[i];
        size_t j;
        if (a->kind != b->kind || a->id != b->id || a->tlv_count != b->tlv_count)
            return 0;
        for (j = 0u; j < a->tlv_count; ++j)
            if (a->tlvs[j].tag != b->tlvs[j].tag || a->tlvs[j].wire_type != b->tlvs[j].wire_type
                || a->tlvs[j].size != b->tlvs[j].size
                || memcmp(a->tlvs[j].value, b->tlvs[j].value, a->tlvs[j].size) != 0)
                return 0;
    }
    return 1;
}

static sdaf_status parse_schema(const uint8_t* h, const uint8_t* payload, size_t payload_size,
    const sdaf_limits* limits, const sdaf_document* doc, sdaf_record* record, char* error,
    size_t error_size)
{
    sdaf_schema* schema = &record->value.schema;
    uint32_t object_count = sdaf_get_u32(h + 8u);
    size_t offset = 0u, i;
    if (record->envelope.header_size != 48u || (record->envelope.flags & 0xff00u) != 0u
        || sdaf_get_u32(h + 12u) != 0u || sdaf_get_u32(h) == 0u || sdaf_get_u32(h + 4u) == 0u) {
        sdaf_set_error(error, error_size, "invalid SCMA header");
        return SDAF_ERROR_FORMAT;
    }
    if (object_count > limits->max_schema_objects) {
        sdaf_set_error(error, error_size, "SCMA exceeds the object limit");
        return SDAF_ERROR_LIMIT;
    }
    schema->id = sdaf_get_u32(h);
    schema->revision = sdaf_get_u32(h + 4u);
    schema->object_count = object_count;
    schema->objects
        = (sdaf_schema_object*)sdaf_calloc_array(object_count, sizeof(*schema->objects));
    if (schema->objects == NULL)
        return SDAF_ERROR_MEMORY;
    for (i = 0u; i < object_count; ++i) {
        sdaf_schema_object* object = &schema->objects[i];
        uint32_t object_size;
        sdaf_status status;
        if (payload_size - offset < 12u) {
            sdaf_set_error(error, error_size, "schema object header overruns payload");
            return SDAF_ERROR_FORMAT;
        }
        object_size = sdaf_get_u32(payload + offset + 4u);
        if (payload[offset] < 1u || payload[offset] > 6u || payload[offset + 1u] != 0u
            || sdaf_get_u16(payload + offset + 2u) != 12u || object_size < 12u
            || object_size > payload_size - offset || sdaf_get_u32(payload + offset + 8u) == 0u) {
            sdaf_set_error(error, error_size, "invalid schema object envelope");
            return SDAF_ERROR_FORMAT;
        }
        object->kind = payload[offset];
        object->id = sdaf_get_u32(payload + offset + 8u);
        status = parse_tlvs(payload + offset + 12u, object_size - 12u, object, error, error_size);
        if (status != SDAF_OK)
            return status;
        offset += object_size;
    }
    if (offset != payload_size) {
        sdaf_set_error(error, error_size, "object_count does not consume SCMA payload");
        return SDAF_ERROR_FORMAT;
    }
    {
        const sdaf_schema* prior;
        sdaf_status status = sdaf_validate_schema(schema, limits, error, error_size);
        if (status != SDAF_OK)
            return status;
        prior = sdaf_find_schema(doc, schema->id, schema->revision);
        if (prior != NULL && !schemas_equal(prior, schema)) {
            sdaf_set_error(
                error, error_size, "schema revision was redefined with different content");
            return SDAF_ERROR_FORMAT;
        }
    }
    return SDAF_OK;
}

static sdaf_status parse_transforms(const uint8_t* payload, size_t payload_size, size_t count,
    sdaf_transform** transforms, size_t* encoded_offset, char* error, size_t error_size)
{
    sdaf_transform* result = (sdaf_transform*)sdaf_calloc_array(count, sizeof(*result));
    size_t offset = 0u, i;
    if (result == NULL)
        return SDAF_ERROR_MEMORY;
    for (i = 0u; i < count; ++i) {
        uint32_t parameter_size;
        sdaf_transform* transform = &result[i];
        if (payload_size - offset < 8u) {
            sdaf_set_error(error, error_size, "transform descriptor overruns payload");
            sdaf_free_transforms(result, count);
            return SDAF_ERROR_FORMAT;
        }
        parameter_size = sdaf_get_u32(payload + offset + 4u);
        if (payload[offset + 3u] != 0u || parameter_size > payload_size - offset - 8u) {
            sdaf_set_error(error, error_size, "invalid transform flags or parameter size");
            sdaf_free_transforms(result, count);
            return SDAF_ERROR_FORMAT;
        }
        transform->id = sdaf_get_u16(payload + offset);
        transform->version = payload[offset + 2u];
        transform->parameter_size = parameter_size;
        transform->parameters = sdaf_memdup(payload + offset + 8u, parameter_size);
        if (transform->parameters == NULL) {
            sdaf_free_transforms(result, count);
            return SDAF_ERROR_MEMORY;
        }
        if ((transform->id == 1u || transform->id == 2u || transform->id == 3u
                || transform->id == 16u)
            && (transform->version != 1u || parameter_size != 0u)) {
            sdaf_set_error(
                error, error_size, "known transform has unsupported version or parameters");
            sdaf_free_transforms(result, count);
            return SDAF_ERROR_FORMAT;
        }
        offset += 8u + parameter_size;
    }
    *transforms = result;
    *encoded_offset = offset;
    return SDAF_OK;
}

static int stream_kind(const sdaf_schema* schema, uint32_t stream_id, uint8_t* kind)
{
    const sdaf_schema_object* stream
        = sdaf_schema_find_object(schema, SDAF_OBJECT_STREAM, stream_id);
    if (stream == NULL)
        return 0;
    *kind = 1u;
    (void)sdaf_tlv_u8(sdaf_object_find_tlv(stream, 106u, 0u), kind);
    return 1;
}

static sdaf_status validate_timestamp_header(
    const sdaf_data_record* data, char* error, size_t error_size)
{
    uint64_t expected = (uint64_t)data->sample_count * 8u;
    if (data->timestamp_mode == SDAF_TIMESTAMP_PERIODIC
        && (data->period_denominator == 0u || data->timestamp_bytes != 0u))
        goto invalid;
    if ((data->timestamp_mode == SDAF_TIMESTAMP_DELTA
            || data->timestamp_mode == SDAF_TIMESTAMP_EXPLICIT)
        && data->timestamp_bytes != expected)
        goto invalid;
    if (data->timestamp_mode == SDAF_TIMESTAMP_NONE
        && (data->start_time_ticks != 0 || data->period_numerator != 0u
            || data->period_denominator != 0u || data->timestamp_bytes != 0u))
        goto invalid;
    return SDAF_OK;
invalid:
    sdaf_set_error(error, error_size, "invalid DATA timestamp fields");
    return SDAF_ERROR_FORMAT;
}

static int data_transforms_valid(const sdaf_transform* transforms, size_t count, int* supported)
{
    size_t i;
    int any_typed = 0;
    *supported = 0;
    if (count == 0u || (count == 1u && transforms[0].id == 16u)) {
        *supported = 1;
        return 1;
    }
    for (i = 0u; i < count; ++i)
        if (transforms[i].id >= 1u && transforms[i].id <= 3u)
            any_typed = 1;
    if (any_typed) {
        if (!(count == 4u && transforms[0].id == 1u && transforms[1].id == 2u
                && transforms[2].id == 3u && transforms[3].id == 16u))
            return 0;
        *supported = 1;
    }
    return 1;
}

static sdaf_status parse_data(const uint8_t* h, const uint8_t* payload, size_t payload_size,
    const sdaf_limits* limits, sdaf_document* doc, sdaf_record* record, char* error,
    size_t error_size)
{
    sdaf_data_record* data = &record->value.data;
    const sdaf_schema* schema;
    size_t encoded_offset = 0u, expected = 0u;
    sdaf_status status;
    int supported;
    uint8_t kind;
    sdaf_channel_info* channels = NULL;
    size_t channel_count = 0u, lane_count = 0u;
    if (record->envelope.header_size != 96u || (record->envelope.flags & 0xff00u) != 0u) {
        sdaf_set_error(error, error_size, "invalid DATA header size or flags");
        return SDAF_ERROR_FORMAT;
    }
    data->schema_id = sdaf_get_u32(h);
    data->schema_revision = sdaf_get_u32(h + 4u);
    data->stream_id = sdaf_get_u32(h + 8u);
    data->sample_count = sdaf_get_u32(h + 12u);
    data->first_sample_index = sdaf_get_u64(h + 16u);
    data->start_time_ticks = sdaf_get_i64(h + 24u);
    data->period_numerator = sdaf_get_u64(h + 32u);
    data->period_denominator = sdaf_get_u64(h + 40u);
    data->timestamp_mode = h[48];
    data->layout = h[49];
    data->packing = h[50];
    data->transform_count = h[51];
    data->timestamp_bytes = sdaf_get_u32(h + 52u);
    data->decoded_sample_bytes = sdaf_get_u64(h + 56u);
    if (data->schema_id == 0u || data->schema_revision == 0u || data->stream_id == 0u
        || data->timestamp_mode < 1u || data->timestamp_mode > 4u || data->layout < 1u
        || data->layout > 2u || data->packing < 1u || data->packing > 2u) {
        sdaf_set_error(error, error_size, "invalid DATA IDs or enum fields");
        return SDAF_ERROR_FORMAT;
    }
    if (data->decoded_sample_bytes > limits->max_decoded_size
        || data->decoded_sample_bytes > SIZE_MAX)
        return SDAF_ERROR_LIMIT;
    status = validate_timestamp_header(data, error, error_size);
    if (status != SDAF_OK)
        return status;
    status = parse_transforms(payload, payload_size, data->transform_count, &data->transforms,
        &encoded_offset, error, error_size);
    if (status != SDAF_OK)
        return status;
    if (!data_transforms_valid(data->transforms, data->transform_count, &supported)) {
        sdaf_set_error(error, error_size, "typed transforms must be exactly [1,2,3,16]");
        return SDAF_ERROR_FORMAT;
    }
    data->stored_payload = sdaf_memdup(payload, payload_size);
    if (data->stored_payload == NULL)
        return SDAF_ERROR_MEMORY;
    data->stored_payload_size = payload_size;
    schema = sdaf_find_schema(doc, data->schema_id, data->schema_revision);
    if (schema == NULL)
        return SDAF_OK;
    if (!stream_kind(schema, data->stream_id, &kind) || (kind != 1u && kind != 4u)
        || (kind == 4u && data->timestamp_mode != SDAF_TIMESTAMP_NONE)) {
        sdaf_set_error(error, error_size, "DATA record does not match its stream kind");
        return SDAF_ERROR_FORMAT;
    }
    status = sdaf_channels_for_stream(
        schema, data->stream_id, limits, &channels, &channel_count, &lane_count, error, error_size);
    if (status != SDAF_OK)
        return status;
    status = sdaf_expected_payload_size(channels, channel_count, data->sample_count,
        data->timestamp_bytes, data->packing, &expected);
    if (status != SDAF_OK) {
        free(channels);
        return status;
    }
    if (expected != data->decoded_sample_bytes) {
        free(channels);
        sdaf_set_error(error, error_size, "decoded_sample_bytes does not match schema");
        return SDAF_ERROR_FORMAT;
    }
    if (supported) {
        status = sdaf_decode_transforms(payload + encoded_offset, payload_size - encoded_offset,
            data->transforms, data->transform_count, channels, channel_count, lane_count, data,
            &data->decoded_payload, &data->decoded_payload_size, error, error_size);
        if (status == SDAF_ERROR_UNSUPPORTED) {
            error[0] = '\0';
            status = SDAF_OK;
        } else if (status == SDAF_OK && data->decoded_payload_size != expected) {
            sdaf_set_error(error, error_size, "decoded DATA size mismatch");
            status = SDAF_ERROR_FORMAT;
        }
        if (status == SDAF_OK && data->decoded_payload != NULL)
            status = sdaf_decode_data_samples(data, schema, limits, error, error_size);
    }
    free(channels);
    return status;
}

static sdaf_status parse_text(const uint8_t* h, const uint8_t* payload, size_t payload_size,
    const sdaf_limits* limits, const sdaf_document* doc, sdaf_record* record, char* error,
    size_t error_size)
{
    sdaf_text_record* text = &record->value.text;
    const sdaf_schema* schema;
    uint8_t kind;
    if (payload_size > limits->max_utf8_size) {
        sdaf_set_error(error, error_size, "TEXT exceeds the UTF-8 limit");
        return SDAF_ERROR_LIMIT;
    }
    if (record->envelope.header_size != 64u || (record->envelope.flags & 0xfc00u) != 0u
        || h[16] > 6u || h[17] != 1u || sdaf_get_u16(h + 18u) != 0u
        || !sdaf_utf8_valid(payload, payload_size)) {
        sdaf_set_error(error, error_size, "invalid TEXT header or UTF-8");
        return SDAF_ERROR_FORMAT;
    }
    text->schema_id = sdaf_get_u32(h);
    text->stream_id = sdaf_get_u32(h + 4u);
    text->time_ticks = sdaf_get_i64(h + 8u);
    text->severity = h[16];
    text->source_id = sdaf_get_u32(h + 20u);
    text->event_code = sdaf_get_i32(h + 24u);
    text->schema_revision = sdaf_get_u32(h + 28u);
    text->has_source_id = (record->envelope.flags & 0x0100u) != 0u;
    text->has_event_code = (record->envelope.flags & 0x0200u) != 0u;
    if ((!text->has_source_id && text->source_id != 0u)
        || (!text->has_event_code && text->event_code != 0))
        goto invalid;
    if (text->schema_id == 0u) {
        if (text->stream_id != 0u || text->schema_revision != 0u || text->has_source_id
            || text->has_event_code)
            goto invalid;
    } else {
        if (text->stream_id == 0u || text->schema_revision == 0u)
            goto invalid;
        schema = sdaf_find_schema(doc, text->schema_id, text->schema_revision);
        if (schema != NULL && (!stream_kind(schema, text->stream_id, &kind) || kind != 2u))
            goto invalid;
    }
    text->message = sdaf_strndup_bytes(payload, payload_size);
    if (text->message == NULL)
        return SDAF_ERROR_MEMORY;
    text->message_size = payload_size;
    return SDAF_OK;
invalid:
    sdaf_set_error(error, error_size, "invalid TEXT references or optional fields");
    return SDAF_ERROR_FORMAT;
}

static int last_blob_index(
    const sdaf_document* doc, uint32_t schema_id, uint32_t stream_id, uint64_t* index)
{
    size_t i = doc->record_count;
    while (i != 0u) {
        const sdaf_record* r = &doc->records[--i];
        if (r->envelope.type == SDAF_RECORD_BLOB && r->value.blob.schema_id == schema_id
            && r->value.blob.stream_id == stream_id) {
            *index = r->value.blob.item_index;
            return 1;
        }
    }
    return 0;
}

static sdaf_status parse_blob(const uint8_t* h, const uint8_t* payload, size_t payload_size,
    const sdaf_limits* limits, const sdaf_document* doc, sdaf_record* record, char* error,
    size_t error_size)
{
    sdaf_blob_record* blob = &record->value.blob;
    const sdaf_schema* schema;
    size_t encoded_offset = 0u;
    sdaf_status status;
    uint8_t kind;
    uint64_t previous;
    if (record->envelope.header_size != 80u || (record->envelope.flags & 0xff00u) != 0u
        || sdaf_get_u32(h + 12u) != 0u || !sdaf_all_zero(h + 41u, 7u) || h[40] > 1u) {
        sdaf_set_error(error, error_size, "invalid BLOB header");
        return SDAF_ERROR_FORMAT;
    }
    blob->schema_id = sdaf_get_u32(h);
    blob->schema_revision = sdaf_get_u32(h + 4u);
    blob->stream_id = sdaf_get_u32(h + 8u);
    blob->item_index = sdaf_get_u64(h + 16u);
    blob->time_ticks = sdaf_get_i64(h + 24u);
    blob->decoded_bytes = sdaf_get_u64(h + 32u);
    blob->transform_count = h[40];
    if (blob->schema_id == 0u || blob->schema_revision == 0u || blob->stream_id == 0u)
        return SDAF_ERROR_FORMAT;
    if (blob->decoded_bytes > limits->max_decoded_size || blob->decoded_bytes > SIZE_MAX)
        return SDAF_ERROR_LIMIT;
    status = parse_transforms(payload, payload_size, blob->transform_count, &blob->transforms,
        &encoded_offset, error, error_size);
    if (status != SDAF_OK)
        return status;
    if (blob->transform_count == 1u && blob->transforms[0].id != 16u) {
        sdaf_set_error(error, error_size, "BLOB permits only transform 16");
        return SDAF_ERROR_FORMAT;
    }
    if (last_blob_index(doc, blob->schema_id, blob->stream_id, &previous)
        && blob->item_index <= previous) {
        sdaf_set_error(error, error_size, "BLOB item_index is not increasing");
        return SDAF_ERROR_FORMAT;
    }
    schema = sdaf_find_schema(doc, blob->schema_id, blob->schema_revision);
    if (schema != NULL && (!stream_kind(schema, blob->stream_id, &kind) || kind != 3u)) {
        sdaf_set_error(error, error_size, "BLOB record does not match stream kind");
        return SDAF_ERROR_FORMAT;
    }
    blob->stored_payload = sdaf_memdup(payload, payload_size);
    if (blob->stored_payload == NULL)
        return SDAF_ERROR_MEMORY;
    blob->stored_payload_size = payload_size;
    if (blob->transform_count == 0u) {
        blob->decoded_payload
            = sdaf_memdup(payload + encoded_offset, payload_size - encoded_offset);
        if (blob->decoded_payload == NULL)
            return SDAF_ERROR_MEMORY;
        blob->decoded_payload_size = payload_size - encoded_offset;
    } else {
        status = sdaf_zstd_decompress(payload + encoded_offset, payload_size - encoded_offset,
            (size_t)blob->decoded_bytes, &blob->decoded_payload, error, error_size);
        if (status == SDAF_ERROR_UNSUPPORTED) {
            error[0] = '\0';
            return SDAF_OK;
        }
        if (status != SDAF_OK)
            return status;
        blob->decoded_payload_size = (size_t)blob->decoded_bytes;
    }
    if (blob->decoded_payload_size != blob->decoded_bytes) {
        sdaf_set_error(error, error_size, "decoded BLOB size mismatch");
        return SDAF_ERROR_FORMAT;
    }
    return SDAF_OK;
}

static const sdaf_record* record_at_offset(const sdaf_document* doc, uint64_t offset)
{
    size_t i;
    for (i = 0u; i < doc->record_count; ++i)
        if (doc->records[i].envelope.file_offset == offset)
            return &doc->records[i];
    return NULL;
}

static sdaf_status parse_index(const uint8_t* h, const uint8_t* payload, size_t payload_size,
    const sdaf_document* doc, sdaf_record* record, char* error, size_t error_size)
{
    uint32_t count = sdaf_get_u32(h);
    size_t required, i;
    if (record->envelope.header_size != 48u || (record->envelope.flags & 0xff00u) != 0u
        || !sdaf_all_zero(h + 4u, 12u) || !sdaf_mul_size(count, 48u, &required)
        || required != payload_size) {
        sdaf_set_error(error, error_size, "invalid INDX header or payload size");
        return SDAF_ERROR_FORMAT;
    }
    record->value.index.entries
        = (sdaf_index_entry*)sdaf_calloc_array(count, sizeof(*record->value.index.entries));
    if (record->value.index.entries == NULL)
        return SDAF_ERROR_MEMORY;
    record->value.index.entry_count = count;
    for (i = 0u; i < count; ++i) {
        const uint8_t* p = payload + i * 48u;
        sdaf_index_entry* entry = &record->value.index.entries[i];
        const sdaf_record* target;
        if (sdaf_get_u32(p + 28u) != 0u)
            goto invalid;
        entry->record_offset = sdaf_get_u64(p);
        entry->sequence = sdaf_get_u32(p + 8u);
        entry->stream_id = sdaf_get_u32(p + 12u);
        entry->first_sample_index = sdaf_get_u64(p + 16u);
        entry->sample_count = sdaf_get_u32(p + 24u);
        entry->first_time_ticks = sdaf_get_i64(p + 32u);
        entry->last_time_ticks = sdaf_get_i64(p + 40u);
        target = record_at_offset(doc, entry->record_offset);
        if (target == NULL || target->envelope.sequence != entry->sequence)
            goto invalid;
        if (target->envelope.type == SDAF_RECORD_DATA) {
            if (target->value.data.stream_id != entry->stream_id
                || target->value.data.first_sample_index != entry->first_sample_index
                || target->value.data.sample_count != entry->sample_count)
                goto invalid;
        } else if (target->envelope.type == SDAF_RECORD_BLOB) {
            if (target->value.blob.stream_id != entry->stream_id
                || target->value.blob.item_index != entry->first_sample_index
                || entry->sample_count != 1u)
                goto invalid;
        } else
            goto invalid;
    }
    return SDAF_OK;
invalid:
    sdaf_set_error(error, error_size, "INDX entry is invalid or does not match its target");
    return SDAF_ERROR_FORMAT;
}

static sdaf_status parse_end(const uint8_t* h, size_t payload_size, const sdaf_document* doc,
    sdaf_record* record, char* error, size_t error_size)
{
    uint64_t total, data_total, last_index;
    size_t i, actual_data = 0u;
    if (record->envelope.header_size != 64u || (record->envelope.flags & 0xff00u) != 0u
        || payload_size != 0u || !sdaf_all_zero(h + 24u, 8u)) {
        sdaf_set_error(error, error_size, "invalid END record");
        return SDAF_ERROR_FORMAT;
    }
    total = sdaf_get_u64(h);
    data_total = sdaf_get_u64(h + 8u);
    last_index = sdaf_get_u64(h + 16u);
    for (i = 0u; i < doc->record_count; ++i)
        if (doc->records[i].envelope.type == SDAF_RECORD_DATA)
            ++actual_data;
    if (total != doc->record_count + 1u || data_total != actual_data
        || (last_index != 0u
            && (record_at_offset(doc, last_index) == NULL
                || record_at_offset(doc, last_index)->envelope.type != SDAF_RECORD_INDEX))) {
        sdaf_set_error(error, error_size, "END summary does not match scanned records");
        return SDAF_ERROR_FORMAT;
    }
    record->value.end.total_records = total;
    record->value.end.total_data_records = data_total;
    record->value.end.last_index_offset = last_index;
    return SDAF_OK;
}

static sdaf_status parse_known_record(const uint8_t* type_header, const uint8_t* payload,
    size_t payload_size, const sdaf_limits* limits, sdaf_document* doc, sdaf_record* record)
{
    sdaf_status status;
    switch (record->envelope.type) {
    case SDAF_RECORD_SCHEMA:
        return parse_schema(type_header, payload, payload_size, limits, doc, record, doc->error,
            sizeof(doc->error));
    case SDAF_RECORD_DATA:
        return parse_data(type_header, payload, payload_size, limits, doc, record, doc->error,
            sizeof(doc->error));
    case SDAF_RECORD_TEXT:
        return parse_text(type_header, payload, payload_size, limits, doc, record, doc->error,
            sizeof(doc->error));
    case SDAF_RECORD_BLOB:
        return parse_blob(type_header, payload, payload_size, limits, doc, record, doc->error,
            sizeof(doc->error));
    case SDAF_RECORD_INDEX:
        return parse_index(
            type_header, payload, payload_size, doc, record, doc->error, sizeof(doc->error));
    case SDAF_RECORD_END:
        return parse_end(type_header, payload_size, doc, record, doc->error, sizeof(doc->error));
    case SDAF_RECORD_NOTE:
        if (payload_size > limits->max_utf8_size) {
            sdaf_set_error(doc->error, sizeof(doc->error), "NOTE exceeds the UTF-8 limit");
            return SDAF_ERROR_LIMIT;
        }
        if (record->envelope.header_size != 32u || (record->envelope.flags & 0xff00u) != 0u
            || !sdaf_utf8_valid(payload, payload_size)) {
            sdaf_set_error(doc->error, sizeof(doc->error), "invalid NOTE record");
            return SDAF_ERROR_FORMAT;
        }
        record->value.note.message = sdaf_strndup_bytes(payload, payload_size);
        if (record->value.note.message == NULL)
            return SDAF_ERROR_MEMORY;
        record->value.note.size = payload_size;
        return SDAF_OK;
    default:
        record->value.unknown.type_header_size = record->envelope.header_size - 32u;
        record->value.unknown.type_header
            = sdaf_memdup(type_header, record->value.unknown.type_header_size);
        record->value.unknown.payload = sdaf_memdup(payload, payload_size);
        record->value.unknown.payload_size = payload_size;
        status = record->value.unknown.type_header != NULL && record->value.unknown.payload != NULL
            ? SDAF_OK
            : SDAF_ERROR_MEMORY;
        return status;
    }
}

sdaf_status sdaf_decode(
    const void* input, size_t size, const sdaf_limits* limits_input, sdaf_document* document)
{
    const uint8_t* data = (const uint8_t*)input;
    sdaf_limits defaults;
    const sdaf_limits* limits = limits_input;
    uint16_t file_header_size;
    uint64_t offset;
    if (document == NULL || (input == NULL && size != 0u))
        return SDAF_ERROR_ARGUMENT;
    memset(document, 0, sizeof(*document));
    if (limits == NULL) {
        sdaf_limits_default(&defaults);
        limits = &defaults;
    }
    if (size < 64u)
        return fail_at(document, SDAF_ERROR_FORMAT, 0u, "file is shorter than the SDAF header");
    if (memcmp(data, sdaf_magic, 8u) != 0 || data[8] != 1u)
        return fail_at(document, SDAF_ERROR_FORMAT, 0u, "invalid SDAF magic or major version");
    file_header_size = sdaf_get_u16(data + 10u);
    if (file_header_size < 64u || file_header_size > size)
        return fail_at(document, SDAF_ERROR_FORMAT, 0u, "invalid or incomplete file header size");
    if (file_header_size > limits->max_header_size)
        return fail_at(document, SDAF_ERROR_LIMIT, 0u, "file header exceeds the configured limit");
    {
        uint8_t* copy = sdaf_memdup(data, file_header_size);
        uint32_t expected;
        if (copy == NULL)
            return fail_at(document, SDAF_ERROR_MEMORY, 0u, "out of memory");
        expected = sdaf_get_u32(copy + 56u);
        sdaf_put_u32(copy + 56u, 0u);
        if (sdaf_crc32c(copy, file_header_size) != expected) {
            free(copy);
            return fail_at(document, SDAF_ERROR_FORMAT, 0u, "file header CRC-32C mismatch");
        }
        free(copy);
    }
    if (sdaf_get_u32(data + 12u) != UINT32_C(0x12345678) || sdaf_get_u32(data + 16u) != 0u
        || sdaf_get_u32(data + 20u) != 0u || sdaf_get_u32(data + 60u) != 0u
        || sdaf_get_u64(data + 48u) != file_header_size)
        return fail_at(
            document, SDAF_ERROR_FORMAT, 0u, "invalid file-header constants or reserved fields");
    document->header.major = data[8];
    document->header.minor = data[9];
    document->header.created_unix_ns = sdaf_get_i64(data + 24u);
    memcpy(document->header.file_uuid, data + 32u, 16u);
    document->header.first_record_offset = file_header_size;
    offset = file_header_size;
    while (offset < size) {
        const uint8_t* common;
        uint16_t header_size, flags, type;
        uint64_t payload_size, total;
        uint32_t sequence, expected_header_crc, expected_payload_crc;
        int trailer_mode;
        uint8_t* header_copy;
        sdaf_record record;
        sdaf_status status;
        size_t available = size - (size_t)offset;
        if (available < 32u)
            break;
        common = data + (size_t)offset;
        if (memcmp(common, "SDRC", 4u) != 0)
            return fail_at(document, SDAF_ERROR_FORMAT, offset, "invalid record marker");
        type = sdaf_get_u16(common + 4u);
        flags = sdaf_get_u16(common + 6u);
        header_size = sdaf_get_u16(common + 8u);
        sequence = sdaf_get_u32(common + 12u);
        payload_size = sdaf_get_u64(common + 16u);
        expected_header_crc = sdaf_get_u32(common + 24u);
        expected_payload_crc = sdaf_get_u32(common + 28u);
        trailer_mode = (flags & 1u) != 0u;
        if ((flags & 0x00feu) != 0u || header_size < 32u || common[10] != 1u || common[11] != 0u
            || (trailer_mode && expected_payload_crc != 0u))
            return fail_at(document, SDAF_ERROR_FORMAT, offset, "invalid record envelope");
        if (header_size > limits->max_header_size || payload_size > limits->max_payload_size
            || payload_size > SIZE_MAX)
            return fail_at(document, SDAF_ERROR_LIMIT, offset,
                "record exceeds a configured or platform limit");
        total = header_size;
        if (payload_size > UINT64_MAX - total)
            return fail_at(document, SDAF_ERROR_FORMAT, offset, "record size overflow");
        total += payload_size;
        if (trailer_mode) {
            if (total > UINT64_MAX - 16u)
                return fail_at(document, SDAF_ERROR_FORMAT, offset, "record size overflow");
            total += 16u;
        }
        if (total > available)
            break;
        header_copy = sdaf_memdup(common, header_size);
        if (header_copy == NULL)
            return fail_at(document, SDAF_ERROR_MEMORY, offset, "out of memory");
        sdaf_put_u32(header_copy + 24u, 0u);
        if (sdaf_crc32c(header_copy, header_size) != expected_header_crc) {
            free(header_copy);
            return fail_at(document, SDAF_ERROR_FORMAT, offset, "record header CRC-32C mismatch");
        }
        free(header_copy);
        if (!trailer_mode) {
            if (sdaf_crc32c(common + header_size, (size_t)payload_size) != expected_payload_crc)
                return fail_at(document, SDAF_ERROR_FORMAT, offset, "payload CRC-32C mismatch");
        } else {
            const uint8_t* trailer = common + header_size + (size_t)payload_size;
            if (memcmp(trailer, "SDCT", 4u) != 0 || sdaf_get_u16(trailer + 4u) != 16u
                || trailer[6] != 1u || trailer[7] != 0u || sdaf_get_u32(trailer + 8u) != sequence
                || sdaf_get_u32(trailer + 12u)
                    != sdaf_crc32c(common + header_size, (size_t)payload_size))
                return fail_at(document, SDAF_ERROR_FORMAT, offset, "invalid payload CRC trailer");
        }
        memset(&record, 0, sizeof(record));
        record.envelope.type = type;
        record.envelope.flags = flags;
        record.envelope.header_size = header_size;
        record.envelope.version = common[10];
        record.envelope.sequence = sequence;
        record.envelope.payload_size = payload_size;
        record.envelope.file_offset = offset;
        record.envelope.payload_crc_in_trailer = trailer_mode;
        status = parse_known_record(
            common + 32u, common + header_size, (size_t)payload_size, limits, document, &record);
        if (status != SDAF_OK) {
            free_record(&record);
            document->status = status;
            document->error_offset = offset;
            if (document->error[0] == '\0')
                sdaf_set_error(
                    document->error, sizeof(document->error), "%s", sdaf_status_string(status));
            return status;
        }
        status = append_record(document, &record, limits);
        if (status != SDAF_OK) {
            free_record(&record);
            return fail_at(document, status, offset, "could not append decoded record");
        }
        offset += total;
    }
    document->status = SDAF_OK;
    document->error_offset = 0u;
    document->error[0] = '\0';
    return SDAF_OK;
}

sdaf_status sdaf_decode_file(const char* path, const sdaf_limits* limits, sdaf_document* document)
{
    FILE* file;
    long length;
    uint8_t* data;
    size_t read;
    sdaf_status status;
    if (path == NULL || document == NULL)
        return SDAF_ERROR_ARGUMENT;
    memset(document, 0, sizeof(*document));
    file = fopen(path, "rb");
    if (file == NULL) {
        document->status = SDAF_ERROR_IO;
        sdaf_set_error(
            document->error, sizeof(document->error), "cannot open %s: %s", path, strerror(errno));
        return SDAF_ERROR_IO;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        document->status = SDAF_ERROR_IO;
        sdaf_set_error(document->error, sizeof(document->error), "cannot determine input size");
        return SDAF_ERROR_IO;
    }
    data = (uint8_t*)malloc(length == 0 ? 1u : (size_t)length);
    if (data == NULL) {
        fclose(file);
        document->status = SDAF_ERROR_MEMORY;
        return SDAF_ERROR_MEMORY;
    }
    read = fread(data, 1u, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(data);
        document->status = SDAF_ERROR_IO;
        sdaf_set_error(document->error, sizeof(document->error), "could not read complete input");
        return SDAF_ERROR_IO;
    }
    status = sdaf_decode(data, read, limits, document);
    free(data);
    return status;
}
