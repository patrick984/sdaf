namespace Sdaf;

public sealed class SdafWriter : IDisposable
{
    private readonly Stream _stream;
    private readonly bool _leaveOpen;
    private readonly Dictionary<(uint, uint), SdafSchema> _schemas = new();
    private readonly Dictionary<(uint, uint), byte[]> _schemaPayloads = new();
    private readonly Dictionary<(uint SchemaId, uint StreamId), ulong> _lastBlobIndexes = new();
    private uint _sequence;
    private ulong _recordCount;
    private ulong _dataRecordCount;
    private bool _disposed;

    public SdafFileHeader Header { get; }

    public SdafWriter(
        Stream stream,
        long createdUnixNanoseconds = long.MinValue,
        ReadOnlySpan<byte> fileUuid = default,
        bool leaveOpen = false
    )
    {
        _stream = stream ?? throw new ArgumentNullException(nameof(stream));
        if (!stream.CanWrite)
            throw new ArgumentException("Stream must be writable.", nameof(stream));
        _leaveOpen = leaveOpen;
        byte[] uuid = fileUuid.IsEmpty ? new byte[16] : fileUuid.ToArray();
        if (uuid.Length != 16)
            throw new ArgumentException(
                "File UUID must contain exactly 16 network-order bytes.",
                nameof(fileUuid)
            );
        Header = new SdafFileHeader(1, 0, createdUnixNanoseconds, uuid);
        WriteFileHeader();
    }

    public void WriteSchema(SdafSchema schema, bool payloadCrcInTrailer = false)
    {
        ArgumentNullException.ThrowIfNull(schema);
        byte[] payload = SerializeSchema(schema);
        if (
            _schemaPayloads.TryGetValue((schema.Id, schema.Revision), out byte[]? prior)
            && !prior.AsSpan().SequenceEqual(payload)
        )
            throw new ArgumentException(
                "A schema revision cannot be redefined with different bytes.",
                nameof(schema)
            );
        byte[] h = new byte[16];
        Bin.U32(h, schema.Id);
        Bin.U32(h.AsSpan(4), schema.Revision);
        Bin.U32(h.AsSpan(8), checked((uint)schema.Objects.Count));
        WriteRecord((ushort)SdafRecordType.Schema, 0, h, payload, payloadCrcInTrailer);
        _schemas[(schema.Id, schema.Revision)] = schema;
        _schemaPayloads[(schema.Id, schema.Revision)] = payload;
    }

