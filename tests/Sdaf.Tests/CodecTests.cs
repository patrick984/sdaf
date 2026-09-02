using NUnit.Framework;

namespace Sdaf.Tests;

[TestFixture]
public class CodecTests
{
    [Test]
    public void Crc32CKnownVectors()
    {
        Assert.Multiple(() =>
        {
            Assert.That(SdafCrc32C.Compute([]), Is.Zero);
            Assert.That(SdafCrc32C.Compute("123456789"u8), Is.EqualTo(0xe3069283u));
        });
    }

    [TestCase(SdafCompression.None, false)]
    [TestCase(SdafCompression.None, true)]
    [TestCase(SdafCompression.Zstandard, false)]
    [TestCase(SdafCompression.CompressedNumeric, false)]
    public void WriterReaderRoundTripData(SdafCompression compression, bool trailer)
    {
        using var stream = new MemoryStream();
        using (var writer = new SdafWriter(stream, 123, Enumerable.Range(0, 16).Select(x => (byte)x).ToArray(), true))
        {
            writer.WriteSchema(TwoChannelSchema());
            writer.WriteData(new SdafDataWriteOptions
            {
                SchemaId = 1, StreamId = 1, SampleCount = 3, StartTimeTicks = 100,
                PeriodNumerator = 2, PeriodDenominator = 1, TimestampMode = SdafTimestampMode.Periodic,
                Layout = SdafLayout.Interleaved, Packing = SdafPacking.Lsb0Dense,
                Compression = compression, PayloadCrcInTrailer = trailer,
            }, Convert.FromHexString("00F0FF236145BC9A78"));
            writer.WriteEnd();
        }
        stream.Position = 0;
        using var reader = new SdafReader(stream, leaveOpen: true);
        List<SdafRecord> records = reader.ReadRecords().ToList();
        SdafDataRecord data = records.OfType<SdafDataRecord>().Single();
        IReadOnlyList<SdafSample> samples = data.Samples!;
        Assert.Multiple(() =>
        {
            Assert.That(samples.SelectMany(x => x.Values).Select(x => x.RawUnsigned), Is.EqualTo(new ulong[] { 0, 4095, 291, 1110, 2748, 1929 }));
            Assert.That(samples.Select(x => x.TimeTicks), Is.EqualTo(new long?[] { 100, 102, 104 }));
            Assert.That(data.Envelope.PayloadCrcInTrailer, Is.EqualTo(trailer));
            Assert.That(records[^1], Is.TypeOf<SdafEndRecord>());
        });
    }

    [Test]
    public void RoundTripsTextBlobNoteIndexAndPrivateRecord()
    {
        using var stream = new MemoryStream();
        using (var writer = new SdafWriter(stream, leaveOpen: true))
        {
            writer.WriteSchema(BlobAndTextSchema());
            writer.WriteText(new SdafTextWriteOptions { SchemaId = 2, SchemaRevision = 1, StreamId = 10, TimeTicks = 9, Severity = SdafTextSeverity.Warning, SourceId = 3, EventCode = -7 }, "µ warning");
            ulong blobOffset = (ulong)stream.Position;
            writer.WriteBlob(new SdafBlobWriteOptions { SchemaId = 2, StreamId = 20, ItemIndex = 42, Compression = SdafCompression.Zstandard }, [1, 2, 3, 4]);
            writer.WriteNote("hello");
            writer.WriteIndex([new SdafIndexEntry(blobOffset, 2, 20, 42, 1, long.MinValue, long.MinValue)]);
            writer.WritePrivateRecord(0x8001, [1, 2], [3, 4], true);
        }
        stream.Position = 0;
        using var reader = new SdafReader(stream);
        List<SdafRecord> records = reader.ReadRecords().ToList();
        Assert.Multiple(() =>
        {
            Assert.That(records.OfType<SdafTextRecord>().Single().Message, Is.EqualTo("µ warning"));
            Assert.That(records.OfType<SdafBlobRecord>().Single().DecodedPayload, Is.EqualTo(new byte[] { 1, 2, 3, 4 }));
            Assert.That(records.OfType<SdafNoteRecord>().Single().Message, Is.EqualTo("hello"));
            Assert.That(records.OfType<SdafIndexRecord>().Single().Entries[0].ItemIndexOrSampleIndex(), Is.EqualTo(42));
            Assert.That(records.OfType<SdafUnknownRecord>().Single().Payload, Is.EqualTo(new byte[] { 3, 4 }));
        });
    }

    [Test]
    public void ReaderLimitIsAppliedBeforePayloadAllocation()
    {
        string fixture = Path.Combine(TestContext.CurrentContext.TestDirectory, "fixtures", "valid", "minimal-leading.sdaf");
        using var stream = File.OpenRead(fixture);
        using var reader = new SdafReader(stream, new SdafLimits { MaxPayloadSize = 8 });
        Assert.Throws<SdafFormatException>(() => reader.ReadRecords().ToList());
    }

    private static SdafSchema TwoChannelSchema() => new(1, 1,
    [
        new(SdafObjectKind.Stream, 1, [SdafTlv.Utf8(1, "adc"), SdafTlv.UInt8(100, 1), SdafTlv.Rational(101, 1UL, 1000), SdafTlv.UInt8(103, 1), SdafTlv.UInt32(104, 2)]),
        Channel(1, 1, "adc0", 12), Channel(2, 1, "adc1", 12),
    ]);

    private static SdafSchema BlobAndTextSchema() => new(2, 1,
    [
        new(SdafObjectKind.Stream, 10, [SdafTlv.Utf8(1, "events"), SdafTlv.UInt32(104, 0), SdafTlv.UInt8(106, 2)]),
        new(SdafObjectKind.Stream, 20, [SdafTlv.Utf8(1, "raw"), SdafTlv.UInt32(104, 0), SdafTlv.UInt8(106, 3), SdafTlv.Utf8(107, "application/octet-stream")]),
    ]);

    private static SdafSchemaObject Channel(uint id, uint stream, string name, ushort bits) => new(SdafObjectKind.Channel, id,
        [SdafTlv.Utf8(1, name), SdafTlv.UInt32(200, stream), SdafTlv.UInt8(201, 1), SdafTlv.UInt16(202, bits), SdafTlv.UInt16(203, bits)]);
}

internal static class TestExtensions
{
    internal static ulong ItemIndexOrSampleIndex(this SdafIndexEntry entry) => entry.FirstSampleIndex;
}
