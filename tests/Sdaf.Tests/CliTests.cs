using System.Text;
using System.Text.Json;
using NUnit.Framework;
using Sdaf.Cli;

namespace Sdaf.Tests;

[TestFixture]
public class CliTests
{
    private static string Fixture => Path.Combine(TestContext.CurrentContext.TestDirectory, "fixtures", "valid", "minimal-leading.sdaf");

    [Test]
    public void JsonContainsDecodedSamples()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        int result = CliApp.Run(["decode", Fixture, "--format", "json"], output, error);
        using JsonDocument doc = JsonDocument.Parse(output.ToArray());
        JsonElement data = doc.RootElement.GetProperty("records")[1];
        Assert.Multiple(() =>
        {
            Assert.That(result, Is.Zero, error.ToString());
            Assert.That(data.GetProperty("type").GetString(), Is.EqualTo("DATA"));
            Assert.That(data.GetProperty("samples")[0].GetProperty("values")[1].GetProperty("raw").GetUInt64(), Is.EqualTo(4095));
        });
    }

    [Test]
    public void CsvIsLongFormAndInvariant()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        int result = CliApp.Run([Fixture, "-f", "csv"], output, error);
        string text = Encoding.UTF8.GetString(output.ToArray());
        Assert.Multiple(() =>
        {
            Assert.That(result, Is.Zero, error.ToString());
            Assert.That(text, Does.StartWith("sequence,stream_id,sample_index"));
            Assert.That(text, Does.Contain("1,1,2,1000002,2,adc1,0,1929,1929,code"));
        });
    }

    [Test]
    public void CborStartsWithTwoEntryMap()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        int result = CliApp.Run([Fixture, "-f", "cbor"], output, error);
        Assert.Multiple(() => { Assert.That(result, Is.Zero, error.ToString()); Assert.That(output.ToArray()[0], Is.EqualTo(0xa2)); });
    }

    [Test]
    public void InvalidFormatReturnsFailure()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        Assert.That(CliApp.Run([Fixture, "-f", "xml"], output, error), Is.EqualTo(1));
        Assert.That(error.ToString(), Does.Contain("json, cbor, or csv"));
    }
}
