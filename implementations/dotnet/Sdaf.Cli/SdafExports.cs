using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Sdaf.Cli;

internal static class SdafExports
{
    internal static void WriteJson(
        Stream output,
        SdafFileHeader header,
        IReadOnlyList<SdafRecord> records
    )
    {
        using var json = new Utf8JsonWriter(output, new JsonWriterOptions { Indented = true });
        json.WriteStartObject();
        WriteHeader(json, header);
        json.WriteStartArray("records");
        foreach (SdafRecord record in records)
            WriteRecord(json, record);
        json.WriteEndArray();
        json.WriteEndObject();
        json.Flush();
        output.WriteByte((byte)'\n');
    }

    private static void WriteHeader(Utf8JsonWriter j, SdafFileHeader h)
    {
        j.WriteStartObject("header");
        j.WriteNumber("major", h.Major);
        j.WriteNumber("minor", h.Minor);
        j.WriteNumber("created_unix_ns", h.CreatedUnixNanoseconds);
        j.WriteString("file_uuid", Convert.ToHexString(h.FileUuid).ToLowerInvariant());
        j.WriteNumber("first_record_offset", h.FirstRecordOffset);
        j.WriteEndObject();
    }

    private static void WriteRecord(Utf8JsonWriter j, SdafRecord record)
    {
        j.WriteStartObject();
        j.WriteString("type", TypeName(record.Envelope.RawRecordType));
        j.WriteNumber("type_id", record.Envelope.RawRecordType);
        j.WriteNumber("sequence", record.Envelope.Sequence);
        j.WriteNumber("offset", record.Envelope.FileOffset);
        j.WriteNumber("stored_payload_bytes", record.Envelope.PayloadSize);
        switch (record)
        {
            case SdafSchemaRecord r:
                j.WriteNumber("schema_id", r.Schema.Id);
                j.WriteNumber("schema_revision", r.Schema.Revision);
                j.WriteStartArray("objects");
                foreach (SdafSchemaObject o in r.Schema.Objects)
                {
                    j.WriteStartObject();
                    j.WriteString("kind", o.Kind.ToString());
                    j.WriteNumber("id", o.Id);
                    j.WriteString("name", o.Name);
                    j.WriteEndObject();
                }
                j.WriteEndArray();
                break;
            case SdafDataRecord r:
                j.WriteNumber("schema_id", r.SchemaId);
                j.WriteNumber("schema_revision", r.SchemaRevision);
                j.WriteNumber("stream_id", r.StreamId);
                j.WriteNumber("sample_count", r.SampleCount);
                WriteTransforms(j, r.Transforms);
                j.WriteStartArray("samples");
                if (r.Samples is not null)
                    foreach (SdafSample sample in r.Samples)
                        WriteSample(j, sample);
                j.WriteEndArray();
                if (r.Samples is null)
                    j.WriteBoolean("decoded", false);
                break;
            case SdafTextRecord r:
                j.WriteNumber("schema_id", r.SchemaId);
                j.WriteNumber("schema_revision", r.SchemaRevision);
                j.WriteNumber("stream_id", r.StreamId);
                j.WriteNumber("time_ticks", r.TimeTicks);
                j.WriteString("severity", r.Severity.ToString().ToLowerInvariant());
                if (r.SourceId is uint source)
                    j.WriteNumber("source_id", source);
                if (r.EventCode is int code)
                    j.WriteNumber("event_code", code);
                j.WriteString("message", r.Message);
                break;
            case SdafBlobRecord r:
                j.WriteNumber("schema_id", r.SchemaId);
                j.WriteNumber("schema_revision", r.SchemaRevision);
                j.WriteNumber("stream_id", r.StreamId);
                j.WriteNumber("item_index", r.ItemIndex);
                j.WriteNumber("time_ticks", r.TimeTicks);
                j.WriteNumber("decoded_bytes", r.DecodedBytes);
                WriteTransforms(j, r.Transforms);
                if (r.DecodedPayload is not null)
                    j.WriteBase64String("data", r.DecodedPayload);
                else
                    j.WriteBoolean("decoded", false);
                break;
            case SdafNoteRecord r:
                j.WriteString("message", r.Message);
                break;
            case SdafIndexRecord r:
                j.WriteNumber("entry_count", r.Entries.Count);
                break;
            case SdafEndRecord r:
                j.WriteNumber("total_record_count", r.TotalRecordCount);
                j.WriteNumber("total_data_record_count", r.TotalDataRecordCount);
                j.WriteNumber("last_index_offset", r.LastIndexOffset);
                break;
            case SdafUnknownRecord r:
                j.WriteBase64String("payload", r.Payload);
                break;
        }
        j.WriteEndObject();
    }

