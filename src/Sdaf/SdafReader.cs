using System.Buffers.Binary;
using System.Text;

namespace Sdaf;

public sealed class SdafReader : IDisposable
{
    private static ReadOnlySpan<byte> Magic => "SDAF\r\n\x1a\n"u8;
    private readonly Stream _stream;
    private readonly bool _leaveOpen;
    private readonly Dictionary<(uint, uint), SdafSchema> _schemas = new();
    private readonly Dictionary<(uint, uint), byte[]> _schemaPayloads = new();
    private readonly Dictionary<long, SdafRecord> _recordsByOffset = new();
    private readonly Dictionary<(uint SchemaId, uint StreamId), ulong> _lastBlobIndexes = new();
    private bool _enumerated;

    public SdafFileHeader Header { get; }
    public SdafLimits Limits { get; }
    public IReadOnlyDictionary<(uint SchemaId, uint Revision), SdafSchema> Schemas => _schemas;

    public SdafReader(Stream stream, SdafLimits? limits = null, bool leaveOpen = false)
    {
        _stream = stream ?? throw new ArgumentNullException(nameof(stream));
        if (!stream.CanRead) throw new ArgumentException("Stream must be readable.", nameof(stream));
        Limits = limits ?? new SdafLimits();
        _leaveOpen = leaveOpen;
        Header = ReadFileHeader();
    }

    public static SdafReader Open(string path, SdafLimits? limits = null) =>
        new(File.OpenRead(path), limits);

    public IEnumerable<SdafRecord> ReadRecords()
    {
        if (_enumerated) throw new InvalidOperationException("An SdafReader is forward-only and can be enumerated once.");
        _enumerated = true;
        long offset = checked((long)Header.FirstRecordOffset);
        while (true)
        {
            byte[] common = new byte[32];
            int first = _stream.ReadByte();
            if (first < 0) yield break;
            common[0] = (byte)first;
            if (!Bin.TryReadExactly(_stream, common.AsSpan(1))) yield break;
            if (!common.AsSpan(0, 4).SequenceEqual("SDRC"u8)) throw Error(offset, "Invalid record marker.");

            ushort rawType = Bin.U16(common.AsSpan(4));
            ushort flags = Bin.U16(common.AsSpan(6));
            ushort headerSize = Bin.U16(common.AsSpan(8));
            byte version = common[10];
            uint sequence = Bin.U32(common.AsSpan(12));
            ulong payloadSize = Bin.U64(common.AsSpan(16));
            uint expectedHeaderCrc = Bin.U32(common.AsSpan(24));
            uint leadingPayloadCrc = Bin.U32(common.AsSpan(28));
            if ((flags & 0x00fe) != 0) throw Error(offset, "Reserved common envelope flag is set.");
            if (headerSize < 32 || headerSize > Limits.MaxHeaderSize) throw Error(offset, "Record header size is outside configured limits.");
            if (version != 1 || common[11] != 0) throw Error(offset, "Unsupported record version or nonzero reserved byte.");
            if (payloadSize > Limits.MaxPayloadSize) throw Error(offset, "Record payload exceeds configured limit.");
            bool trailerMode = (flags & 1) != 0;
            if (trailerMode && leadingPayloadCrc != 0) throw Error(offset, "Trailer-mode record has a leading payload CRC.");
            try { _ = checked((ulong)headerSize + payloadSize + (trailerMode ? 16UL : 0)); }
            catch (OverflowException) { throw Error(offset, "Record size overflows."); }

            byte[] header = new byte[headerSize];
            common.CopyTo(header, 0);
            if (!Bin.TryReadExactly(_stream, header.AsSpan(32))) yield break;
            Bin.U32(header.AsSpan(24), 0);
            if (SdafCrc32C.Compute(header) != expectedHeaderCrc) throw Error(offset, "Record header CRC-32C mismatch.");
            Bin.U32(header.AsSpan(24), expectedHeaderCrc);
            byte[] payload = new byte[Bin.CheckedInt(payloadSize, "payload_size")];
            if (!Bin.TryReadExactly(_stream, payload)) yield break;
            uint actualPayloadCrc = SdafCrc32C.Compute(payload);
            if (!trailerMode)
            {
                if (actualPayloadCrc != leadingPayloadCrc) throw Error(offset, "Payload CRC-32C mismatch.");
            }
            else
            {
                byte[] trailer = new byte[16];
                if (!Bin.TryReadExactly(_stream, trailer)) yield break;
                if (!trailer.AsSpan(0, 4).SequenceEqual("SDCT"u8) || Bin.U16(trailer.AsSpan(4)) != 16 || trailer[6] != 1 || trailer[7] != 0)
                    throw Error(offset, "Invalid payload CRC trailer.");
                if (Bin.U32(trailer.AsSpan(8)) != sequence) throw Error(offset, "Payload CRC trailer sequence mismatch.");
                if (Bin.U32(trailer.AsSpan(12)) != actualPayloadCrc) throw Error(offset, "Trailer payload CRC-32C mismatch.");
            }

            SdafRecordType recordType = (SdafRecordType)rawType;
            var envelope = new SdafRecordEnvelope(recordType, rawType, flags, headerSize, version, sequence, payloadSize, offset, trailerMode);
            SdafRecord record = ParseRecord(envelope, header.AsSpan(32), payload);
            _recordsByOffset[offset] = record;
            yield return record;
            offset = checked(offset + headerSize + (long)payloadSize + (trailerMode ? 16 : 0));
        }
    }

