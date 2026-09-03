#include "exports.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

static const char *record_name(uint16_t type)
{
    switch (type) { case SDAF_RECORD_SCHEMA: return "SCMA"; case SDAF_RECORD_DATA: return "DATA"; case SDAF_RECORD_TEXT: return "TEXT"; case SDAF_RECORD_INDEX: return "INDX"; case SDAF_RECORD_END: return "END!"; case SDAF_RECORD_BLOB: return "BLOB"; case SDAF_RECORD_NOTE: return "NOTE"; default: return NULL; }
}

static int json_string(FILE *output, const char *text, size_t size)
{
    size_t i;
    if (fputc('"', output) == EOF) return -1;
    for (i = 0u; i < size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') { if (fputc('\\', output) == EOF || fputc(c, output) == EOF) return -1; }
        else if (c == '\b') { if (fputs("\\b", output) == EOF) return -1; }
        else if (c == '\f') { if (fputs("\\f", output) == EOF) return -1; }
        else if (c == '\n') { if (fputs("\\n", output) == EOF) return -1; }
        else if (c == '\r') { if (fputs("\\r", output) == EOF) return -1; }
        else if (c == '\t') { if (fputs("\\t", output) == EOF) return -1; }
        else if (c < 0x20u) { if (fprintf(output, "\\u%04x", c) < 0) return -1; }
        else if (fputc(c, output) == EOF) return -1;
    }
    return fputc('"', output) == EOF ? -1 : 0;
}

static int write_hex(FILE *output, const uint8_t *bytes, size_t count)
{
    static const char hex[] = "0123456789abcdef"; size_t i;
    for (i = 0u; i < count; ++i) if (fputc(hex[bytes[i] >> 4u], output) == EOF || fputc(hex[bytes[i] & 15u], output) == EOF) return -1;
    return 0;
}

static int json_double(FILE *output, double value)
{
    return isfinite(value) ? (fprintf(output, "%.17g", value) < 0 ? -1 : 0) : (fputs("null", output) == EOF ? -1 : 0);
}

static int json_sample(FILE *output, const sdaf_sample *sample)
{
    size_t i;
    if (fprintf(output, "{\"index\":%" PRIu64, sample->index) < 0) return -1;
    if (sample->has_time && fprintf(output, ",\"time_ticks\":%" PRId64, sample->time_ticks) < 0) return -1;
    if (fputs(",\"values\":[", output) == EOF) return -1;
    for (i = 0u; i < sample->value_count; ++i) {
        const sdaf_sample_value *value = &sample->values[i];
        if (i != 0u && fputc(',', output) == EOF) return -1;
        if (fprintf(output, "{\"channel_id\":%" PRIu32 ",\"name\":", value->channel_id) < 0 || json_string(output, value->channel_name, strlen(value->channel_name)) != 0 || fprintf(output, ",\"element\":%" PRIu32 ",\"raw\":", value->element_index) < 0) return -1;
        if (value->bytes != NULL) { if (fputc('"', output) == EOF || write_hex(output, value->bytes, value->byte_count) != 0 || fputc('"', output) == EOF) return -1; }
        else if (value->logical_type == SDAF_LOGICAL_SIGNED) { if (fprintf(output, "%" PRId64, value->raw_signed) < 0) return -1; }
        else if (value->logical_type == SDAF_LOGICAL_FLOAT) { if (json_double(output, value->numeric_value) != 0) return -1; }
        else if (fprintf(output, "%" PRIu64, value->raw_unsigned) < 0) return -1;
        if (value->bytes == NULL && (fputs(",\"physical\":", output) == EOF || json_double(output, value->physical_value) != 0)) return -1;
        if (value->unit != NULL && (fputs(",\"unit\":", output) == EOF || json_string(output, value->unit, strlen(value->unit)) != 0)) return -1;
        if (fputc('}', output) == EOF) return -1;
    }
    return fputs("]}", output) == EOF ? -1 : 0;
}