    private static void WriteTransforms(Utf8JsonWriter j, IReadOnlyList<SdafTransform> transforms)
    {
        j.WriteStartArray("transforms");
        foreach (SdafTransform t in transforms)
            j.WriteNumberValue(t.Id);
        j.WriteEndArray();
    }

    private static void WriteSample(Utf8JsonWriter j, SdafSample sample)
    {
        j.WriteStartObject();
        j.WriteNumber("index", sample.Index);
        if (sample.TimeTicks is long time)
            j.WriteNumber("time_ticks", time);
        j.WriteStartArray("values");
        foreach (SdafSampleValue value in sample.Values)
        {
            j.WriteStartObject();
            j.WriteNumber("channel_id", value.ChannelId);
            j.WriteString("name", value.Name);
            if (value.ElementIndex != 0)
                j.WriteNumber("element", value.ElementIndex);
            if (value.Bytes is not null)
                j.WriteBase64String("raw", value.Bytes);
            else if (value.LogicalType == SdafLogicalType.SignedInteger)
                j.WriteNumber("raw", value.RawSigned);
            else if (value.LogicalType == SdafLogicalType.Float)
                j.WriteNumber("raw", value.NumericValue);
            else
                j.WriteNumber("raw", value.RawUnsigned);
            if (value.Bytes is null)
                j.WriteNumber("physical", value.PhysicalValue);
            if (value.Unit is not null)
                j.WriteString("unit", value.Unit);
            j.WriteEndObject();
        }
        j.WriteEndArray();
        j.WriteEndObject();
    }

    internal static void WriteCsv(Stream output, IReadOnlyList<SdafRecord> records)
    {
        using var writer = new StreamWriter(output, new UTF8Encoding(false), 4096, true)
        {
            NewLine = "\n",
        };
        writer.WriteLine(
            "sequence,stream_id,sample_index,time_ticks,channel_id,channel_name,element,raw,physical,unit"
        );
        foreach (SdafDataRecord record in records.OfType<SdafDataRecord>())
            if (record.Samples is not null)
                foreach (SdafSample sample in record.Samples)
                foreach (SdafSampleValue value in sample.Values)
                {
                    WriteCell(
                        writer,
                        record.Envelope.Sequence.ToString(CultureInfo.InvariantCulture)
                    );
                    WriteCell(writer, record.StreamId.ToString(CultureInfo.InvariantCulture));
                    WriteCell(writer, sample.Index.ToString(CultureInfo.InvariantCulture));
                    WriteCell(
                        writer,
                        sample.TimeTicks?.ToString(CultureInfo.InvariantCulture) ?? ""
                    );
                    WriteCell(writer, value.ChannelId.ToString(CultureInfo.InvariantCulture));
                    WriteCell(writer, value.Name);
                    WriteCell(writer, value.ElementIndex.ToString(CultureInfo.InvariantCulture));
                    string raw =
                        value.Bytes is not null
                            ? Convert.ToHexString(value.Bytes).ToLowerInvariant()
                        : value.LogicalType == SdafLogicalType.SignedInteger
                            ? value.RawSigned.ToString(CultureInfo.InvariantCulture)
                        : value.LogicalType == SdafLogicalType.Float
                            ? value.NumericValue.ToString("R", CultureInfo.InvariantCulture)
                        : value.RawUnsigned.ToString(CultureInfo.InvariantCulture);
                    WriteCell(writer, raw);
                    WriteCell(
                        writer,
                        value.Bytes is null
                            ? value.PhysicalValue.ToString("R", CultureInfo.InvariantCulture)
                            : ""
                    );
                    WriteLastCell(writer, value.Unit ?? "");
                }
        writer.Flush();
    }