    private SdafFileHeader ReadFileHeader()
    {
        byte[] first = new byte[64];
        try { _stream.ReadExactly(first); }
        catch (EndOfStreamException) { throw new SdafFormatException("File is shorter than the SDAF header."); }
        if (!first.AsSpan(0, 8).SequenceEqual(Magic)) throw new SdafFormatException("Invalid SDAF magic.");
        if (first[8] != 1) throw new SdafFormatException($"Unsupported SDAF major version {first[8]}.");
        ushort headerSize = Bin.U16(first.AsSpan(10));
        if (headerSize < 64 || headerSize > Limits.MaxHeaderSize) throw new SdafFormatException("File header size is outside configured limits.");
        byte[] header = headerSize == 64 ? first : new byte[headerSize];
        if (headerSize != 64)
        {
            first.CopyTo(header, 0);
            try { _stream.ReadExactly(header.AsSpan(64)); }
            catch (EndOfStreamException) { throw new SdafFormatException("File ends inside its declared header."); }
        }
        uint expected = Bin.U32(header.AsSpan(56));
        Bin.U32(header.AsSpan(56), 0);
        if (SdafCrc32C.Compute(header) != expected) throw new SdafFormatException("File header CRC-32C mismatch.");
        if (Bin.U32(header.AsSpan(12)) != 0x12345678 || Bin.U32(header.AsSpan(16)) != 0 || Bin.U32(header.AsSpan(20)) != 0 || Bin.U32(header.AsSpan(60)) != 0)
            throw new SdafFormatException("Invalid byte-order check, feature flags, or reserved file-header field.");
        ulong firstRecord = Bin.U64(header.AsSpan(48));
        if (firstRecord != headerSize) throw new SdafFormatException("first_record_offset must equal the declared header size.");
        return new SdafFileHeader(header[8], header[9], Bin.I64(header.AsSpan(24)), header.AsSpan(32, 16).ToArray(), firstRecord);
    }

    private SdafRecord ParseRecord(SdafRecordEnvelope e, ReadOnlySpan<byte> typeHeader, byte[] payload)
    {
        return e.RawRecordType switch
        {
            (ushort)SdafRecordType.Schema => ParseSchema(e, typeHeader, payload),
            (ushort)SdafRecordType.Data => ParseData(e, typeHeader, payload),
            (ushort)SdafRecordType.Text => ParseText(e, typeHeader, payload),
            (ushort)SdafRecordType.Blob => ParseBlob(e, typeHeader, payload),
            (ushort)SdafRecordType.Index => ParseIndex(e, typeHeader, payload),
            (ushort)SdafRecordType.End => ParseEnd(e, typeHeader, payload),
            (ushort)SdafRecordType.Note => ParseNote(e, typeHeader, payload),
            _ => new SdafUnknownRecord(e, typeHeader.ToArray(), payload),
        };
    }