int sdaf_cli_write_json(FILE *output, const sdaf_document *document)
{
    size_t i;
    if (output == NULL || document == NULL) return -1;
    if (fprintf(output, "{\"header\":{\"major\":%u,\"minor\":%u,\"created_unix_ns\":%" PRId64 ",\"file_uuid\":\"", document->header.major, document->header.minor, document->header.created_unix_ns) < 0 || write_hex(output, document->header.file_uuid, 16u) != 0 || fputs("\"},\"records\":[", output) == EOF) return -1;
    for (i = 0u; i < document->record_count; ++i) {
        const sdaf_record *record = &document->records[i]; const char *name = record_name(record->envelope.type); char unknown[7];
        if (i != 0u && fputc(',', output) == EOF) return -1;
        if (name == NULL) { (void)snprintf(unknown, sizeof(unknown), "0x%04x", record->envelope.type); name = unknown; }
        if (fputs("{\"type\":", output) == EOF || json_string(output, name, strlen(name)) != 0 || fprintf(output, ",\"type_id\":%u,\"sequence\":%" PRIu32, record->envelope.type, record->envelope.sequence) < 0) return -1;
        switch (record->envelope.type) {
        case SDAF_RECORD_SCHEMA:
            if (fprintf(output, ",\"schema_id\":%" PRIu32 ",\"schema_revision\":%" PRIu32 ",\"object_count\":%lu", record->value.schema.id, record->value.schema.revision, (unsigned long)record->value.schema.object_count) < 0) return -1;
            break;
        case SDAF_RECORD_DATA: {
            size_t s; const sdaf_data_record *data = &record->value.data;
            if (fprintf(output, ",\"schema_id\":%" PRIu32 ",\"stream_id\":%" PRIu32 ",\"sample_count\":%" PRIu32 ",\"samples\":[", data->schema_id, data->stream_id, data->sample_count) < 0) return -1;
            if (data->samples != NULL) for (s = 0u; s < data->sample_count; ++s) { if (s != 0u && fputc(',', output) == EOF) return -1; if (json_sample(output, &data->samples[s]) != 0) return -1; }
            if (fputc(']', output) == EOF) return -1;
            break;
        }
        case SDAF_RECORD_TEXT:
            if (fprintf(output, ",\"stream_id\":%" PRIu32 ",\"time_ticks\":%" PRId64 ",\"severity\":%u,\"message\":", record->value.text.stream_id, record->value.text.time_ticks, record->value.text.severity) < 0 || json_string(output, record->value.text.message, record->value.text.message_size) != 0) return -1;
            break;
        case SDAF_RECORD_BLOB:
            if (fprintf(output, ",\"stream_id\":%" PRIu32 ",\"item_index\":%" PRIu64 ",\"time_ticks\":%" PRId64 ",\"decoded_bytes\":%" PRIu64 ",\"data_hex\":\"", record->value.blob.stream_id, record->value.blob.item_index, record->value.blob.time_ticks, record->value.blob.decoded_bytes) < 0 || (record->value.blob.decoded_payload != NULL && write_hex(output, record->value.blob.decoded_payload, record->value.blob.decoded_payload_size) != 0) || fputc('"', output) == EOF) return -1;
            break;
        case SDAF_RECORD_NOTE: if (fputs(",\"message\":", output) == EOF || json_string(output, record->value.note.message, record->value.note.size) != 0) return -1; break;
        case SDAF_RECORD_INDEX: if (fprintf(output, ",\"entry_count\":%lu", (unsigned long)record->value.index.entry_count) < 0) return -1; break;
        case SDAF_RECORD_END: if (fprintf(output, ",\"total_record_count\":%" PRIu64 ",\"total_data_record_count\":%" PRIu64, record->value.end.total_records, record->value.end.total_data_records) < 0) return -1; break;
        default: break;
        }
        if (fputc('}', output) == EOF) return -1;
    }
    return fputs("]}\n", output) == EOF ? -1 : 0;
}

static int csv_cell(FILE *output, const char *text)
{
    const char *p; int quote = 0;
    for (p = text; *p != '\0'; ++p) if (*p == ',' || *p == '"' || *p == '\r' || *p == '\n') quote = 1;
    if (!quote) return fputs(text, output) == EOF ? -1 : 0;
    if (fputc('"', output) == EOF) return -1;
    for (p = text; *p != '\0'; ++p) { if (*p == '"' && fputc('"', output) == EOF) return -1; if (fputc(*p, output) == EOF) return -1; }
    return fputc('"', output) == EOF ? -1 : 0;
}

