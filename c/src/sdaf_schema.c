#include "sdaf_internal.h"

#include <math.h>
#include <stdio.h>

const sdaf_tlv *sdaf_object_find_tlv(const sdaf_schema_object *object, uint16_t tag, size_t occurrence)
{
    size_t i;
    if (object == NULL) return NULL;
    for (i = 0u; i < object->tlv_count; ++i) {
        if (object->tlvs[i].tag == tag) {
            if (occurrence == 0u) return &object->tlvs[i];
            --occurrence;
        }
    }
    return NULL;
}

const sdaf_schema_object *sdaf_schema_find_object(const sdaf_schema *schema, uint8_t kind, uint32_t id)
{
    size_t i;
    if (schema == NULL) return NULL;
    for (i = 0u; i < schema->object_count; ++i)
        if (schema->objects[i].kind == kind && schema->objects[i].id == id) return &schema->objects[i];
    return NULL;
}

int sdaf_tlv_u8(const sdaf_tlv *tlv, uint8_t *value) { if (tlv == NULL || value == NULL || tlv->size != 1u) return 0; *value = tlv->value[0]; return 1; }
int sdaf_tlv_u16(const sdaf_tlv *tlv, uint16_t *value) { if (tlv == NULL || value == NULL || tlv->size != 2u) return 0; *value = sdaf_get_u16(tlv->value); return 1; }
int sdaf_tlv_u32(const sdaf_tlv *tlv, uint32_t *value) { if (tlv == NULL || value == NULL || tlv->size != 4u) return 0; *value = sdaf_get_u32(tlv->value); return 1; }
int sdaf_tlv_u64(const sdaf_tlv *tlv, uint64_t *value) { if (tlv == NULL || value == NULL || tlv->size != 8u) return 0; *value = sdaf_get_u64(tlv->value); return 1; }
int sdaf_tlv_i64(const sdaf_tlv *tlv, int64_t *value) { if (tlv == NULL || value == NULL || tlv->size != 8u) return 0; *value = sdaf_get_i64(tlv->value); return 1; }
int sdaf_tlv_f64(const sdaf_tlv *tlv, double *value) { if (tlv == NULL || value == NULL || tlv->size != 8u) return 0; *value = sdaf_get_f64(tlv->value); return 1; }

void sdaf_free_schema(sdaf_schema *schema)
{
    size_t i;
    if (schema == NULL) return;
    for (i = 0u; i < schema->object_count; ++i) {
        size_t j;
        for (j = 0u; j < schema->objects[i].tlv_count; ++j) free(schema->objects[i].tlvs[j].value);
        free(schema->objects[i].tlvs);
    }
    free(schema->objects);
    memset(schema, 0, sizeof(*schema));
}

void sdaf_free_transforms(sdaf_transform *transforms, size_t count)
{
    size_t i;
    for (i = 0u; i < count; ++i) free(transforms[i].parameters);
    free(transforms);
}

static int wire_size_valid(const sdaf_tlv *tlv)
{
    uint32_t expected = 0u;
    switch (tlv->wire_type) {
    case SDAF_WIRE_U8: case SDAF_WIRE_BOOL: expected = 1u; break;
    case SDAF_WIRE_U16: expected = 2u; break;
    case SDAF_WIRE_U32: expected = 4u; break;
    case SDAF_WIRE_U64: case SDAF_WIRE_I64: case SDAF_WIRE_F64: expected = 8u; break;
    case SDAF_WIRE_RATIONAL_U64: case SDAF_WIRE_RATIONAL_I64: expected = 16u; break;
    default: return 1;
    }
    return tlv->size == expected;
}