    private SdafSchemaRecord ParseSchema(SdafRecordEnvelope e, ReadOnlySpan<byte> h, byte[] payload)
    {
        RequireHeader(e, h, 16, 0);
        uint id = Bin.U32(h); uint revision = Bin.U32(h[4..]); uint count = Bin.U32(h[8..]);
        if (id == 0 || revision == 0 || count > Limits.MaxSchemaObjects || Bin.U32(h[12..]) != 0) throw Error(e.FileOffset, "Invalid schema header.");
        var objects = new List<SdafSchemaObject>(checked((int)count));
        int offset = 0;
        for (uint i = 0; i < count; i++)
        {
            if (payload.Length - offset < 12) throw Error(e.FileOffset, "Schema object header overruns payload.");
            ReadOnlySpan<byte> oh = payload.AsSpan(offset, 12);
            byte kind = oh[0]; uint size = Bin.U32(oh[4..]); uint objectId = Bin.U32(oh[8..]);
            if (kind is < 1 or > 6 || oh[1] != 0 || Bin.U16(oh[2..]) != 12 || size < 12 || size > payload.Length - offset || objectId == 0)
                throw Error(e.FileOffset, "Invalid schema object envelope.");
            var tlvs = ParseTlvs(payload.AsSpan(offset + 12, checked((int)size - 12)), e.FileOffset);
            objects.Add(new SdafSchemaObject((SdafObjectKind)kind, objectId, tlvs));
            offset += checked((int)size);
        }
        if (offset != payload.Length) throw Error(e.FileOffset, "Schema object_count does not consume the payload exactly.");
        var schema = new SdafSchema(id, revision, objects);
        ValidateSchema(schema, e.FileOffset);
        var key = (id, revision);
        if (_schemaPayloads.TryGetValue(key, out byte[]? old) && !old.AsSpan().SequenceEqual(payload)) throw Error(e.FileOffset, "Schema revision was redefined with different bytes.");
        _schemaPayloads[key] = payload.ToArray(); _schemas[key] = schema;
        return new SdafSchemaRecord(e, schema);
    }

    private SdafDataRecord ParseData(SdafRecordEnvelope e, ReadOnlySpan<byte> h, byte[] payload)
    {
        RequireHeader(e, h, 64, 0);
        uint schemaId = Bin.U32(h); uint revision = Bin.U32(h[4..]); uint streamId = Bin.U32(h[8..]); uint sampleCount = Bin.U32(h[12..]);
        ulong first = Bin.U64(h[16..]); long start = Bin.I64(h[24..]); ulong pn = Bin.U64(h[32..]); ulong pd = Bin.U64(h[40..]);
        var tm = (SdafTimestampMode)h[48]; var layout = (SdafLayout)h[49]; var packing = (SdafPacking)h[50]; int transformCount = h[51];
        uint timestampBytes = Bin.U32(h[52..]); ulong decodedBytes = Bin.U64(h[56..]);
        if (schemaId == 0 || revision == 0 || streamId == 0 || tm is < SdafTimestampMode.Periodic or > SdafTimestampMode.None || layout is < SdafLayout.Interleaved or > SdafLayout.Planar || packing is < SdafPacking.Lsb0Dense or > SdafPacking.ByteAligned)
            throw Error(e.FileOffset, "Invalid DATA header.");
        if (decodedBytes > Limits.MaxDecodedSize) throw Error(e.FileOffset, "Decoded DATA size exceeds configured limit.");
        ValidateTimestampHeader(tm, sampleCount, start, pn, pd, timestampBytes, e.FileOffset);
        var (transforms, encodedOffset) = ParseTransforms(payload, transformCount, e.FileOffset);
        var dataHeader = new SdafDataHeader(sampleCount, first, start, pn, pd, tm, layout, packing, timestampBytes, decodedBytes);
        byte[]? decoded = null; IReadOnlyList<SdafSample>? samples = null;
        if (_schemas.TryGetValue((schemaId, revision), out SdafSchema? schema))
        {
            ValidateStream(schema, streamId, 1, 4, e.FileOffset);
            ChannelInfo[] channels = SampleCodec.GetChannels(schema, streamId);
            if ((schema.Find(SdafObjectKind.Stream, streamId)?.Find(106)?.AsUInt8() ?? 1) == 4 && tm != SdafTimestampMode.None) throw Error(e.FileOffset, "Clock-correlation DATA must use timestamp mode 4.");
            if ((uint)channels.Length > Limits.MaxChannelsPerStream) throw Error(e.FileOffset, "Stream exceeds configured channel limit.");
            ulong expected = SampleCodec.ExpectedCanonicalBytes(channels, sampleCount, timestampBytes, packing);
            if (expected != decodedBytes) throw Error(e.FileOffset, $"decoded_sample_bytes is {decodedBytes}; schema requires {expected}.");
            if (IsSupportedDataTransforms(transforms))
            {
                decoded = SampleCodec.DecodeTransforms(payload.AsSpan(encodedOffset).ToArray(), transforms, channels, dataHeader);
                if ((ulong)decoded.Length != decodedBytes) throw Error(e.FileOffset, "Decoded DATA size mismatch.");
                samples = SampleCodec.DecodeSamples(decoded, channels, dataHeader);
            }
        }
        return new SdafDataRecord(e, schemaId, revision, streamId, sampleCount, first, start, pn, pd, tm, layout, packing, timestampBytes, decodedBytes, transforms, payload, decoded, samples);
    }

