namespace Sdaf.Tests;

public class CodecTests
{
    [Test]
    public async Task Crc32CKnownVectors()
    {
        using (Assert.Multiple())
        {
            await Assert.That(SdafCrc32C.Compute([])).IsEqualTo(0U);
            await Assert.That(SdafCrc32C.Compute("123456789"u8)).IsEqualTo(0xe3069283u);
        }
    }

    [Test]
    [Arguments(SdafCompression.None, false)]
    [Arguments(SdafCompression.None, true)]
    [Arguments(SdafCompression.Zstandard, false)]
    [Arguments(SdafCompression.CompressedNumeric, false)]
    public async Task WriterReaderRoundTripData(SdafCompression compression, bool trailer)
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
        using (Assert.Multiple())
        {
            await Assert.That(samples.SelectMany(x => x.Values).Select(x => x.RawUnsigned)).IsEquivalentTo(new ulong[] { 0, 4095, 291, 1110, 2748, 1929 }, CollectionOrdering.Matching);
            await Assert.That(samples.Select(x => x.TimeTicks)).IsEquivalentTo(new long?[] { 100, 102, 104 }, CollectionOrdering.Matching);
            await Assert.That(data.Envelope.PayloadCrcInTrailer).IsEqualTo(trailer);
            await Assert.That(records[^1]).IsTypeOf<SdafEndRecord>();
        }
    }

    [Test]
    public async Task RoundTripsTextBlobNoteIndexAndPrivateRecord()
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
        using (Assert.Multiple())
        {
            await Assert.That(records.OfType<SdafTextRecord>().Single().Message).IsEqualTo("µ warning");
            await Assert.That(records.OfType<SdafBlobRecord>().Single().DecodedPayload).IsEquivalentTo(new byte[] { 1, 2, 3, 4 }, CollectionOrdering.Matching);
            await Assert.That(records.OfType<SdafNoteRecord>().Single().Message).IsEqualTo("hello");
            await Assert.That(records.OfType<SdafIndexRecord>().Single().Entries[0].ItemIndexOrSampleIndex()).IsEqualTo(42UL);
            await Assert.That(records.OfType<SdafUnknownRecord>().Single().Payload).IsEquivalentTo(new byte[] { 3, 4 }, CollectionOrdering.Matching);
        }
    }

    [Test]
    public async Task ReaderLimitIsAppliedBeforePayloadAllocation()
    {
        string fixture = Path.Combine(AppContext.BaseDirectory, "fixtures", "valid", "minimal-leading.sdaf");
        using var stream = File.OpenRead(fixture);
        using var reader = new SdafReader(stream, new SdafLimits { MaxPayloadSize = 8 });
        await Assert.That(() => { _ = reader.ReadRecords().ToList(); }).Throws<SdafFormatException>();
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
