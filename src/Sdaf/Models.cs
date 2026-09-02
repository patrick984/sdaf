using System.Collections.ObjectModel;

namespace Sdaf;

public enum SdafRecordType : ushort
{
    Schema = 0x0001,
    Data = 0x0002,
    Text = 0x0003,
    Index = 0x0004,
    End = 0x0005,
    Blob = 0x0006,
    Note = 0x7fff,
}

public enum SdafObjectKind : byte
{
    FileMetadata = 1,
    Stream = 2,
    Channel = 3,
    Clock = 4,
    ValueMap = 5,
    Bitfield = 6,
}

public enum SdafWireType : byte
{
    UInt8 = 1,
    UInt16 = 2,
    UInt32 = 3,
    UInt64 = 4,
    Int64 = 5,
    Float64 = 6,
    Utf8 = 7,
    Bytes = 8,
    RationalUInt64 = 9,
    Boolean = 10,
    RationalInt64 = 11,
}

public enum SdafLogicalType : byte
{
    UnsignedInteger = 1,
    SignedInteger = 2,
    Float = 3,
    Boolean = 4,
    FixedBytes = 5,
}

public enum SdafTimestampMode : byte { Periodic = 1, Delta = 2, Explicit = 3, None = 4 }
public enum SdafLayout : byte { Interleaved = 1, Planar = 2 }
public enum SdafPacking : byte { Lsb0Dense = 1, ByteAligned = 2 }
public enum SdafTextSeverity : byte { Unspecified = 0, Trace = 1, Debug = 2, Info = 3, Warning = 4, Error = 5, Fatal = 6 }
public enum SdafCompression { None, Zstandard, CompressedNumeric }

public sealed class SdafFormatException : IOException
{
    public SdafFormatException(string message) : base(message) { }
}

public sealed record SdafLimits
{
    public int MaxHeaderSize { get; init; } = 64 * 1024;
    public ulong MaxPayloadSize { get; init; } = 1024UL * 1024 * 1024;
    public ulong MaxDecodedSize { get; init; } = 4UL * 1024 * 1024 * 1024;
    public uint MaxSchemaObjects { get; init; } = 65_535;
    public uint MaxChannelsPerStream { get; init; } = 4_096;
    public int MaxUtf8Bytes { get; init; } = 16 * 1024 * 1024;
}

public sealed record SdafFileHeader(
    byte Major,
    byte Minor,
    long CreatedUnixNanoseconds,
    byte[] FileUuid,
    ulong FirstRecordOffset = 64);

public sealed record SdafRecordEnvelope(
    SdafRecordType RecordType,
    ushort RawRecordType,
    ushort Flags,
    ushort HeaderSize,
    byte RecordVersion,
    uint Sequence,
    ulong PayloadSize,
    long FileOffset,
    bool PayloadCrcInTrailer);

public abstract record SdafRecord(SdafRecordEnvelope Envelope);

public sealed record SdafUnknownRecord(SdafRecordEnvelope Envelope, byte[] TypeHeader, byte[] Payload)
    : SdafRecord(Envelope);

public sealed record SdafTransform(ushort Id, byte Version, byte[] Parameters);

public sealed record SdafSchemaRecord(SdafRecordEnvelope Envelope, SdafSchema Schema)
    : SdafRecord(Envelope);

public sealed record SdafDataRecord(
    SdafRecordEnvelope Envelope,
    uint SchemaId,
    uint SchemaRevision,
    uint StreamId,
    uint SampleCount,
    ulong FirstSampleIndex,
    long StartTimeTicks,
    ulong PeriodNumerator,
    ulong PeriodDenominator,
    SdafTimestampMode TimestampMode,
    SdafLayout Layout,
    SdafPacking Packing,
    uint TimestampBytes,
    ulong DecodedSampleBytes,
    IReadOnlyList<SdafTransform> Transforms,
    byte[] StoredPayload,
    byte[]? DecodedPayload,
    IReadOnlyList<SdafSample>? Samples)
    : SdafRecord(Envelope);

public sealed record SdafTextRecord(
    SdafRecordEnvelope Envelope,
    uint SchemaId,
    uint SchemaRevision,
    uint StreamId,
    long TimeTicks,
    SdafTextSeverity Severity,
    uint? SourceId,
    int? EventCode,
    string Message)
    : SdafRecord(Envelope);

public sealed record SdafBlobRecord(
    SdafRecordEnvelope Envelope,
    uint SchemaId,
    uint SchemaRevision,
    uint StreamId,
    ulong ItemIndex,
    long TimeTicks,
    ulong DecodedBytes,
    IReadOnlyList<SdafTransform> Transforms,
    byte[] StoredPayload,
    byte[]? DecodedPayload)
    : SdafRecord(Envelope);

public sealed record SdafIndexEntry(
    ulong RecordOffset, uint Sequence, uint StreamId, ulong FirstSampleIndex,
    uint SampleCount, long FirstTimeTicks, long LastTimeTicks);

public sealed record SdafIndexRecord(SdafRecordEnvelope Envelope, IReadOnlyList<SdafIndexEntry> Entries)
    : SdafRecord(Envelope);

public sealed record SdafEndRecord(
    SdafRecordEnvelope Envelope, ulong TotalRecordCount, ulong TotalDataRecordCount, ulong LastIndexOffset)
    : SdafRecord(Envelope);

public sealed record SdafNoteRecord(SdafRecordEnvelope Envelope, string Message) : SdafRecord(Envelope);

public sealed record SdafSample(ulong Index, long? TimeTicks, IReadOnlyList<SdafSampleValue> Values);

public sealed record SdafSampleValue(
    uint ChannelId,
    string Name,
    uint ElementIndex,
    SdafLogicalType LogicalType,
    ulong RawUnsigned,
    long RawSigned,
    double NumericValue,
    double PhysicalValue,
    byte[]? Bytes,
    string? Unit);

public sealed record SdafDataWriteOptions
{
    public required uint SchemaId { get; init; }
    public uint SchemaRevision { get; init; } = 1;
    public required uint StreamId { get; init; }
    public required uint SampleCount { get; init; }
    public ulong FirstSampleIndex { get; init; }
    public long StartTimeTicks { get; init; }
    public ulong PeriodNumerator { get; init; }
    public ulong PeriodDenominator { get; init; }
    public SdafTimestampMode TimestampMode { get; init; } = SdafTimestampMode.None;
    public SdafLayout Layout { get; init; } = SdafLayout.Interleaved;
    public SdafPacking Packing { get; init; } = SdafPacking.Lsb0Dense;
    public uint TimestampBytes { get; init; }
    public SdafCompression Compression { get; init; }
    public bool PayloadCrcInTrailer { get; init; }
}

public sealed record SdafTextWriteOptions
{
    public uint SchemaId { get; init; }
    public uint SchemaRevision { get; init; }
    public uint StreamId { get; init; }
    public long TimeTicks { get; init; }
    public SdafTextSeverity Severity { get; init; }
    public uint? SourceId { get; init; }
    public int? EventCode { get; init; }
    public bool PayloadCrcInTrailer { get; init; }
}

public sealed record SdafBlobWriteOptions
{
    public required uint SchemaId { get; init; }
    public uint SchemaRevision { get; init; } = 1;
    public required uint StreamId { get; init; }
    public required ulong ItemIndex { get; init; }
    public long TimeTicks { get; init; } = long.MinValue;
    public SdafCompression Compression { get; init; }
    public bool PayloadCrcInTrailer { get; init; }
}
