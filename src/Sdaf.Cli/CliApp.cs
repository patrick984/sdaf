using System.Globalization;
using System.Text;

namespace Sdaf.Cli;

public static class CliApp
{
    public static int Run(string[] args, Stream standardOutput, TextWriter standardError)
    {
        try
        {
            if (args.Length == 0 || args.Contains("--help") || args.Contains("-h"))
            {
                WriteHelp(standardError); return args.Length == 0 ? 2 : 0;
            }
            int index = args[0] == "decode" ? 1 : 0;
            if (index >= args.Length || args[index].StartsWith('-')) throw new ArgumentException("An input SDAF path is required.");
            string input = args[index++]; string format = "json"; string? output = null;
            while (index < args.Length)
            {
                string option = args[index++];
                if (option is "--format" or "-f") format = Next(args, ref index, option).ToLowerInvariant();
                else if (option is "--output" or "-o") output = Next(args, ref index, option);
                else throw new ArgumentException($"Unknown option '{option}'.");
            }
            if (format is not ("json" or "cbor" or "csv")) throw new ArgumentException("Format must be json, cbor, or csv.");
            using var reader = SdafReader.Open(input);
            List<SdafRecord> records = reader.ReadRecords().ToList();
            Stream destination = output is null or "-" ? standardOutput : File.Create(output);
            try
            {
                if (format == "json") SdafExports.WriteJson(destination, reader.Header, records);
                else if (format == "cbor") SdafExports.WriteCbor(destination, reader.Header, records);
                else SdafExports.WriteCsv(destination, records);
            }
            finally { if (!ReferenceEquals(destination, standardOutput)) destination.Dispose(); }
            return 0;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or SdafFormatException)
        {
            standardError.WriteLine($"sdaf: {ex.Message}"); return 1;
        }
    }

    private static string Next(string[] args, ref int index, string option) => index < args.Length ? args[index++] : throw new ArgumentException($"{option} requires a value.");
    private static void WriteHelp(TextWriter writer) => writer.WriteLine("Usage: sdaf decode <input.sdaf> [--format json|cbor|csv] [--output <path>|-]");
}