static int expected_wire(uint16_t tag, uint8_t *wire)
{
    if ((tag >= 1u && tag <= 7u) || tag == 10u || tag == 107u || tag == 204u || tag == 208u || tag == 304u) *wire = SDAF_WIRE_UTF8;
    else if (tag == 8u || tag == 9u || tag == 104u || tag == 105u || (tag >= 108u && tag <= 111u) || tag == 200u || tag == 207u || tag == 209u || tag == 210u) *wire = SDAF_WIRE_U32;
    else if (tag == 100u || tag == 103u || tag == 106u || tag == 201u || tag == 300u || tag == 303u) *wire = SDAF_WIRE_U8;
    else if (tag == 101u || tag == 102u || tag == 301u) *wire = SDAF_WIRE_RATIONAL_U64;
    else if (tag == 202u || tag == 203u || tag == 302u || tag == 500u) *wire = SDAF_WIRE_U16;
    else if (tag == 205u || tag == 206u) *wire = SDAF_WIRE_F64;
    else if (tag == 211u || tag == 212u) *wire = SDAF_WIRE_RATIONAL_I64;
    else if (tag == 400u || tag == 501u) *wire = SDAF_WIRE_BYTES;
    else return 0;
    return 1;
}

static int tag_repeatable(const sdaf_schema_object *object, uint16_t tag)
{
    return (object->kind == SDAF_OBJECT_VALUE_MAP && tag == 400u) || (object->kind == SDAF_OBJECT_BITFIELD && tag == 501u);
}

static sdaf_status validate_object_tlvs(const sdaf_schema_object *object, const sdaf_limits *limits, char *error, size_t error_size)
{
    size_t i;
    for (i = 0u; i < object->tlv_count; ++i) {
        const sdaf_tlv *tlv = &object->tlvs[i];
        uint8_t expected;
        size_t j;
        if (!wire_size_valid(tlv)) { sdaf_set_error(error, error_size, "tag %u has an invalid wire size", (unsigned)tlv->tag); return SDAF_ERROR_FORMAT; }
        if (expected_wire(tlv->tag, &expected) && tlv->wire_type != expected) { sdaf_set_error(error, error_size, "tag %u has the wrong wire type", (unsigned)tlv->tag); return SDAF_ERROR_FORMAT; }
        if (tlv->wire_type == SDAF_WIRE_UTF8 && (size_t)tlv->size > limits->max_utf8_size) { sdaf_set_error(error, error_size, "tag %u exceeds the UTF-8 limit", (unsigned)tlv->tag); return SDAF_ERROR_LIMIT; }
        if (tlv->wire_type == SDAF_WIRE_UTF8 && !sdaf_utf8_valid(tlv->value, tlv->size)) { sdaf_set_error(error, error_size, "tag %u contains invalid UTF-8", (unsigned)tlv->tag); return SDAF_ERROR_FORMAT; }
        if (tlv->wire_type == SDAF_WIRE_BOOL && tlv->value[0] > 1u) { sdaf_set_error(error, error_size, "tag %u contains an invalid boolean", (unsigned)tlv->tag); return SDAF_ERROR_FORMAT; }
        if ((tlv->wire_type == SDAF_WIRE_RATIONAL_U64 || tlv->wire_type == SDAF_WIRE_RATIONAL_I64) && sdaf_get_u64(tlv->value + 8u) == 0u) { sdaf_set_error(error, error_size, "tag %u has a zero denominator", (unsigned)tlv->tag); return SDAF_ERROR_FORMAT; }
        for (j = 0u; j < i; ++j) if (object->tlvs[j].tag == tlv->tag && !tag_repeatable(object, tlv->tag)) { sdaf_set_error(error, error_size, "tag %u is repeated", (unsigned)tlv->tag); return SDAF_ERROR_FORMAT; }
    }
    return SDAF_OK;
}