    private SdafTextRecord ParseText(SdafRecordEnvelope e, ReadOnlySpan<byte> h, byte[] payload)
    {
        RequireHeader(e, h, 32, 0x0300);
        uint schemaId = Bin.U32(h); uint streamId = Bin.U32(h[4..]); long time = Bin.I64(h[8..]); byte severity = h[16]; byte encoding = h[17];
        uint source = Bin.U32(h[20..]); int eventCode = Bin.I32(h[24..]); uint revision = Bin.U32(h[28..]);
        if (severity > 6 || encoding != 1 || Bin.U16(h[18..]) != 0 || payload.Length > Limits.MaxUtf8Bytes) throw Error(e.FileOffset, "Invalid TEXT header.");
        bool sourcePresent = (e.Flags & 0x0100) != 0, eventPresent = (e.Flags & 0x0200) != 0;
        if (!sourcePresent && source != 0 || !eventPresent && eventCode != 0) throw Error(e.FileOffset, "Absent TEXT metadata field is nonzero.");
        if (schemaId == 0)
        {
            if (revision != 0 || streamId != 0 || sourcePresent || eventPresent) throw Error(e.FileOffset, "Invalid global TEXT references.");
        }
        else
        {
            if (revision == 0 || streamId == 0) throw Error(e.FileOffset, "Incomplete stream TEXT references.");
            if (_schemas.TryGetValue((schemaId, revision), out var schema)) ValidateStream(schema, streamId, 2, 2, e.FileOffset);
        }
        string message;
        try { message = Bin.Utf8.GetString(payload); } catch (DecoderFallbackException ex) { throw new SdafFormatException($"Invalid TEXT UTF-8 at offset {e.FileOffset}: {ex.Message}"); }
        return new SdafTextRecord(e, schemaId, revision, streamId, time, (SdafTextSeverity)severity, sourcePresent ? source : null, eventPresent ? eventCode : null, message);
    }

    private SdafBlobRecord ParseBlob(SdafRecordEnvelope e, ReadOnlySpan<byte> h, byte[] payload)
    {
        RequireHeader(e, h, 48, 0);
        uint schemaId = Bin.U32(h), revision = Bin.U32(h[4..]), streamId = Bin.U32(h[8..]);
        ulong index = Bin.U64(h[16..]); long time = Bin.I64(h[24..]); ulong decodedBytes = Bin.U64(h[32..]); int transformCount = h[40];
        if (schemaId == 0 || revision == 0 || streamId == 0 || Bin.U32(h[12..]) != 0 || !Bin.AllZero(h[41..]) || transformCount > 1 || decodedBytes > Limits.MaxDecodedSize)
            throw Error(e.FileOffset, "Invalid BLOB header.");
        var (transforms, encodedOffset) = ParseTransforms(payload, transformCount, e.FileOffset);
        if (transforms.Count == 1 && (transforms[0].Id != 16 || transforms[0].Version != 1 || transforms[0].Parameters.Length != 0))
            throw Error(e.FileOffset, "BLOB permits only a single parameterless Zstandard transform.");
        if (_schemas.TryGetValue((schemaId, revision), out var schema)) ValidateStream(schema, streamId, 3, 3, e.FileOffset);
        if (_lastBlobIndexes.TryGetValue((schemaId, streamId), out ulong lastIndex) && index <= lastIndex) throw Error(e.FileOffset, "BLOB item_index is not strictly increasing within its stream.");
        _lastBlobIndexes[(schemaId, streamId)] = index;
        byte[]? decoded = transforms.Count switch
        {
            0 => payload.AsSpan(encodedOffset).ToArray(),
            1 => Zstandard.Decompress(payload.AsSpan(encodedOffset), Bin.CheckedInt(decodedBytes, "decoded_bytes")),
            _ => null,
        };
        if (decoded is not null && (ulong)decoded.Length != decodedBytes) throw Error(e.FileOffset, "Decoded BLOB size mismatch.");
        return new SdafBlobRecord(e, schemaId, revision, streamId, index, time, decodedBytes, transforms, payload, decoded);
    }