int sdaf_cli_write_csv(FILE *output, const sdaf_document *document)
{
    size_t i;
    if (fputs("sequence,stream_id,sample_index,time_ticks,channel_id,channel_name,element,raw,physical,unit\n", output) == EOF) return -1;
    for (i = 0u; i < document->record_count; ++i) if (document->records[i].envelope.type == SDAF_RECORD_DATA && document->records[i].value.data.samples != NULL) {
        const sdaf_record *record = &document->records[i]; size_t s;
        for (s = 0u; s < record->value.data.sample_count; ++s) { const sdaf_sample *sample = &record->value.data.samples[s]; size_t v; for (v = 0u; v < sample->value_count; ++v) {
            const sdaf_sample_value *value = &sample->values[v];
            if (fprintf(output, "%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",", record->envelope.sequence, record->value.data.stream_id, sample->index) < 0) return -1;
            if (sample->has_time && fprintf(output, "%" PRId64, sample->time_ticks) < 0) return -1;
            if (fprintf(output, ",%" PRIu32 ",", value->channel_id) < 0 || csv_cell(output, value->channel_name) != 0 || fprintf(output, ",%" PRIu32 ",", value->element_index) < 0) return -1;
            if (value->bytes != NULL) { if (write_hex(output, value->bytes, value->byte_count) != 0) return -1; }
            else if (value->logical_type == SDAF_LOGICAL_SIGNED) { if (fprintf(output, "%" PRId64, value->raw_signed) < 0) return -1; }
            else if (value->logical_type == SDAF_LOGICAL_FLOAT) { if (fprintf(output, "%.17g", value->numeric_value) < 0) return -1; }
            else if (fprintf(output, "%" PRIu64, value->raw_unsigned) < 0) return -1;
            if (fputc(',', output) == EOF || (value->bytes == NULL && fprintf(output, "%.17g", value->physical_value) < 0) || fputc(',', output) == EOF || (value->unit != NULL && csv_cell(output, value->unit) != 0) || fputc('\n', output) == EOF) return -1;
        } }
    }
    return 0;
}

static int cbor_head(FILE *output, unsigned major, uint64_t value)
{
    uint8_t b[9]; size_t count = 1u, i;
    if (value < 24u) b[0] = (uint8_t)((major << 5u) | (unsigned)value);
    else if (value <= UINT8_MAX) { b[0] = (uint8_t)((major << 5u) | 24u); b[1] = (uint8_t)value; count = 2u; }
    else if (value <= UINT16_MAX) { b[0] = (uint8_t)((major << 5u) | 25u); b[1] = (uint8_t)(value >> 8u); b[2] = (uint8_t)value; count = 3u; }
    else if (value <= UINT32_MAX) { b[0] = (uint8_t)((major << 5u) | 26u); for (i = 0u; i < 4u; ++i) b[1u + i] = (uint8_t)(value >> ((3u - i) * 8u)); count = 5u; }
    else { b[0] = (uint8_t)((major << 5u) | 27u); for (i = 0u; i < 8u; ++i) b[1u + i] = (uint8_t)(value >> ((7u - i) * 8u)); count = 9u; }
    return fwrite(b, 1u, count, output) == count ? 0 : -1;
}

static int cbor_text_n(FILE *output, const char *text, size_t size) { return cbor_head(output, 3u, size) != 0 || fwrite(text, 1u, size, output) != size ? -1 : 0; }
static int cbor_text(FILE *output, const char *text) { return cbor_text_n(output, text, strlen(text)); }
static int cbor_uint(FILE *output, uint64_t value) { return cbor_head(output, 0u, value); }
static int cbor_int(FILE *output, int64_t value) { return value >= 0 ? cbor_uint(output, (uint64_t)value) : cbor_head(output, 1u, (uint64_t)(-(value + 1))); }
static int cbor_bytes(FILE *output, const uint8_t *bytes, size_t size) { return cbor_head(output, 2u, size) != 0 || (size != 0u && fwrite(bytes, 1u, size, output) != size) ? -1 : 0; }

static int cbor_double(FILE *output, double value)
{
    uint64_t bits; uint8_t bytes[9]; size_t i;
    memcpy(&bits, &value, sizeof(bits)); bytes[0] = 0xfbu;
    for (i = 0u; i < 8u; ++i) bytes[i + 1u] = (uint8_t)(bits >> ((7u - i) * 8u));
    return fwrite(bytes, 1u, sizeof(bytes), output) == sizeof(bytes) ? 0 : -1;
}

static const char *object_kind_name(uint8_t kind)
{
    switch (kind) { case SDAF_OBJECT_FILE_METADATA: return "FileMetadata"; case SDAF_OBJECT_STREAM: return "Stream"; case SDAF_OBJECT_CHANNEL: return "Channel"; case SDAF_OBJECT_CLOCK: return "Clock"; case SDAF_OBJECT_VALUE_MAP: return "ValueMap"; case SDAF_OBJECT_BITFIELD: return "Bitfield"; default: return "Unknown"; }
}

static int cbor_sample(FILE *output, const sdaf_sample *sample)
{
    size_t i;
    if (cbor_head(output, 5u, sample->has_time ? 3u : 2u) != 0 || cbor_text(output, "index") != 0 || cbor_uint(output, sample->index) != 0) return -1;
    if (sample->has_time && (cbor_text(output, "time_ticks") != 0 || cbor_int(output, sample->time_ticks) != 0)) return -1;
    if (cbor_text(output, "values") != 0 || cbor_head(output, 4u, sample->value_count) != 0) return -1;
    for (i = 0u; i < sample->value_count; ++i) {
        const sdaf_sample_value *value = &sample->values[i];
        if (cbor_head(output, 5u, value->bytes == NULL ? 4u : 3u) != 0 || cbor_text(output, "channel_id") != 0 || cbor_uint(output, value->channel_id) != 0 || cbor_text(output, "name") != 0 || cbor_text(output, value->channel_name) != 0 || cbor_text(output, "raw") != 0) return -1;
        if (value->bytes != NULL) { if (cbor_bytes(output, value->bytes, value->byte_count) != 0) return -1; }
        else if (value->logical_type == SDAF_LOGICAL_SIGNED) { if (cbor_int(output, value->raw_signed) != 0) return -1; }
        else if (value->logical_type == SDAF_LOGICAL_FLOAT) { if (cbor_double(output, value->numeric_value) != 0) return -1; }
        else if (cbor_uint(output, value->raw_unsigned) != 0) return -1;
        if (value->bytes == NULL && (cbor_text(output, "physical") != 0 || cbor_double(output, value->physical_value) != 0)) return -1;
    }
    return 0;
}
int sdaf_cli_write_cbor(FILE *output, const sdaf_document *document)
{
    size_t i;
    if (cbor_head(output, 5u, 2u) != 0 || cbor_text(output, "header") != 0 || cbor_head(output, 5u, 2u) != 0 || cbor_text(output, "major") != 0 || cbor_uint(output, document->header.major) != 0 || cbor_text(output, "minor") != 0 || cbor_uint(output, document->header.minor) != 0 || cbor_text(output, "records") != 0 || cbor_head(output, 4u, document->record_count) != 0) return -1;
    for (i = 0u; i < document->record_count; ++i) {
        const sdaf_record *record = &document->records[i]; const char *name = record_name(record->envelope.type); char unknown[7];
        uint64_t extras;
        if (name == NULL) { (void)snprintf(unknown, sizeof(unknown), "0x%04x", record->envelope.type); name = unknown; }
        switch (record->envelope.type) { case SDAF_RECORD_SCHEMA: extras = 3u; break; case SDAF_RECORD_DATA: extras = 4u; break; case SDAF_RECORD_TEXT: extras = 5u; break; case SDAF_RECORD_BLOB: extras = 5u; break; case SDAF_RECORD_NOTE: extras = 1u; break; case SDAF_RECORD_INDEX: extras = 1u; break; case SDAF_RECORD_END: extras = 3u; break; default: extras = 1u; break; }
        if (cbor_head(output, 5u, 3u + extras) != 0 || cbor_text(output, "type") != 0 || cbor_text(output, name) != 0 || cbor_text(output, "type_id") != 0 || cbor_uint(output, record->envelope.type) != 0 || cbor_text(output, "sequence") != 0 || cbor_uint(output, record->envelope.sequence) != 0) return -1;
        switch (record->envelope.type) {
        case SDAF_RECORD_SCHEMA: {
            const sdaf_schema *schema = &record->value.schema; size_t object;
            if (cbor_text(output, "schema_id") != 0 || cbor_uint(output, schema->id) != 0 || cbor_text(output, "schema_revision") != 0 || cbor_uint(output, schema->revision) != 0 || cbor_text(output, "objects") != 0 || cbor_head(output, 4u, schema->object_count) != 0) return -1;
            for (object = 0u; object < schema->object_count; ++object) { const sdaf_schema_object *o = &schema->objects[object]; const sdaf_tlv *object_name = sdaf_object_find_tlv(o, 1u, 0u); if (cbor_head(output, 5u, 3u) != 0 || cbor_text(output, "kind") != 0 || cbor_text(output, object_kind_name(o->kind)) != 0 || cbor_text(output, "id") != 0 || cbor_uint(output, o->id) != 0 || cbor_text(output, "name") != 0 || (object_name != NULL ? cbor_text_n(output, (const char *)object_name->value, object_name->size) : cbor_text(output, "")) != 0) return -1; }
            break;
        }
        case SDAF_RECORD_DATA: {
            const sdaf_data_record *data = &record->value.data; size_t sample;
            if (cbor_text(output, "schema_id") != 0 || cbor_uint(output, data->schema_id) != 0 || cbor_text(output, "stream_id") != 0 || cbor_uint(output, data->stream_id) != 0 || cbor_text(output, "sample_count") != 0 || cbor_uint(output, data->sample_count) != 0 || cbor_text(output, "samples") != 0 || cbor_head(output, 4u, data->samples != NULL ? data->sample_count : 0u) != 0) return -1;
            if (data->samples != NULL) for (sample = 0u; sample < data->sample_count; ++sample) if (cbor_sample(output, &data->samples[sample]) != 0) return -1;
            break;
        }
        case SDAF_RECORD_TEXT:
            if (cbor_text(output, "schema_id") != 0 || cbor_uint(output, record->value.text.schema_id) != 0 || cbor_text(output, "stream_id") != 0 || cbor_uint(output, record->value.text.stream_id) != 0 || cbor_text(output, "time_ticks") != 0 || cbor_int(output, record->value.text.time_ticks) != 0 || cbor_text(output, "severity") != 0 || cbor_uint(output, record->value.text.severity) != 0 || cbor_text(output, "message") != 0 || cbor_text_n(output, record->value.text.message, record->value.text.message_size) != 0) return -1;
            break;
        case SDAF_RECORD_BLOB:
            if (cbor_text(output, "schema_id") != 0 || cbor_uint(output, record->value.blob.schema_id) != 0 || cbor_text(output, "stream_id") != 0 || cbor_uint(output, record->value.blob.stream_id) != 0 || cbor_text(output, "item_index") != 0 || cbor_uint(output, record->value.blob.item_index) != 0 || cbor_text(output, "time_ticks") != 0 || cbor_int(output, record->value.blob.time_ticks) != 0 || cbor_text(output, "data") != 0 || cbor_bytes(output, record->value.blob.decoded_payload, record->value.blob.decoded_payload_size) != 0) return -1;
            break;
        case SDAF_RECORD_NOTE:
            if (cbor_text(output, "message") != 0 || cbor_text_n(output, record->value.note.message, record->value.note.size) != 0) return -1;
            break;
        case SDAF_RECORD_INDEX:
            if (cbor_text(output, "entry_count") != 0 || cbor_uint(output, record->value.index.entry_count) != 0) return -1;
            break;
        case SDAF_RECORD_END:
            if (cbor_text(output, "total_record_count") != 0 || cbor_uint(output, record->value.end.total_records) != 0 || cbor_text(output, "total_data_record_count") != 0 || cbor_uint(output, record->value.end.total_data_records) != 0 || cbor_text(output, "last_index_offset") != 0 || cbor_uint(output, record->value.end.last_index_offset) != 0) return -1;
            break;
        default:
            if (cbor_text(output, "payload") != 0 || cbor_bytes(output, record->value.unknown.payload, record->value.unknown.payload_size) != 0) return -1;
            break;
        }
    }
    return 0;
}