static sdaf_status validate_channel(const sdaf_schema *schema, const sdaf_schema_object *channel, char *error, size_t error_size)
{
    uint32_t stream_id = 0u;
    uint8_t type = 0u;
    uint16_t logical = 0u, storage = 0u;
    const sdaf_tlv *logical_tlv;
    if (!sdaf_tlv_u32(sdaf_object_find_tlv(channel, 200u, 0u), &stream_id) || sdaf_schema_find_object(schema, SDAF_OBJECT_STREAM, stream_id) == NULL || !sdaf_tlv_u8(sdaf_object_find_tlv(channel, 201u, 0u), &type) || !sdaf_tlv_u16(sdaf_object_find_tlv(channel, 203u, 0u), &storage)) {
        sdaf_set_error(error, error_size, "channel %u lacks required fields or references", channel->id); return SDAF_ERROR_FORMAT;
    }
    logical_tlv = sdaf_object_find_tlv(channel, 202u, 0u);
    if (logical_tlv != NULL) (void)sdaf_tlv_u16(logical_tlv, &logical);
    else if (type == SDAF_LOGICAL_FIXED_BYTES) logical = storage;
    else { sdaf_set_error(error, error_size, "channel %u lacks logical_bits", channel->id); return SDAF_ERROR_FORMAT; }
    if ((type == SDAF_LOGICAL_UNSIGNED || type == SDAF_LOGICAL_SIGNED) && !(logical >= 1u && logical <= 64u && storage >= logical && storage <= 64u)) goto invalid;
    if (type == SDAF_LOGICAL_FLOAT && !((logical == 32u && storage == 32u) || (logical == 64u && storage == 64u))) goto invalid;
    if (type == SDAF_LOGICAL_BOOLEAN && !(logical == 1u && storage == 1u)) goto invalid;
    if (type == SDAF_LOGICAL_FIXED_BYTES && !(storage >= 8u && (storage % 8u) == 0u && logical == storage)) goto invalid;
    if (type < SDAF_LOGICAL_UNSIGNED || type > SDAF_LOGICAL_FIXED_BYTES) goto invalid;
    if (sdaf_object_find_tlv(channel, 205u, 0u) != NULL && sdaf_object_find_tlv(channel, 211u, 0u) != NULL) goto invalid;
    if (sdaf_object_find_tlv(channel, 206u, 0u) != NULL && sdaf_object_find_tlv(channel, 212u, 0u) != NULL) goto invalid;
    {
        uint32_t map_id = 0u, bitfield_id = 0u, elements = 1u;
        const sdaf_tlv *elements_tlv = sdaf_object_find_tlv(channel, 207u, 0u);
        if (elements_tlv != NULL && (!sdaf_tlv_u32(elements_tlv, &elements) || elements == 0u)) goto invalid;
        (void)sdaf_tlv_u32(sdaf_object_find_tlv(channel, 209u, 0u), &map_id);
        (void)sdaf_tlv_u32(sdaf_object_find_tlv(channel, 210u, 0u), &bitfield_id);
        if ((map_id != 0u && bitfield_id != 0u) || (map_id != 0u && sdaf_schema_find_object(schema, SDAF_OBJECT_VALUE_MAP, map_id) == NULL) || (bitfield_id != 0u && sdaf_schema_find_object(schema, SDAF_OBJECT_BITFIELD, bitfield_id) == NULL)) goto invalid;
    }
    return SDAF_OK;
invalid:
    sdaf_set_error(error, error_size, "channel %u has an invalid type, width, or interpretation", channel->id); return SDAF_ERROR_FORMAT;
}

static size_t stream_channel_count(const sdaf_schema *schema, uint32_t stream_id)
{
    size_t i, count = 0u;
    for (i = 0u; i < schema->object_count; ++i) if (schema->objects[i].kind == SDAF_OBJECT_CHANNEL) { uint32_t reference = 0u; (void)sdaf_tlv_u32(sdaf_object_find_tlv(&schema->objects[i], 200u, 0u), &reference); if (reference == stream_id) ++count; }
    return count;
}