    private SdafIndexRecord ParseIndex(SdafRecordEnvelope e, ReadOnlySpan<byte> h, byte[] payload)
    {
        RequireHeader(e, h, 16, 0); uint count = Bin.U32(h);
        if (!Bin.AllZero(h[4..]) || (ulong)payload.Length != checked((ulong)count * 48)) throw Error(e.FileOffset, "Invalid INDX size or reserved field.");
        var entries = new SdafIndexEntry[count];
        for (int i = 0; i < entries.Length; i++)
        {
            ReadOnlySpan<byte> p = payload.AsSpan(i * 48, 48);
            if (Bin.U32(p[28..]) != 0) throw Error(e.FileOffset, "INDX entry has nonzero reserved field.");
            entries[i] = new(Bin.U64(p), Bin.U32(p[8..]), Bin.U32(p[12..]), Bin.U64(p[16..]), Bin.U32(p[24..]), Bin.I64(p[32..]), Bin.I64(p[40..]));
            if (entries[i].RecordOffset > long.MaxValue || !_recordsByOffset.TryGetValue((long)entries[i].RecordOffset, out SdafRecord? target)) throw Error(e.FileOffset, "INDX entry does not reference a prior record boundary.");
            bool matches = target switch
            {
                SdafDataRecord d => entries[i].Sequence == d.Envelope.Sequence && entries[i].StreamId == d.StreamId && entries[i].FirstSampleIndex == d.FirstSampleIndex && entries[i].SampleCount == d.SampleCount,
                SdafBlobRecord b => entries[i].Sequence == b.Envelope.Sequence && entries[i].StreamId == b.StreamId && entries[i].FirstSampleIndex == b.ItemIndex && entries[i].SampleCount == 1,
                _ => false,
            };
            if (!matches) throw Error(e.FileOffset, "INDX entry metadata does not match its referenced DATA or BLOB record.");
        }
        return new SdafIndexRecord(e, entries);
    }

    private SdafEndRecord ParseEnd(SdafRecordEnvelope e, ReadOnlySpan<byte> h, byte[] payload)
    {
        RequireHeader(e, h, 32, 0);
        if (payload.Length != 0 || !Bin.AllZero(h[24..])) throw Error(e.FileOffset, "Invalid END record.");
        ulong total = Bin.U64(h), dataTotal = Bin.U64(h[8..]), lastIndex = Bin.U64(h[16..]);
        if (total != (ulong)_recordsByOffset.Count + 1 || dataTotal != (ulong)_recordsByOffset.Values.Count(x => x is SdafDataRecord)) throw Error(e.FileOffset, "END record counts do not match the scanned records.");
        if (lastIndex != 0 && (lastIndex > long.MaxValue || !_recordsByOffset.TryGetValue((long)lastIndex, out SdafRecord? indexed) || indexed is not SdafIndexRecord)) throw Error(e.FileOffset, "END last_index_offset is invalid.");
        return new SdafEndRecord(e, total, dataTotal, lastIndex);
    }

    private SdafNoteRecord ParseNote(SdafRecordEnvelope e, ReadOnlySpan<byte> h, byte[] payload)
    {
        RequireHeader(e, h, 0, 0);
        if (payload.Length > Limits.MaxUtf8Bytes) throw Error(e.FileOffset, "NOTE exceeds UTF-8 limit.");
        try { return new SdafNoteRecord(e, Bin.Utf8.GetString(payload)); }
        catch (DecoderFallbackException ex) { throw new SdafFormatException($"Invalid NOTE UTF-8 at offset {e.FileOffset}: {ex.Message}"); }
    }

    private List<SdafTlv> ParseTlvs(ReadOnlySpan<byte> body, long recordOffset)
    {
        var result = new List<SdafTlv>(); int offset = 0;
        while (offset < body.Length)
        {
            if (body.Length - offset < 8) throw Error(recordOffset, "TLV header overruns object.");
            ushort tag = Bin.U16(body[offset..]); byte wire = body[offset + 2]; byte flags = body[offset + 3]; uint size = Bin.U32(body[(offset + 4)..]);
            if (flags != 0 || size > body.Length - offset - 8) throw Error(recordOffset, "Invalid TLV flags or value overrun.");
            byte[] value = body.Slice(offset + 8, checked((int)size)).ToArray();
            var type = (SdafWireType)wire;
            SdafTlv.ValidateWireSize(type, value.Length);
            if ((type == SdafWireType.Utf8 && value.Length > Limits.MaxUtf8Bytes)) throw Error(recordOffset, "Schema UTF-8 field exceeds configured limit.");
            if (type == SdafWireType.Utf8) { try { _ = Bin.Utf8.GetCharCount(value); } catch (DecoderFallbackException) { throw Error(recordOffset, "Invalid UTF-8 in schema."); } }
            if (type == SdafWireType.Boolean && value[0] > 1) throw Error(recordOffset, "Invalid boolean TLV.");
            if (type is SdafWireType.RationalUInt64 or SdafWireType.RationalInt64 && Bin.U64(value.AsSpan(8)) == 0) throw Error(recordOffset, "Rational denominator is zero.");
            result.Add(new SdafTlv(tag, type, value)); offset += 8 + checked((int)size);
        }
        return result;
    }

