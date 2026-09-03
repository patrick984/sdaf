namespace Sdaf.Tests;

public class ConformanceTests
{
    private static string Fixture(string group, string name) => Path.Combine(AppContext.BaseDirectory, "fixtures", group, name);

    [Test]
    [Arguments("minimal-leading.sdaf")]
    [Arguments("minimal-trailing.sdaf")]
    public async Task MinimalFixturesDecodeDenseTwelveBitValues(string file)
    {
        using var reader = SdafReader.Open(Fixture("valid", file));
        SdafDataRecord data = reader.ReadRecords().OfType<SdafDataRecord>().Single();
        await Assert.That(Values(data)).IsEquivalentTo(new ulong[] { 0, 4095, 291, 1110, 2748, 1929 }, CollectionOrdering.Matching);
        await Assert.That(data.Samples!.Select(x => x.TimeTicks)).IsEquivalentTo(new long?[] { 1_000_000, 1_000_001, 1_000_002 }, CollectionOrdering.Matching);
    }

    [Test]
    public async Task CompressedNumericFixtureDecodesProfile()
    {
        using var reader = SdafReader.Open(Fixture("valid", "compressed-numeric.sdaf"));
        SdafDataRecord data = reader.ReadRecords().OfType<SdafDataRecord>().Single();
        using (Assert.Multiple())
        {
            await Assert.That(data.Transforms.Select(x => x.Id)).IsEquivalentTo(new ushort[] { 1, 2, 3, 16 }, CollectionOrdering.Matching);
            await Assert.That(Values(data)).IsEquivalentTo(new ulong[] { 256, 512, 257, 511, 255, 513 }, CollectionOrdering.Matching);
        }
    }

    [Test]
    public async Task RationalAndSymbolicFixtureAppliesExactScale()
    {
        using var reader = SdafReader.Open(Fixture("valid", "rational-symbolic.sdaf"));
        SdafDataRecord data = reader.ReadRecords().OfType<SdafDataRecord>().Single();
        using (Assert.Multiple())
        {
            await Assert.That(data.Samples![0].Values[0].RawUnsigned).IsEqualTo(49UL);
            await Assert.That(data.Samples[0].Values[1].RawSigned).IsEqualTo(2312L);
            await Assert.That(data.Samples[0].Values[1].PhysicalValue).IsEqualTo(23.12).Within(1e-12);
        }
    }

    [Test]
    [Arguments("blob-uncompressed.sdaf", 0)]
    [Arguments("blob-zstd.sdaf", 1)]
    public async Task BlobFixturesDecode(string file, int transformCount)
    {
        using var reader = SdafReader.Open(Fixture("valid", file));
        SdafBlobRecord blob = reader.ReadRecords().OfType<SdafBlobRecord>().Single();
        using (Assert.Multiple())
        {
            await Assert.That(blob.Transforms).Count().IsEqualTo(transformCount);
            await Assert.That(blob.DecodedPayload).IsEquivalentTo(new byte[] { 0xaa, 0x55, 1, 2, 3 }, CollectionOrdering.Matching);
            await Assert.That(blob.ItemIndex).IsEqualTo(42UL);
        }
    }

    [Test]
    [Arguments("bad-file-header-crc.sdaf")]
    [Arguments("bad-record-header-crc.sdaf")]
    [Arguments("bad-leading-payload-crc.sdaf")]
    [Arguments("bad-trailer-marker.sdaf")]
    [Arguments("bad-trailer-sequence.sdaf")]
    [Arguments("malformed-tlv-overrun.sdaf")]
    [Arguments("invalid-transform-order.sdaf")]
    [Arguments("blob-decoded-size-mismatch.sdaf")]
    [Arguments("reserved-envelope-flag.sdaf")]
    public async Task CorruptFixturesAreRejected(string file)
    {
        await Assert.That(() =>
        {
            using var reader = SdafReader.Open(Fixture("corrupt", file));
            _ = reader.ReadRecords().ToList();
        }).Throws<SdafFormatException>();
    }

    [Test]
    [Arguments("truncated-record-header.sdaf", 2)]
    [Arguments("truncated-payload.sdaf", 1)]
    public async Task TruncatedFinalRecordLeavesPriorRecordsAvailable(string file, int completeRecords)
    {
        using var reader = SdafReader.Open(Fixture("corrupt", file));
        List<SdafRecord> records = reader.ReadRecords().ToList();
        await Assert.That(records).Count().IsEqualTo(completeRecords);
        await Assert.That(records[0]).IsTypeOf<SdafSchemaRecord>();
    }

    private static ulong[] Values(SdafDataRecord data) => data.Samples!.SelectMany(x => x.Values).Select(x => x.RawUnsigned).ToArray();
}