static sdaf_status validate_stream(const sdaf_schema *schema, const sdaf_schema_object *stream, char *error, size_t error_size)
{
    uint8_t kind = 1u, domain; uint32_t declared, reference; size_t actual = stream_channel_count(schema, stream->id); const sdaf_tlv *tlv;
    tlv = sdaf_object_find_tlv(stream, 106u, 0u); if (tlv != NULL && (!sdaf_tlv_u8(tlv, &kind) || kind < 1u || kind > 4u)) goto invalid;
    tlv = sdaf_object_find_tlv(stream, 104u, 0u); if (tlv != NULL && (!sdaf_tlv_u32(tlv, &declared) || declared != actual)) goto invalid;
    if ((kind == 2u || kind == 3u) && actual != 0u) goto invalid;
    tlv = sdaf_object_find_tlv(stream, 100u, 0u); if (tlv != NULL && (!sdaf_tlv_u8(tlv, &domain) || domain > 4u)) goto invalid;
    if (kind == 3u) { tlv = sdaf_object_find_tlv(stream, 107u, 0u); if (tlv == NULL || tlv->size == 0u) goto invalid; }
    tlv = sdaf_object_find_tlv(stream, 105u, 0u);
    if (tlv != NULL) {
        const sdaf_schema_object *clock;
        if (!sdaf_tlv_u32(tlv, &reference)) goto invalid;
        if (reference != 0u) {
            const sdaf_tlv *stream_domain = sdaf_object_find_tlv(stream, 100u, 0u), *clock_domain;
            const sdaf_tlv *stream_unit = sdaf_object_find_tlv(stream, 101u, 0u), *clock_unit;
            clock = sdaf_schema_find_object(schema, SDAF_OBJECT_CLOCK, reference); if (clock == NULL) goto invalid;
            clock_domain = sdaf_object_find_tlv(clock, 300u, 0u); clock_unit = sdaf_object_find_tlv(clock, 301u, 0u);
            if (stream_domain != NULL && (clock_domain == NULL || stream_domain->size != clock_domain->size || memcmp(stream_domain->value, clock_domain->value, stream_domain->size) != 0)) goto invalid;
            if (stream_unit != NULL && (clock_unit == NULL || stream_unit->size != clock_unit->size || memcmp(stream_unit->value, clock_unit->value, stream_unit->size) != 0)) goto invalid;
        }
    }
    if (kind == 4u) {
        uint32_t source = 0u, target = 0u; int has_source = 0, has_target = 0; size_t i;
        if (!sdaf_tlv_u32(sdaf_object_find_tlv(stream, 108u, 0u), &source) || !sdaf_tlv_u32(sdaf_object_find_tlv(stream, 109u, 0u), &target) || source == 0u || target == 0u || sdaf_schema_find_object(schema, SDAF_OBJECT_CLOCK, source) == NULL || sdaf_schema_find_object(schema, SDAF_OBJECT_CLOCK, target) == NULL) goto invalid;
        for (i = 0u; i < schema->object_count; ++i) if (schema->objects[i].kind == SDAF_OBJECT_CHANNEL) {
            const sdaf_schema_object *channel = &schema->objects[i]; uint32_t channel_stream = 0u, elements = 1u; uint8_t type = 0u; const sdaf_tlv *semantic;
            (void)sdaf_tlv_u32(sdaf_object_find_tlv(channel, 200u, 0u), &channel_stream); if (channel_stream != stream->id) continue;
            (void)sdaf_tlv_u8(sdaf_object_find_tlv(channel, 201u, 0u), &type); (void)sdaf_tlv_u32(sdaf_object_find_tlv(channel, 207u, 0u), &elements); if (type != SDAF_LOGICAL_SIGNED || elements != 1u) goto invalid;
            semantic = sdaf_object_find_tlv(channel, 208u, 0u); if (semantic != NULL && semantic->size == 18u && memcmp(semantic->value, "clock.source_ticks", 18u) == 0) has_source = 1; if (semantic != NULL && semantic->size == 18u && memcmp(semantic->value, "clock.target_ticks", 18u) == 0) has_target = 1;
        }
        if (!has_source || !has_target) goto invalid;
    }
    {
        const uint16_t map_tags[2] = {110u, 111u}; size_t i;
        for (i = 0u; i < 2u; ++i) { tlv = sdaf_object_find_tlv(stream, map_tags[i], 0u); if (tlv != NULL && (!sdaf_tlv_u32(tlv, &reference) || reference == 0u || sdaf_schema_find_object(schema, SDAF_OBJECT_VALUE_MAP, reference) == NULL)) goto invalid; }
    }
    return SDAF_OK;
invalid: sdaf_set_error(error, error_size, "stream %u has invalid kind, count, or references", stream->id); return SDAF_ERROR_FORMAT;
}