    private static void WriteCell(TextWriter writer, string value)
    {
        WriteEscaped(writer, value);
        writer.Write(',');
    }

    private static void WriteLastCell(TextWriter writer, string value)
    {
        WriteEscaped(writer, value);
        writer.WriteLine();
    }

    private static void WriteEscaped(TextWriter writer, string value)
    {
        if (value.IndexOfAny([',', '"', '\r', '\n']) < 0)
        {
            writer.Write(value);
            return;
        }
        writer.Write('"');
        writer.Write(value.Replace("\"", "\"\"", StringComparison.Ordinal));
        writer.Write('"');
    }

    internal static void WriteCbor(
        Stream output,
        SdafFileHeader header,
        IReadOnlyList<SdafRecord> records
    )
    {
        var c = new MiniCborWriter(output);
        c.Map(2);
        c.Text("header");
        c.Map(5);
        c.Text("major");
        c.UInt(header.Major);
        c.Text("minor");
        c.UInt(header.Minor);
        c.Text("created_unix_ns");
        c.Int(header.CreatedUnixNanoseconds);
        c.Text("file_uuid");
        c.Bytes(header.FileUuid);
        c.Text("first_record_offset");
        c.UInt(header.FirstRecordOffset);
        c.Text("records");
        c.Array(records.Count);
        foreach (SdafRecord r in records)
            WriteCborRecord(c, r);
    }

    private static void WriteCborRecord(MiniCborWriter c, SdafRecord r)
    {
        // A compact stable summary plus fully decoded DATA/TEXT/BLOB content.
        int extras = r switch
        {
            SdafDataRecord => 4,
            SdafTextRecord => 5,
            SdafBlobRecord => 5,
            SdafSchemaRecord => 3,
            SdafNoteRecord => 1,
            SdafEndRecord => 3,
            SdafIndexRecord => 1,
            SdafUnknownRecord => 1,
            _ => 0,
        };
        c.Map(3 + extras);
        c.Text("type");
        c.Text(TypeName(r.Envelope.RawRecordType));
        c.Text("type_id");
        c.UInt(r.Envelope.RawRecordType);
        c.Text("sequence");
        c.UInt(r.Envelope.Sequence);
        switch (r)
        {
            case SdafSchemaRecord x:
                c.Text("schema_id");
                c.UInt(x.Schema.Id);
                c.Text("schema_revision");
                c.UInt(x.Schema.Revision);
                c.Text("objects");
                c.Array(x.Schema.Objects.Count);
                foreach (var o in x.Schema.Objects)
                {
                    c.Map(3);
                    c.Text("kind");
                    c.Text(o.Kind.ToString());
                    c.Text("id");
                    c.UInt(o.Id);
                    c.Text("name");
                    c.Text(o.Name);
                }
                break;
            case SdafDataRecord x:
                c.Text("schema_id");
                c.UInt(x.SchemaId);
                c.Text("stream_id");
                c.UInt(x.StreamId);
                c.Text("sample_count");
                c.UInt(x.SampleCount);
                c.Text("samples");
                c.Array(x.Samples?.Count ?? 0);
                if (x.Samples is not null)
                    foreach (var s in x.Samples)
                        WriteCborSample(c, s);
                break;
            case SdafTextRecord x:
                c.Text("schema_id");
                c.UInt(x.SchemaId);
                c.Text("stream_id");
                c.UInt(x.StreamId);
                c.Text("time_ticks");
                c.Int(x.TimeTicks);
                c.Text("severity");
                c.Text(x.Severity.ToString().ToLowerInvariant());
                c.Text("message");
                c.Text(x.Message);
                break;
            case SdafBlobRecord x:
                c.Text("schema_id");
                c.UInt(x.SchemaId);
                c.Text("stream_id");
                c.UInt(x.StreamId);
                c.Text("item_index");
                c.UInt(x.ItemIndex);
                c.Text("time_ticks");
                c.Int(x.TimeTicks);
                c.Text("data");
                c.Bytes(x.DecodedPayload ?? []);
                break;
            case SdafNoteRecord x:
                c.Text("message");
                c.Text(x.Message);
                break;
            case SdafEndRecord x:
                c.Text("total_record_count");
                c.UInt(x.TotalRecordCount);
                c.Text("total_data_record_count");
                c.UInt(x.TotalDataRecordCount);
                c.Text("last_index_offset");
                c.UInt(x.LastIndexOffset);
                break;
            case SdafIndexRecord x:
                c.Text("entry_count");
                c.UInt((ulong)x.Entries.Count);
                break;
            case SdafUnknownRecord x:
                c.Text("payload");
                c.Bytes(x.Payload);
                break;
        }
    }