    private static (IReadOnlyList<SdafTransform>, int) ParseTransforms(byte[] payload, int count, long recordOffset)
    {
        var result = new List<SdafTransform>(count); int offset = 0;
        for (int i = 0; i < count; i++)
        {
            if (payload.Length - offset < 8) throw Error(recordOffset, "Transform descriptor overruns payload.");
            ushort id = Bin.U16(payload.AsSpan(offset)); byte version = payload[offset + 2]; byte flags = payload[offset + 3]; uint size = Bin.U32(payload.AsSpan(offset + 4));
            if (flags != 0 || size > payload.Length - offset - 8) throw Error(recordOffset, "Invalid transform descriptor.");
            result.Add(new SdafTransform(id, version, payload.AsSpan(offset + 8, checked((int)size)).ToArray())); offset += 8 + checked((int)size);
        }
        foreach (SdafTransform t in result.Where(t => t.Id is 1 or 2 or 3 or 16))
            if (t.Version != 1 || t.Parameters.Length != 0) throw Error(recordOffset, "Known transform has unsupported version or parameters.");
        return (result, offset);
    }

    private static bool IsSupportedDataTransforms(IReadOnlyList<SdafTransform> transforms)
    {
        if (transforms.Count == 0) return true;
        if (transforms.Count == 1 && transforms[0].Id == 16) return true;
        bool anyTyped = transforms.Any(t => t.Id is 1 or 2 or 3);
        if (anyTyped && (transforms.Count != 4 || transforms[0].Id != 1 || transforms[1].Id != 2 || transforms[2].Id != 3 || transforms[3].Id != 16))
            throw new SdafFormatException("Typed numeric transforms must be exactly [1, 2, 3, 16].");
        return transforms.Count == 4 && transforms[0].Id == 1 && transforms[1].Id == 2 && transforms[2].Id == 3 && transforms[3].Id == 16;
    }

    private static void ValidateTimestampHeader(SdafTimestampMode mode, uint count, long start, ulong pn, ulong pd, uint bytes, long offset)
    {
        ulong expected = checked((ulong)count * 8);
        if (mode == SdafTimestampMode.Periodic && (pd == 0 || bytes != 0)) throw Error(offset, "Periodic DATA has invalid period or timestamp_bytes.");
        if (mode is SdafTimestampMode.Delta or SdafTimestampMode.Explicit && bytes != expected) throw Error(offset, "Timestamp area size mismatch.");
        if (mode == SdafTimestampMode.None && (start != 0 || pn != 0 || pd != 0 || bytes != 0)) throw Error(offset, "Untimestamped DATA has nonzero time fields.");
    }