static sdaf_status validate_value_map(const sdaf_schema_object *map, char *error, size_t error_size)
{
    size_t i;
    for (i = 0u; i < map->tlv_count; ++i) if (map->tlvs[i].tag == 400u) {
        const sdaf_tlv *entry = &map->tlvs[i];
        uint16_t name_size, description_size;
        size_t total;
        size_t j;
        if (entry->size < 12u) goto invalid;
        name_size = sdaf_get_u16(entry->value + 8u); description_size = sdaf_get_u16(entry->value + 10u);
        total = 12u + (size_t)name_size + (size_t)description_size;
        if (total != entry->size || name_size == 0u || !sdaf_utf8_valid(entry->value + 12u, name_size) || !sdaf_utf8_valid(entry->value + 12u + name_size, description_size)) goto invalid;
        for (j = 0u; j < i; ++j) if (map->tlvs[j].tag == 400u) {
            uint16_t earlier_name = sdaf_get_u16(map->tlvs[j].value + 8u);
            if (sdaf_get_u64(map->tlvs[j].value) == sdaf_get_u64(entry->value) || (earlier_name == name_size && memcmp(map->tlvs[j].value + 12u, entry->value + 12u, name_size) == 0)) goto invalid;
        }
    }
    return SDAF_OK;
invalid:
    sdaf_set_error(error, error_size, "value map %u contains an invalid or duplicate entry", map->id); return SDAF_ERROR_FORMAT;
}

static sdaf_status validate_bitfield(const sdaf_schema *schema, const sdaf_schema_object *field, char *error, size_t error_size)
{
    uint16_t storage = 0u;
    uint8_t *used;
    size_t i;
    if (!sdaf_tlv_u16(sdaf_object_find_tlv(field, 500u, 0u), &storage) || storage == 0u) { sdaf_set_error(error, error_size, "bitfield %u lacks storage_bits", field->id); return SDAF_ERROR_FORMAT; }
    used = (uint8_t *)calloc(storage, 1u); if (used == NULL) return SDAF_ERROR_MEMORY;
    for (i = 0u; i < field->tlv_count; ++i) if (field->tlvs[i].tag == 501u) {
        const sdaf_tlv *member = &field->tlvs[i];
        uint16_t lsb, width, flags, name_size, description_size;
        uint32_t map_id;
        size_t bit;
        if (member->size < 14u) goto invalid;
        lsb = sdaf_get_u16(member->value); width = sdaf_get_u16(member->value + 2u); map_id = sdaf_get_u32(member->value + 4u); flags = sdaf_get_u16(member->value + 8u); name_size = sdaf_get_u16(member->value + 10u); description_size = sdaf_get_u16(member->value + 12u);
        if (width == 0u || (uint32_t)lsb + width > storage || (flags & (uint16_t)~1u) != 0u || (size_t)member->size != 14u + name_size + description_size || !sdaf_utf8_valid(member->value + 14u, name_size) || !sdaf_utf8_valid(member->value + 14u + name_size, description_size) || (map_id != 0u && sdaf_schema_find_object(schema, SDAF_OBJECT_VALUE_MAP, map_id) == NULL)) goto invalid;
        for (bit = lsb; bit < (size_t)lsb + width; ++bit) { if (used[bit] != 0u) goto invalid; used[bit] = 1u; }
    }
    for (i = 0u; i < schema->object_count; ++i) if (schema->objects[i].kind == SDAF_OBJECT_CHANNEL) { uint32_t reference = 0u; uint16_t channel_storage = 0u; (void)sdaf_tlv_u32(sdaf_object_find_tlv(&schema->objects[i], 210u, 0u), &reference); if (reference == field->id && (!sdaf_tlv_u16(sdaf_object_find_tlv(&schema->objects[i], 203u, 0u), &channel_storage) || channel_storage != storage)) goto invalid; }
    free(used); return SDAF_OK;
invalid:
    free(used); sdaf_set_error(error, error_size, "bitfield %u contains an invalid member", field->id); return SDAF_ERROR_FORMAT;
}