    private static void WriteCborSample(MiniCborWriter c, SdafSample sample)
    {
        c.Map(sample.TimeTicks.HasValue ? 3 : 2);
        c.Text("index");
        c.UInt(sample.Index);
        if (sample.TimeTicks.HasValue)
        {
            c.Text("time_ticks");
            c.Int(sample.TimeTicks.Value);
        }
        c.Text("values");
        c.Array(sample.Values.Count);
        foreach (var v in sample.Values)
        {
            c.Map(v.Bytes is null ? 4 : 3);
            c.Text("channel_id");
            c.UInt(v.ChannelId);
            c.Text("name");
            c.Text(v.Name);
            c.Text("raw");
            if (v.Bytes is not null)
                c.Bytes(v.Bytes);
            else if (v.LogicalType == SdafLogicalType.SignedInteger)
                c.Int(v.RawSigned);
            else if (v.LogicalType == SdafLogicalType.Float)
                c.Float(v.NumericValue);
            else
                c.UInt(v.RawUnsigned);
            if (v.Bytes is null)
            {
                c.Text("physical");
                c.Float(v.PhysicalValue);
            }
        }
    }

    private static string TypeName(ushort type) =>
        type switch
        {
            1 => "SCMA",
            2 => "DATA",
            3 => "TEXT",
            4 => "INDX",
            5 => "END!",
            6 => "BLOB",
            0x7fff => "NOTE",
            _ => $"0x{type:x4}",
        };
}

internal sealed class MiniCborWriter(Stream output)
{
    internal void UInt(ulong value) => Head(0, value);

    internal void Int(long value)
    {
        if (value >= 0)
            UInt((ulong)value);
        else
            Head(1, (ulong)(-1 - value));
    }

    internal void Bytes(ReadOnlySpan<byte> value)
    {
        Head(2, (ulong)value.Length);
        output.Write(value);
    }

    internal void Text(string value)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(value);
        Head(3, (ulong)bytes.Length);
        output.Write(bytes);
    }

    internal void Array(int count) => Head(4, (ulong)count);

    internal void Map(int count) => Head(5, (ulong)count);

    internal void Float(double value)
    {
        output.WriteByte(0xfb);
        Span<byte> b = stackalloc byte[8];
        System.Buffers.Binary.BinaryPrimitives.WriteInt64BigEndian(
            b,
            BitConverter.DoubleToInt64Bits(value)
        );
        output.Write(b);
    }

    private void Head(byte major, ulong value)
    {
        if (value < 24)
        {
            output.WriteByte((byte)((major << 5) | (byte)value));
            return;
        }
        if (value <= byte.MaxValue)
        {
            output.WriteByte((byte)((major << 5) | 24));
            output.WriteByte((byte)value);
            return;
        }
        Span<byte> b = stackalloc byte[8];
        int count;
        if (value <= ushort.MaxValue)
        {
            output.WriteByte((byte)((major << 5) | 25));
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16BigEndian(b, (ushort)value);
            count = 2;
        }
        else if (value <= uint.MaxValue)
        {
            output.WriteByte((byte)((major << 5) | 26));
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(b, (uint)value);
            count = 4;
        }
        else
        {
            output.WriteByte((byte)((major << 5) | 27));
            System.Buffers.Binary.BinaryPrimitives.WriteUInt64BigEndian(b, value);
            count = 8;
        }
        output.Write(b[..count]);
    }
}
