using NUnit.Framework;

namespace Sdaf.Tests;

[TestFixture]
public class ConformanceTests
{
    private static string Fixture(string group, string name) => Path.Combine(TestContext.CurrentContext.TestDirectory, "fixtures", group, name);

    [TestCase("minimal-leading.sdaf")]
    [TestCase("minimal-trailing.sdaf")]
    public void MinimalFixturesDecodeDenseTwelveBitValues(string file)
    {
        using var reader = SdafReader.Open(Fixture("valid", file));
        SdafDataRecord data = reader.ReadRecords().OfType<SdafDataRecord>().Single();
        Assert.That(Values(data), Is.EqualTo(new ulong[] { 0, 4095, 291, 1110, 2748, 1929 }));
        Assert.That(data.Samples!.Select(x => x.TimeTicks), Is.EqualTo(new long?[] { 1_000_000, 1_000_001, 1_000_002 }));
    }

    [Test]
    public void CompressedNumericFixtureDecodesProfile()
    {
        using var reader = SdafReader.Open(Fixture("valid", "compressed-numeric.sdaf"));
        SdafDataRecord data = reader.ReadRecords().OfType<SdafDataRecord>().Single();
        Assert.Multiple(() =>
        {
            Assert.That(data.Transforms.Select(x => x.Id), Is.EqualTo(new ushort[] { 1, 2, 3, 16 }));
            Assert.That(Values(data), Is.EqualTo(new ulong[] { 256, 512, 257, 511, 255, 513 }));
        });
    }

    [Test]
    public void RationalAndSymbolicFixtureAppliesExactScale()
    {
        using var reader = SdafReader.Open(Fixture("valid", "rational-symbolic.sdaf"));
        SdafDataRecord data = reader.ReadRecords().OfType<SdafDataRecord>().Single();
        Assert.Multiple(() =>
        {
            Assert.That(data.Samples![0].Values[0].RawUnsigned, Is.EqualTo(49));
            Assert.That(data.Samples[0].Values[1].RawSigned, Is.EqualTo(2312));
            Assert.That(data.Samples[0].Values[1].PhysicalValue, Is.EqualTo(23.12).Within(1e-12));
        });
    }

    [TestCase("blob-uncompressed.sdaf", 0)]
    [TestCase("blob-zstd.sdaf", 1)]
    public void BlobFixturesDecode(string file, int transformCount)
    {
        using var reader = SdafReader.Open(Fixture("valid", file));
        SdafBlobRecord blob = reader.ReadRecords().OfType<SdafBlobRecord>().Single();
        Assert.Multiple(() =>
        {
            Assert.That(blob.Transforms, Has.Count.EqualTo(transformCount));
            Assert.That(blob.DecodedPayload, Is.EqualTo(new byte[] { 0xaa, 0x55, 1, 2, 3 }));
            Assert.That(blob.ItemIndex, Is.EqualTo(42));
        });
    }

    [TestCase("bad-file-header-crc.sdaf")]
    [TestCase("bad-record-header-crc.sdaf")]
    [TestCase("bad-leading-payload-crc.sdaf")]
    [TestCase("bad-trailer-marker.sdaf")]
    [TestCase("bad-trailer-sequence.sdaf")]
    [TestCase("malformed-tlv-overrun.sdaf")]
    [TestCase("invalid-transform-order.sdaf")]
    [TestCase("blob-decoded-size-mismatch.sdaf")]
    [TestCase("reserved-envelope-flag.sdaf")]
    public void CorruptFixturesAreRejected(string file)
    {
        Assert.Throws<SdafFormatException>(() =>
        {
            using var reader = SdafReader.Open(Fixture("corrupt", file));
            _ = reader.ReadRecords().ToList();
        });
    }

    [TestCase("truncated-record-header.sdaf", 2)]
    [TestCase("truncated-payload.sdaf", 1)]
    public void TruncatedFinalRecordLeavesPriorRecordsAvailable(string file, int completeRecords)
    {
        using var reader = SdafReader.Open(Fixture("corrupt", file));
        List<SdafRecord> records = reader.ReadRecords().ToList();
        Assert.That(records, Has.Count.EqualTo(completeRecords));
        Assert.That(records[0], Is.TypeOf<SdafSchemaRecord>());
    }

    private static ulong[] Values(SdafDataRecord data) => data.Samples!.SelectMany(x => x.Values).Select(x => x.RawUnsigned).ToArray();
}