sdaf_status sdaf_validate_schema(const sdaf_schema *schema, const sdaf_limits *limits, char *error, size_t error_size)
{
    size_t i;
    if (schema->id == 0u || schema->revision == 0u) { sdaf_set_error(error, error_size, "invalid schema ID or revision"); return SDAF_ERROR_FORMAT; }
    if (schema->object_count > limits->max_schema_objects) { sdaf_set_error(error, error_size, "schema exceeds the object limit"); return SDAF_ERROR_LIMIT; }
    for (i = 0u; i < schema->object_count; ++i) {
        const sdaf_schema_object *object = &schema->objects[i];
        size_t j;
        sdaf_status status;
        if (object->kind < SDAF_OBJECT_FILE_METADATA || object->kind > SDAF_OBJECT_BITFIELD || object->id == 0u) { sdaf_set_error(error, error_size, "invalid schema object envelope"); return SDAF_ERROR_FORMAT; }
        for (j = 0u; j < i; ++j) if (schema->objects[j].kind == object->kind && schema->objects[j].id == object->id) { sdaf_set_error(error, error_size, "duplicate schema object ID"); return SDAF_ERROR_FORMAT; }
        status = validate_object_tlvs(object, limits, error, error_size); if (status != SDAF_OK) return status;
    }
    for (i = 0u; i < schema->object_count; ++i) {
        const sdaf_schema_object *object = &schema->objects[i];
        sdaf_status status;
        if (object->kind == SDAF_OBJECT_STREAM) { status = validate_stream(schema, object, error, error_size); if (status != SDAF_OK) return status; }
        else if (object->kind == SDAF_OBJECT_CHANNEL) { status = validate_channel(schema, object, error, error_size); if (status != SDAF_OK) return status; }
        else if (object->kind == SDAF_OBJECT_VALUE_MAP) { status = validate_value_map(object, error, error_size); if (status != SDAF_OK) return status; }
        else if (object->kind == SDAF_OBJECT_BITFIELD) { status = validate_bitfield(schema, object, error, error_size); if (status != SDAF_OK) return status; }
        else if (object->kind == SDAF_OBJECT_CLOCK) {
            uint8_t domain = 0u;
            if (!sdaf_tlv_u8(sdaf_object_find_tlv(object, 300u, 0u), &domain) || domain > 4u || sdaf_object_find_tlv(object, 301u, 0u) == NULL) { sdaf_set_error(error, error_size, "clock %u lacks required fields", object->id); return SDAF_ERROR_FORMAT; }
        }
    }
    return SDAF_OK;
}

static int channel_compare(const void *left, const void *right)
{
    const sdaf_channel_info *a = (const sdaf_channel_info *)left;
    const sdaf_channel_info *b = (const sdaf_channel_info *)right;
    return a->id < b->id ? -1 : (a->id > b->id ? 1 : 0);
}

static double rational_i64_value(const sdaf_tlv *tlv)
{
    return (double)sdaf_get_i64(tlv->value) / (double)sdaf_get_u64(tlv->value + 8u);
}