    public void WriteData(SdafDataWriteOptions options, ReadOnlySpan<byte> canonicalDecodedPayload)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (
            !_schemas.TryGetValue(
                (options.SchemaId, options.SchemaRevision),
                out SdafSchema? schema
            )
        )
            throw new InvalidOperationException("Write the referenced schema before DATA.");
        RequireStreamKind(schema, options.StreamId, 1, 4);
        if (
            options.SchemaId == 0
            || options.SchemaRevision == 0
            || options.StreamId == 0
            || options.Layout is < SdafLayout.Interleaved or > SdafLayout.Planar
            || options.Packing is < SdafPacking.Lsb0Dense or > SdafPacking.ByteAligned
            || options.TimestampMode is < SdafTimestampMode.Periodic or > SdafTimestampMode.None
        )
            throw new ArgumentException(
                "DATA options contain an invalid ID or enum value.",
                nameof(options)
            );
        ChannelInfo[] channels = SampleCodec.GetChannels(schema, options.StreamId);
        if (
            (schema.Find(SdafObjectKind.Stream, options.StreamId)?.Find(106)?.AsUInt8() ?? 1) == 4
            && options.TimestampMode != SdafTimestampMode.None
        )
            throw new ArgumentException(
                "Clock-correlation DATA must use timestamp mode 4.",
                nameof(options)
            );
        ulong expected = SampleCodec.ExpectedCanonicalBytes(
            channels,
            options.SampleCount,
            options.TimestampBytes,
            options.Packing
        );
        if ((ulong)canonicalDecodedPayload.Length != expected)
            throw new ArgumentException(
                $"Canonical payload is {canonicalDecodedPayload.Length} bytes; schema requires {expected}.",
                nameof(canonicalDecodedPayload)
            );
        ValidateDataTime(options);
        var dh = new SdafDataHeader(
            options.SampleCount,
            options.FirstSampleIndex,
            options.StartTimeTicks,
            options.PeriodNumerator,
            options.PeriodDenominator,
            options.TimestampMode,
            options.Layout,
            options.Packing,
            options.TimestampBytes,
            expected
        );
        byte[] encoded;
        ushort[] ids;
        switch (options.Compression)
        {
            case SdafCompression.None:
                encoded = canonicalDecodedPayload.ToArray();
                ids = [];
                break;
            case SdafCompression.Zstandard:
                encoded = Zstandard.Compress(canonicalDecodedPayload);
                ids = [16];
                break;
            case SdafCompression.CompressedNumeric:
                encoded = SampleCodec.EncodeCompressedNumeric(
                    canonicalDecodedPayload,
                    channels,
                    dh
                );
                ids = [1, 2, 3, 16];
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(options.Compression));
        }
        byte[] payload = AddDescriptors(ids, encoded);
        byte[] h = new byte[64];
        Bin.U32(h, options.SchemaId);
        Bin.U32(h.AsSpan(4), options.SchemaRevision);
        Bin.U32(h.AsSpan(8), options.StreamId);
        Bin.U32(h.AsSpan(12), options.SampleCount);
        Bin.U64(h.AsSpan(16), options.FirstSampleIndex);
        Bin.I64(h.AsSpan(24), options.StartTimeTicks);
        Bin.U64(h.AsSpan(32), options.PeriodNumerator);
        Bin.U64(h.AsSpan(40), options.PeriodDenominator);
        h[48] = (byte)options.TimestampMode;
        h[49] = (byte)options.Layout;
        h[50] = (byte)options.Packing;
        h[51] = checked((byte)ids.Length);
        Bin.U32(h.AsSpan(52), options.TimestampBytes);
        Bin.U64(h.AsSpan(56), expected);
        WriteRecord((ushort)SdafRecordType.Data, 0, h, payload, options.PayloadCrcInTrailer);
        _dataRecordCount++;
    }

    public void WriteText(SdafTextWriteOptions options, string message)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(message);
        if ((byte)options.Severity > 6)
            throw new ArgumentOutOfRangeException(nameof(options.Severity));
        if (options.SchemaId == 0)
        {
            if (
                options.SchemaRevision != 0
                || options.StreamId != 0
                || options.SourceId.HasValue
                || options.EventCode.HasValue
            )
                throw new ArgumentException(
                    "Global TEXT must have zero schema/stream references and no symbolic fields."
                );
        }
        else if (
            options.SchemaRevision == 0
            || options.StreamId == 0
            || !_schemas.ContainsKey((options.SchemaId, options.SchemaRevision))
        )
            throw new ArgumentException("Stream TEXT must reference a schema already written.");
        else
            RequireStreamKind(
                _schemas[(options.SchemaId, options.SchemaRevision)],
                options.StreamId,
                2,
                2
            );
        ushort flags = 0;
        if (options.SourceId.HasValue)
            flags |= 0x0100;
        if (options.EventCode.HasValue)
            flags |= 0x0200;
        byte[] h = new byte[32];
        Bin.U32(h, options.SchemaId);
        Bin.U32(h.AsSpan(4), options.StreamId);
        Bin.I64(h.AsSpan(8), options.TimeTicks);
        h[16] = (byte)options.Severity;
        h[17] = 1;
        Bin.U32(h.AsSpan(20), options.SourceId ?? 0);
        Bin.I32(h.AsSpan(24), options.EventCode ?? 0);
        Bin.U32(h.AsSpan(28), options.SchemaRevision);
        WriteRecord(
            (ushort)SdafRecordType.Text,
            flags,
            h,
            Bin.Utf8.GetBytes(message),
            options.PayloadCrcInTrailer
        );
    }

    public void WriteBlob(SdafBlobWriteOptions options, ReadOnlySpan<byte> decodedPayload)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (
            !_schemas.TryGetValue(
                (options.SchemaId, options.SchemaRevision),
                out SdafSchema? schema
            )
        )
            throw new InvalidOperationException("Write the referenced schema before BLOB.");
        RequireStreamKind(schema, options.StreamId, 3, 3);
        if (
            _lastBlobIndexes.TryGetValue((options.SchemaId, options.StreamId), out ulong last)
            && options.ItemIndex <= last
        )
            throw new ArgumentException(
                "BLOB item_index must be strictly increasing within its stream.",
                nameof(options)
            );
        if (options.Compression == SdafCompression.CompressedNumeric)
            throw new ArgumentException("BLOB does not support typed numeric transforms.");
        ushort[] ids = options.Compression == SdafCompression.Zstandard ? [16] : [];
        byte[] encoded =
            options.Compression == SdafCompression.Zstandard
                ? Zstandard.Compress(decodedPayload)
                : decodedPayload.ToArray();
        byte[] h = new byte[48];
        Bin.U32(h, options.SchemaId);
        Bin.U32(h.AsSpan(4), options.SchemaRevision);
        Bin.U32(h.AsSpan(8), options.StreamId);
        Bin.U64(h.AsSpan(16), options.ItemIndex);
        Bin.I64(h.AsSpan(24), options.TimeTicks);
        Bin.U64(h.AsSpan(32), (ulong)decodedPayload.Length);
        h[40] = (byte)ids.Length;
        WriteRecord(
            (ushort)SdafRecordType.Blob,
            0,
            h,
            AddDescriptors(ids, encoded),
            options.PayloadCrcInTrailer
        );
        _lastBlobIndexes[(options.SchemaId, options.StreamId)] = options.ItemIndex;
    }

    public void WriteNote(string message, bool payloadCrcInTrailer = false) =>
        WriteRecord(
            (ushort)SdafRecordType.Note,
            0,
            [],
            Bin.Utf8.GetBytes(message),
            payloadCrcInTrailer
        );

    public void WriteIndex(IEnumerable<SdafIndexEntry> entries, bool payloadCrcInTrailer = false)
    {
        SdafIndexEntry[] values = entries.ToArray();
        byte[] h = new byte[16];
        Bin.U32(h, checked((uint)values.Length));
        byte[] payload = new byte[checked(values.Length * 48)];
        for (int i = 0; i < values.Length; i++)
        {
            Span<byte> p = payload.AsSpan(i * 48, 48);
            SdafIndexEntry v = values[i];
            Bin.U64(p, v.RecordOffset);
            Bin.U32(p[8..], v.Sequence);
            Bin.U32(p[12..], v.StreamId);
            Bin.U64(p[16..], v.FirstSampleIndex);
            Bin.U32(p[24..], v.SampleCount);
            Bin.I64(p[32..], v.FirstTimeTicks);
            Bin.I64(p[40..], v.LastTimeTicks);
        }
        WriteRecord((ushort)SdafRecordType.Index, 0, h, payload, payloadCrcInTrailer);
    }

    public void WriteEnd(ulong lastIndexOffset = 0)
    {
        byte[] h = new byte[32];
        // Include the END record itself in total_record_count.
        Bin.U64(h, _recordCount + 1);
        Bin.U64(h.AsSpan(8), _dataRecordCount);
        Bin.U64(h.AsSpan(16), lastIndexOffset);
        WriteRecord((ushort)SdafRecordType.End, 0, h, [], false);
    }

    public void WritePrivateRecord(
        ushort recordType,
        ReadOnlySpan<byte> typeHeader,
        ReadOnlySpan<byte> payload,
        bool payloadCrcInTrailer = false
    )
    {
        if (recordType < 0x8000)
            throw new ArgumentOutOfRangeException(
                nameof(recordType),
                "Private record types are 0x8000 through 0xffff."
            );
        WriteRecord(recordType, 0, typeHeader, payload, payloadCrcInTrailer);
    }

    private void WriteFileHeader()
    {
        byte[] h = new byte[64];
        "SDAF\r\n\x1a\n"u8.CopyTo(h);
        h[8] = 1;
        h[9] = 0;
        Bin.U16(h.AsSpan(10), 64);
        Bin.U32(h.AsSpan(12), 0x12345678);
        Bin.I64(h.AsSpan(24), Header.CreatedUnixNanoseconds);
        Header.FileUuid.CopyTo(h, 32);
        Bin.U64(h.AsSpan(48), 64);
        Bin.U32(h.AsSpan(56), SdafCrc32C.Compute(h));
        _stream.Write(h);
    }

    private void WriteRecord(
        ushort type,
        ushort typeFlags,
        ReadOnlySpan<byte> typeHeader,
        ReadOnlySpan<byte> payload,
        bool trailerMode
    )
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        int headerSize = checked(32 + typeHeader.Length);
        if (headerSize > ushort.MaxValue)
            throw new ArgumentOutOfRangeException(nameof(typeHeader));
        byte[] h = new byte[headerSize];
        "SDRC"u8.CopyTo(h);
        Bin.U16(h.AsSpan(4), type);
        Bin.U16(h.AsSpan(6), (ushort)(typeFlags | (trailerMode ? 1 : 0)));
        Bin.U16(h.AsSpan(8), (ushort)headerSize);
        h[10] = 1;
        Bin.U32(h.AsSpan(12), _sequence);
        Bin.U64(h.AsSpan(16), (ulong)payload.Length);
        uint payloadCrc = SdafCrc32C.Compute(payload);
        if (!trailerMode)
            Bin.U32(h.AsSpan(28), payloadCrc);
        typeHeader.CopyTo(h.AsSpan(32));
        Bin.U32(h.AsSpan(24), SdafCrc32C.Compute(h));
        _stream.Write(h);
        _stream.Write(payload);
        if (trailerMode)
        {
            Span<byte> t = stackalloc byte[16];
            "SDCT"u8.CopyTo(t);
            Bin.U16(t[4..], 16);
            t[6] = 1;
            Bin.U32(t[8..], _sequence);
            Bin.U32(t[12..], payloadCrc);
            _stream.Write(t);
        }
        _sequence++;
        _recordCount++;
    }

    private static byte[] SerializeSchema(SdafSchema schema)
    {
        using var stream = new MemoryStream();
        foreach (SdafSchemaObject obj in schema.Objects)
        {
            using var body = new MemoryStream();
            foreach (SdafTlv tlv in obj.Tlvs)
            {
                byte[] th = new byte[8];
                Bin.U16(th, tlv.Tag);
                th[2] = (byte)tlv.WireType;
                Bin.U32(th.AsSpan(4), checked((uint)tlv.Value.Length));
                body.Write(th);
                body.Write(tlv.Value);
            }
            byte[] oh = new byte[12];
            oh[0] = (byte)obj.Kind;
            Bin.U16(oh.AsSpan(2), 12);
            Bin.U32(oh.AsSpan(4), checked((uint)(12 + body.Length)));
            Bin.U32(oh.AsSpan(8), obj.Id);
            stream.Write(oh);
            body.Position = 0;
            body.CopyTo(stream);
        }
        return stream.ToArray();
    }

    private static byte[] AddDescriptors(ushort[] ids, byte[] encoded)
    {
        byte[] result = new byte[checked(ids.Length * 8 + encoded.Length)];
        for (int i = 0; i < ids.Length; i++)
        {
            Bin.U16(result.AsSpan(i * 8), ids[i]);
            result[i * 8 + 2] = 1;
        }
        encoded.CopyTo(result, ids.Length * 8);
        return result;
    }

    private static void ValidateDataTime(SdafDataWriteOptions o)
    {
        if (
            o.TimestampMode == SdafTimestampMode.Periodic
            && (o.PeriodDenominator == 0 || o.TimestampBytes != 0)
        )
            throw new ArgumentException(
                "Periodic DATA requires a nonzero period denominator and no timestamp area."
            );
        if (
            o.TimestampMode is SdafTimestampMode.Delta or SdafTimestampMode.Explicit
            && o.TimestampBytes != checked(o.SampleCount * 8)
        )
            throw new ArgumentException("Delta/explicit timestamp area must be sample_count * 8.");
        if (
            o.TimestampMode == SdafTimestampMode.None
            && (
                o.StartTimeTicks != 0
                || o.PeriodNumerator != 0
                || o.PeriodDenominator != 0
                || o.TimestampBytes != 0
            )
        )
            throw new ArgumentException("Untimestamped DATA requires zero time fields.");
    }

    private static void RequireStreamKind(
        SdafSchema schema,
        uint streamId,
        byte firstKind,
        byte secondKind
    )
    {
        SdafSchemaObject stream =
            schema.Find(SdafObjectKind.Stream, streamId)
            ?? throw new ArgumentException($"Schema does not define stream {streamId}.");
        byte kind = stream.Find(106)?.AsUInt8() ?? 1;
        if (kind != firstKind && kind != secondKind)
            throw new ArgumentException($"Stream {streamId} has incompatible stream_kind {kind}.");
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        if (!_leaveOpen)
            _stream.Dispose();
    }
}
