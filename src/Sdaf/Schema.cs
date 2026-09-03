using System.Buffers.Binary;

namespace Sdaf;

public sealed class SdafTlv
{
    public ushort Tag { get; }
    public SdafWireType WireType { get; }
    public byte[] Value { get; }

    public SdafTlv(ushort tag, SdafWireType wireType, byte[] value)
    {
        Tag = tag;
        WireType = wireType;
        Value = value ?? throw new ArgumentNullException(nameof(value));
        ValidateWireSize(wireType, value.Length);
    }

    public static SdafTlv UInt8(ushort tag, byte value) => new(tag, SdafWireType.UInt8, [value]);

    public static SdafTlv UInt16(ushort tag, ushort value)
    {
        byte[] b = new byte[2];
        Bin.U16(b, value);
        return new(tag, SdafWireType.UInt16, b);
    }

    public static SdafTlv UInt32(ushort tag, uint value)
    {
        byte[] b = new byte[4];
        Bin.U32(b, value);
        return new(tag, SdafWireType.UInt32, b);
    }

    public static SdafTlv UInt64(ushort tag, ulong value)
    {
        byte[] b = new byte[8];
        Bin.U64(b, value);
        return new(tag, SdafWireType.UInt64, b);
    }

    public static SdafTlv Int64(ushort tag, long value)
    {
        byte[] b = new byte[8];
        Bin.I64(b, value);
        return new(tag, SdafWireType.Int64, b);
    }

    public static SdafTlv Float64(ushort tag, double value) =>
        Int64(tag, BitConverter.DoubleToInt64Bits(value)).WithWireType(SdafWireType.Float64);

    public static SdafTlv Utf8(ushort tag, string value) =>
        new(tag, SdafWireType.Utf8, Bin.Utf8.GetBytes(value));

    public static SdafTlv Bytes(ushort tag, byte[] value) => new(tag, SdafWireType.Bytes, value);

    public static SdafTlv Boolean(ushort tag, bool value) =>
        new(tag, SdafWireType.Boolean, [value ? (byte)1 : (byte)0]);

    public static SdafTlv Rational(ushort tag, ulong numerator, ulong denominator)
    {
        if (denominator == 0)
            throw new ArgumentOutOfRangeException(nameof(denominator));
        byte[] b = new byte[16];
        Bin.U64(b, numerator);
        Bin.U64(b.AsSpan(8), denominator);
        return new(tag, SdafWireType.RationalUInt64, b);
    }

    public static SdafTlv Rational(ushort tag, long numerator, ulong denominator)
    {
        if (denominator == 0)
            throw new ArgumentOutOfRangeException(nameof(denominator));
        byte[] b = new byte[16];
        Bin.I64(b, numerator);
        Bin.U64(b.AsSpan(8), denominator);
        return new(tag, SdafWireType.RationalInt64, b);
    }

    private SdafTlv WithWireType(SdafWireType type) => new(Tag, type, Value);

    public byte AsUInt8() => Value[0];

    public ushort AsUInt16() => Bin.U16(Value);

    public uint AsUInt32() => Bin.U32(Value);

    public ulong AsUInt64() => Bin.U64(Value);

    public long AsInt64() => Bin.I64(Value);

    public double AsFloat64() => Bin.F64(Value);

    public string AsString() => Bin.Utf8.GetString(Value);

    public (ulong Numerator, ulong Denominator) AsRationalUInt64() =>
        (Bin.U64(Value), Bin.U64(Value.AsSpan(8)));

    public (long Numerator, ulong Denominator) AsRationalInt64() =>
        (Bin.I64(Value), Bin.U64(Value.AsSpan(8)));

    internal static void ValidateWireSize(SdafWireType type, int size)
    {
        int expected = type switch
        {
            SdafWireType.UInt8 or SdafWireType.Boolean => 1,
            SdafWireType.UInt16 => 2,
            SdafWireType.UInt32 => 4,
            SdafWireType.UInt64 or SdafWireType.Int64 or SdafWireType.Float64 => 8,
            SdafWireType.RationalUInt64 or SdafWireType.RationalInt64 => 16,
            _ => -1,
        };
        if (expected >= 0 && size != expected)
            throw new SdafFormatException(
                $"Wire type {type} requires {expected} bytes, not {size}."
            );
    }
}

public sealed class SdafSchemaObject
{
    public SdafObjectKind Kind { get; }
    public uint Id { get; }
    public IReadOnlyList<SdafTlv> Tlvs { get; }

    public SdafSchemaObject(SdafObjectKind kind, uint id, IEnumerable<SdafTlv> tlvs)
    {
        if (id == 0)
            throw new ArgumentOutOfRangeException(nameof(id));
        Kind = kind;
        Id = id;
        Tlvs = tlvs.ToArray();
    }

    public SdafTlv? Find(ushort tag) => Tlvs.FirstOrDefault(x => x.Tag == tag);

    public string Name => Find(1)?.AsString() ?? $"{Kind.ToString().ToLowerInvariant()}_{Id}";
}

public sealed class SdafSchema
{
    public uint Id { get; }
    public uint Revision { get; }
    public IReadOnlyList<SdafSchemaObject> Objects { get; }

    public SdafSchema(uint id, uint revision, IEnumerable<SdafSchemaObject> objects)
    {
        if (id == 0 || revision == 0)
            throw new ArgumentOutOfRangeException(id == 0 ? nameof(id) : nameof(revision));
        Id = id;
        Revision = revision;
        Objects = objects.ToArray();
    }

    public SdafSchemaObject? Find(SdafObjectKind kind, uint id) =>
        Objects.FirstOrDefault(x => x.Kind == kind && x.Id == id);

    public IReadOnlyList<SdafSchemaObject> Channels(uint streamId) =>
        Objects
            .Where(x => x.Kind == SdafObjectKind.Channel && x.Find(200)?.AsUInt32() == streamId)
            .OrderBy(x => x.Id)
            .ToArray();
}
