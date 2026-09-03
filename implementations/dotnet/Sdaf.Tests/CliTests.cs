using System.Text;
using System.Text.Json;
using Sdaf.Cli;

namespace Sdaf.Tests;

public class CliTests
{
    private static string Fixture => Path.Combine(AppContext.BaseDirectory, "fixtures", "valid", "minimal-leading.sdaf");

    [Test]
    public async Task JsonContainsDecodedSamples()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        int result = CliApp.Run(["decode", Fixture, "--format", "json"], output, error);
        using JsonDocument doc = JsonDocument.Parse(output.ToArray());
        JsonElement data = doc.RootElement.GetProperty("records")[1];
        using (Assert.Multiple())
        {
            await Assert.That(result).IsEqualTo(0);
            await Assert.That(data.GetProperty("type").GetString()).IsEqualTo("DATA");
            await Assert.That(data.GetProperty("samples")[0].GetProperty("values")[1].GetProperty("raw").GetUInt64()).IsEqualTo(4095UL);
        }
    }

    [Test]
    public async Task CsvIsLongFormAndInvariant()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        int result = CliApp.Run([Fixture, "-f", "csv"], output, error);
        string text = Encoding.UTF8.GetString(output.ToArray());
        using (Assert.Multiple())
        {
            await Assert.That(result).IsEqualTo(0);
            await Assert.That(text).StartsWith("sequence,stream_id,sample_index");
            await Assert.That(text).Contains("1,1,2,1000002,2,adc1,0,1929,1929,code");
        }
    }

    [Test]
    public async Task CborStartsWithTwoEntryMap()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        int result = CliApp.Run([Fixture, "-f", "cbor"], output, error);
        using (Assert.Multiple())
        {
            await Assert.That(result).IsEqualTo(0);
            await Assert.That(output.ToArray()[0]).IsEqualTo((byte)0xa2);
        }
    }

    [Test]
    public async Task InvalidFormatReturnsFailure()
    {
        using var output = new MemoryStream(); using var error = new StringWriter();
        await Assert.That(CliApp.Run([Fixture, "-f", "xml"], output, error)).IsEqualTo(1);
        await Assert.That(error.ToString()).Contains("json, cbor, or csv");
    }
}
