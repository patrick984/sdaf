using NUnit.Framework;

namespace Sdaf.Tests;

[TestFixture]
public class SampleLayoutTests
{
    [Test]
    public void ByteAlignedMixedTypesIncludeWideFixedBytes()
    {
        byte[] fixedBytes = Enumerable.Range(0, 16).Select(x => (byte)x).ToArray();
        byte[] payload = new byte[2 + 4 + 1 + fixedBytes.Length];
        payload[0] = 0xff; payload[1] = 0xff; // signed 12-bit -1, sign-extended to 16 storage bits
        BitConverter.GetBytes(1.5f).CopyTo(payload, 2); payload[6] = 1; fixedBytes.CopyTo(payload, 7);
        SdafDataRecord data = RoundTrip(MixedSchema(), new SdafDataWriteOptions
        {
            SchemaId = 10, StreamId = 5, SampleCount = 1, TimestampMode = SdafTimestampMode.None,
            Packing = SdafPacking.ByteAligned, Layout = SdafLayout.Interleaved,
        }, payload);
        IReadOnlyList<SdafSampleValue> values = data.Samples![0].Values;
        Assert.Multiple(() =>
        {
            Assert.That(values[0].RawSigned, Is.EqualTo(-1));
            Assert.That(values[1].NumericValue, Is.EqualTo(1.5));
            Assert.That(values[2].RawUnsigned, Is.EqualTo(1));
            Assert.That(values[3].Bytes, Is.EqualTo(fixedBytes));
        });
    }

    [Test]
    public void PlanarLayoutIsReassembledIntoSamples()
    {
        SdafDataRecord data = RoundTrip(TwoByteChannels(), new SdafDataWriteOptions
        {
            SchemaId = 11, StreamId = 1, SampleCount = 2, TimestampMode = SdafTimestampMode.None,
            Packing = SdafPacking.ByteAligned, Layout = SdafLayout.Planar,
        }, [1, 2, 10, 20]);
        Assert.That(data.Samples!.Select(s => s.Values.Select(v => v.RawUnsigned).ToArray()),
            Is.EqualTo(new[] { new ulong[] { 1, 10 }, new ulong[] { 2, 20 } }));
    }

    [Test]
    public void DeltaTimestampsAreRelativeToStart()
    {
        byte[] payload = new byte[18];
        System.Buffers.Binary.BinaryPrimitives.WriteInt64LittleEndian(payload, 0);
        System.Buffers.Binary.BinaryPrimitives.WriteInt64LittleEndian(payload.AsSpan(8), 7);
        payload[16] = 3; payload[17] = 4;
        SdafDataRecord data = RoundTrip(OneByteChannel(), new SdafDataWriteOptions
        {
            SchemaId = 12, StreamId = 1, SampleCount = 2, StartTimeTicks = 100,
            TimestampMode = SdafTimestampMode.Delta, TimestampBytes = 16,
            Packing = SdafPacking.ByteAligned,
        }, payload);
        Assert.That(data.Samples!.Select(x => x.TimeTicks), Is.EqualTo(new long?[] { 100, 107 }));
    }

    [Test]
    public void ExplicitTimestampsAreAbsolute()
    {
        byte[] payload = new byte[18];
        System.Buffers.Binary.BinaryPrimitives.WriteInt64LittleEndian(payload, -5);
        System.Buffers.Binary.BinaryPrimitives.WriteInt64LittleEndian(payload.AsSpan(8), 25);
        payload[16] = 3; payload[17] = 4;
        SdafDataRecord data = RoundTrip(OneByteChannel(), new SdafDataWriteOptions
        {
            SchemaId = 12, StreamId = 1, SampleCount = 2,
            TimestampMode = SdafTimestampMode.Explicit, TimestampBytes = 16,
            Packing = SdafPacking.ByteAligned,
        }, payload);
        Assert.That(data.Samples!.Select(x => x.TimeTicks), Is.EqualTo(new long?[] { -5, 25 }));
    }

    [Test]
    public void NonzeroDensePaddingIsRejected()
    {
        using var stream = new MemoryStream();
        using (var writer = new SdafWriter(stream, leaveOpen: true))
        {
            writer.WriteSchema(new SdafSchema(13, 1,
            [
                Stream(1, 1),
                Channel(1, 1, "flag", SdafLogicalType.Boolean, 1, 1),
            ]));
            writer.WriteData(new SdafDataWriteOptions { SchemaId = 13, StreamId = 1, SampleCount = 1, TimestampMode = SdafTimestampMode.None }, [0xfe]);
        }
        stream.Position = 0; using var reader = new SdafReader(stream);
        Assert.Throws<SdafFormatException>(() => reader.ReadRecords().ToList());
    }

    private static SdafDataRecord RoundTrip(SdafSchema schema, SdafDataWriteOptions options, byte[] payload)
    {
        using var stream = new MemoryStream();
        using (var writer = new SdafWriter(stream, leaveOpen: true)) { writer.WriteSchema(schema); writer.WriteData(options, payload); }
        stream.Position = 0; using var reader = new SdafReader(stream);
        return reader.ReadRecords().OfType<SdafDataRecord>().Single();
    }

    private static SdafSchema MixedSchema() => new(10, 1,
    [
        Stream(5, 4),
        Channel(1, 5, "signed", SdafLogicalType.SignedInteger, 12, 16),
        Channel(2, 5, "float", SdafLogicalType.Float, 32, 32),
        Channel(3, 5, "flag", SdafLogicalType.Boolean, 1, 1),
        new(SdafObjectKind.Channel, 4, [SdafTlv.Utf8(1, "bytes"), SdafTlv.UInt32(200, 5), SdafTlv.UInt8(201, (byte)SdafLogicalType.FixedBytes), SdafTlv.UInt16(203, 128)]),
    ]);

    private static SdafSchema TwoByteChannels() => new(11, 1, [Stream(1, 2), Channel(1, 1, "a", SdafLogicalType.UnsignedInteger, 8, 8), Channel(2, 1, "b", SdafLogicalType.UnsignedInteger, 8, 8)]);
    private static SdafSchema OneByteChannel() => new(12, 1, [Stream(1, 1), Channel(1, 1, "a", SdafLogicalType.UnsignedInteger, 8, 8)]);
    private static SdafSchemaObject Stream(uint id, uint count) => new(SdafObjectKind.Stream, id, [SdafTlv.UInt32(104, count)]);
    private static SdafSchemaObject Channel(uint id, uint stream, string name, SdafLogicalType type, ushort logical, ushort storage) => new(SdafObjectKind.Channel, id,
        [SdafTlv.Utf8(1, name), SdafTlv.UInt32(200, stream), SdafTlv.UInt8(201, (byte)type), SdafTlv.UInt16(202, logical), SdafTlv.UInt16(203, storage)]);
}