sdaf_status sdaf_channels_for_stream(const sdaf_schema *schema, uint32_t stream_id, const sdaf_limits *limits, sdaf_channel_info **channels, size_t *count, size_t *lanes, char *error, size_t error_size)
{
    size_t i, found = 0u, lane_count = 0u;
    sdaf_channel_info *result;
    for (i = 0u; i < schema->object_count; ++i) if (schema->objects[i].kind == SDAF_OBJECT_CHANNEL) { uint32_t id = 0u; (void)sdaf_tlv_u32(sdaf_object_find_tlv(&schema->objects[i], 200u, 0u), &id); if (id == stream_id) ++found; }
    if (found > limits->max_channels_per_stream) { sdaf_set_error(error, error_size, "stream exceeds the channel limit"); return SDAF_ERROR_LIMIT; }
    result = (sdaf_channel_info *)sdaf_calloc_array(found, sizeof(*result)); if (result == NULL) return SDAF_ERROR_MEMORY;
    found = 0u;
    for (i = 0u; i < schema->object_count; ++i) if (schema->objects[i].kind == SDAF_OBJECT_CHANNEL) {
        const sdaf_schema_object *object = &schema->objects[i];
        uint32_t id = 0u;
        if (sdaf_tlv_u32(sdaf_object_find_tlv(object, 200u, 0u), &id) && id == stream_id) {
            sdaf_channel_info *c = &result[found++];
            const sdaf_tlv *tlv;
            c->object = object; c->id = object->id; c->name = ""; c->unit = NULL; c->elements = 1u; c->scale = 1.0; c->offset = 0.0;
            tlv = sdaf_object_find_tlv(object, 1u, 0u); if (tlv != NULL) c->name = (const char *)tlv->value;
            tlv = sdaf_object_find_tlv(object, 204u, 0u); if (tlv != NULL) c->unit = (const char *)tlv->value;
            (void)sdaf_tlv_u8(sdaf_object_find_tlv(object, 201u, 0u), &c->logical_type);
            (void)sdaf_tlv_u16(sdaf_object_find_tlv(object, 203u, 0u), &c->storage_bits);
            if (!sdaf_tlv_u16(sdaf_object_find_tlv(object, 202u, 0u), &c->logical_bits)) c->logical_bits = c->storage_bits;
            (void)sdaf_tlv_u32(sdaf_object_find_tlv(object, 207u, 0u), &c->elements); if (c->elements == 0u) c->elements = 1u;
            tlv = sdaf_object_find_tlv(object, 211u, 0u); if (tlv != NULL) c->scale = rational_i64_value(tlv); else (void)sdaf_tlv_f64(sdaf_object_find_tlv(object, 205u, 0u), &c->scale);
            tlv = sdaf_object_find_tlv(object, 212u, 0u); if (tlv != NULL) c->offset = rational_i64_value(tlv); else (void)sdaf_tlv_f64(sdaf_object_find_tlv(object, 206u, 0u), &c->offset);
            if (lane_count > SIZE_MAX - c->elements) { free(result); sdaf_set_error(error, error_size, "lane count overflow"); return SDAF_ERROR_LIMIT; }
            lane_count += c->elements;
        }
    }
    qsort(result, found, sizeof(*result), channel_compare);
    *channels = result; *count = found; *lanes = lane_count; return SDAF_OK;
}

sdaf_status sdaf_expected_payload_size(const sdaf_channel_info *channels, size_t channel_count, uint32_t sample_count, uint32_t timestamp_bytes, uint8_t packing, size_t *result)
{
    size_t i, per_sample = 0u, total;
    for (i = 0u; i < channel_count; ++i) {
        size_t field;
        if (!sdaf_mul_size(packing == SDAF_PACKING_LSB0_DENSE ? channels[i].storage_bits : (size_t)((channels[i].storage_bits + 7u) / 8u), channels[i].elements, &field) || !sdaf_add_size(per_sample, field, &per_sample)) return SDAF_ERROR_LIMIT;
    }
    if (packing == SDAF_PACKING_LSB0_DENSE) {
        if (!sdaf_mul_size(per_sample, sample_count, &total) || !sdaf_add_size(total, 7u, &total)) return SDAF_ERROR_LIMIT;
        total /= 8u;
    } else if (packing == SDAF_PACKING_BYTE_ALIGNED) {
        if (!sdaf_mul_size(per_sample, sample_count, &total)) return SDAF_ERROR_LIMIT;
    } else return SDAF_ERROR_FORMAT;
    if (!sdaf_add_size(total, timestamp_bytes, result)) return SDAF_ERROR_LIMIT;
    return SDAF_OK;
}

const sdaf_schema *sdaf_find_schema(const sdaf_document *doc, uint32_t id, uint32_t revision)
{
    size_t i = doc->record_count;
    while (i != 0u) { const sdaf_record *record = &doc->records[--i]; if (record->envelope.type == SDAF_RECORD_SCHEMA && record->value.schema.id == id && record->value.schema.revision == revision) return &record->value.schema; }
    return NULL;
}