    private static void ValidateSchema(SdafSchema schema, long offset)
    {
        foreach (var group in schema.Objects.GroupBy(x => x.Kind))
            if (group.Select(x => x.Id).Distinct().Count() != group.Count()) throw Error(offset, $"Duplicate {group.Key} object ID.");
        foreach (SdafSchemaObject obj in schema.Objects)
        {
            ValidateKnownTagTypes(obj, offset);
            foreach (var tags in obj.Tlvs.GroupBy(x => x.Tag))
                if (tags.Count() > 1 && !((obj.Kind == SdafObjectKind.ValueMap && tags.Key == 400) || (obj.Kind == SdafObjectKind.Bitfield && tags.Key == 501)))
                    throw Error(offset, $"Tag {tags.Key} is repeated in object {obj.Id}.");
        }
        foreach (SdafSchemaObject stream in schema.Objects.Where(x => x.Kind == SdafObjectKind.Stream))
        {
            byte kind = stream.Find(106)?.AsUInt8() ?? 1;
            uint channelCount = stream.Find(104)?.AsUInt32() ?? 0;
            int actual = schema.Channels(stream.Id).Count;
            if (kind is < 1 or > 4 || (kind is 2 or 3 && channelCount != 0) || (stream.Find(104) is not null && channelCount != actual)) throw Error(offset, $"Invalid channel count or kind on stream {stream.Id}.");
            if (kind == 3 && string.IsNullOrEmpty(stream.Find(107)?.AsString())) throw Error(offset, $"BLOB stream {stream.Id} requires content_type.");
            uint clock = stream.Find(105)?.AsUInt32() ?? 0;
            if (clock != 0 && schema.Find(SdafObjectKind.Clock, clock) is null) throw Error(offset, $"Stream {stream.Id} references an unknown Clock.");
            if (kind == 4)
            {
                uint source = stream.Find(108)?.AsUInt32() ?? 0, target = stream.Find(109)?.AsUInt32() ?? 0;
                if (source == 0 || target == 0 || schema.Find(SdafObjectKind.Clock, source) is null || schema.Find(SdafObjectKind.Clock, target) is null)
                    throw Error(offset, $"Clock-correlation stream {stream.Id} has invalid Clock references.");
                SdafSchemaObject[] correlationChannels = schema.Channels(stream.Id).ToArray();
                bool channelShape = correlationChannels.All(c => c.Find(201)?.AsUInt8() == (byte)SdafLogicalType.SignedInteger && (c.Find(207)?.AsUInt32() ?? 1) == 1);
                string[] semantics = correlationChannels.Select(c => c.Find(208)?.AsString() ?? "").ToArray();
                if (!channelShape || !semantics.Contains("clock.source_ticks", StringComparer.Ordinal) || !semantics.Contains("clock.target_ticks", StringComparer.Ordinal))
                    throw Error(offset, $"Clock-correlation stream {stream.Id} lacks its required scalar signed channels.");
            }
            foreach (ushort tag in new ushort[] { 110, 111 })
            {
                uint map = stream.Find(tag)?.AsUInt32() ?? 0;
                if (map != 0 && schema.Find(SdafObjectKind.ValueMap, map) is null) throw Error(offset, $"Stream {stream.Id} references an unknown Value map.");
            }
        }
        foreach (SdafSchemaObject channel in schema.Objects.Where(x => x.Kind == SdafObjectKind.Channel))
        {
            uint streamId = channel.Find(200)?.AsUInt32() ?? 0;
            if (schema.Find(SdafObjectKind.Stream, streamId) is null) throw Error(offset, $"Channel {channel.Id} references an unknown stream.");
            _ = SampleCodec.GetChannels(schema, streamId);
            uint map = channel.Find(209)?.AsUInt32() ?? 0, bitfield = channel.Find(210)?.AsUInt32() ?? 0;
            if (map != 0 && bitfield != 0) throw Error(offset, $"Channel {channel.Id} has both value_map and bitfield.");
            if (map != 0 && schema.Find(SdafObjectKind.ValueMap, map) is null) throw Error(offset, $"Channel {channel.Id} references an unknown value map.");
            if (bitfield != 0 && schema.Find(SdafObjectKind.Bitfield, bitfield) is null) throw Error(offset, $"Channel {channel.Id} references an unknown bitfield.");
        }
        foreach (SdafSchemaObject clock in schema.Objects.Where(x => x.Kind == SdafObjectKind.Clock))
            if (clock.Find(300) is null || clock.Find(301) is null || clock.Find(300)!.AsUInt8() > 4) throw Error(offset, $"Clock {clock.Id} lacks a valid time domain or tick period.");
        foreach (SdafSchemaObject map in schema.Objects.Where(x => x.Kind == SdafObjectKind.ValueMap)) ValidateValueMap(map, offset);
        foreach (SdafSchemaObject bitfield in schema.Objects.Where(x => x.Kind == SdafObjectKind.Bitfield)) ValidateBitfield(schema, bitfield, offset);
    }

    private static void ValidateKnownTagTypes(SdafSchemaObject obj, long offset)
    {
        foreach (SdafTlv tlv in obj.Tlvs)
        {
            SdafWireType? expected = tlv.Tag switch
            {
                >= 1 and <= 7 or 10 => SdafWireType.Utf8,
                8 or 9 => SdafWireType.UInt32,
                100 or 103 or 106 or 300 or 303 => SdafWireType.UInt8,
                101 or 102 or 301 => SdafWireType.RationalUInt64,
                104 or 105 or 108 or 109 or 110 or 111 or 200 or 207 or 209 or 210 => SdafWireType.UInt32,
                107 or 204 or 208 or 304 => SdafWireType.Utf8,
                201 => SdafWireType.UInt8,
                202 or 203 or 302 or 500 => SdafWireType.UInt16,
                205 or 206 => SdafWireType.Float64,
                211 or 212 => SdafWireType.RationalInt64,
                400 or 501 => SdafWireType.Bytes,
                _ => null,
            };
            if (expected.HasValue && tlv.WireType != expected.Value) throw Error(offset, $"Tag {tlv.Tag} has wire type {tlv.WireType}; expected {expected.Value}.");
        }
    }

    private static void ValidateValueMap(SdafSchemaObject map, long offset)
    {
        var rawValues = new HashSet<ulong>(); var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (SdafTlv entry in map.Tlvs.Where(x => x.Tag == 400))
        {
            if (entry.Value.Length < 12) throw Error(offset, $"Value map {map.Id} entry is too short.");
            ulong raw = Bin.U64(entry.Value); int nameSize = Bin.U16(entry.Value.AsSpan(8)); int descriptionSize = Bin.U16(entry.Value.AsSpan(10));
            if (entry.Value.Length != 12 + nameSize + descriptionSize || nameSize == 0) throw Error(offset, $"Value map {map.Id} entry has invalid string lengths.");
            string name;
            try { name = Bin.Utf8.GetString(entry.Value, 12, nameSize); _ = Bin.Utf8.GetCharCount(entry.Value, 12 + nameSize, descriptionSize); }
            catch (DecoderFallbackException) { throw Error(offset, $"Value map {map.Id} entry has invalid UTF-8."); }
            if (!rawValues.Add(raw) || !names.Add(name)) throw Error(offset, $"Value map {map.Id} has a duplicate raw value or name.");
        }
    }

    private static void ValidateBitfield(SdafSchema schema, SdafSchemaObject bitfield, long offset)
    {
        int storageBits = bitfield.Find(500)?.AsUInt16() ?? throw Error(offset, $"Bitfield {bitfield.Id} lacks storage_bits.");
        var occupied = new HashSet<int>();
        foreach (SdafTlv member in bitfield.Tlvs.Where(x => x.Tag == 501))
        {
            if (member.Value.Length < 14) throw Error(offset, $"Bitfield {bitfield.Id} member is too short.");
            int lsb = Bin.U16(member.Value); int width = Bin.U16(member.Value.AsSpan(2)); uint mapId = Bin.U32(member.Value.AsSpan(4)); ushort flags = Bin.U16(member.Value.AsSpan(8));
            int nameSize = Bin.U16(member.Value.AsSpan(10)); int descriptionSize = Bin.U16(member.Value.AsSpan(12));
            if (width == 0 || lsb + width > storageBits || (flags & ~1) != 0 || member.Value.Length != 14 + nameSize + descriptionSize) throw Error(offset, $"Bitfield {bitfield.Id} member is invalid.");
            try { _ = Bin.Utf8.GetCharCount(member.Value, 14, nameSize); _ = Bin.Utf8.GetCharCount(member.Value, 14 + nameSize, descriptionSize); }
            catch (DecoderFallbackException) { throw Error(offset, $"Bitfield {bitfield.Id} member has invalid UTF-8."); }
            for (int bit = lsb; bit < lsb + width; bit++) if (!occupied.Add(bit)) throw Error(offset, $"Bitfield {bitfield.Id} members overlap.");
            if (mapId != 0 && schema.Find(SdafObjectKind.ValueMap, mapId) is null) throw Error(offset, $"Bitfield {bitfield.Id} references an unknown Value map.");
        }
        foreach (SdafSchemaObject channel in schema.Objects.Where(x => x.Kind == SdafObjectKind.Channel && x.Find(210)?.AsUInt32() == bitfield.Id))
            if (channel.Find(203)?.AsUInt16() != storageBits) throw Error(offset, $"Bitfield {bitfield.Id} width does not match channel {channel.Id}.");
    }

    private static void ValidateStream(SdafSchema schema, uint streamId, byte kind1, byte kind2, long offset)
    {
        SdafSchemaObject stream = schema.Find(SdafObjectKind.Stream, streamId) ?? throw Error(offset, $"Unknown stream {streamId}.");
        byte kind = stream.Find(106)?.AsUInt8() ?? 1;
        if (kind != kind1 && kind != kind2) throw Error(offset, $"Record type does not match stream kind {kind}.");
    }

    private static void RequireHeader(SdafRecordEnvelope e, ReadOnlySpan<byte> h, int size, ushort allowedFlags)
    {
        if (h.Length != size) throw Error(e.FileOffset, $"Record type 0x{e.RawRecordType:x4} requires header_size {size + 32}.");
        if ((e.Flags & 0xff00 & ~allowedFlags) != 0) throw Error(e.FileOffset, "Reserved type-specific flag is set.");
    }

    private static SdafFormatException Error(long offset, string message) => new($"{message} (record offset {offset}).");
    public void Dispose() { if (!_leaveOpen) _stream.Dispose(); }
}
