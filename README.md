# SDAF for C#

This repository contains a .NET 10 encoder, decoder, and command-line converter for SDAF draft 0.4 (format version 1.0). The implementation is compatible with trimming and Native AOT and uses no reflection-based serialization.

## Projects

- `src/Sdaf`: record model, CRC-32C, schema parser and validator, sample decoder, encoder, and Zstandard transforms.
- `src/Sdaf.Cli`: `sdaf` command-line converter for JSON, CBOR, and long-form CSV.
- `tests/Sdaf.Tests`: NUnit unit, round-trip, CLI, and bundled conformance-fixture tests.

The decoder supports leading and trailing payload CRCs, both sample layouts, dense and byte-aligned packing, all timestamp modes, schema metadata, `TEXT`, `BLOB`, `INDX`, `END!`, `NOTE`, Zstandard-only records, and the complete delta/zigzag/byte-shuffle/Zstandard numeric profile. Unknown record types and unknown `DATA` transforms remain safely skippable.

## Build and test

```sh
dotnet build Sdaf.slnx
dotnet test Sdaf.slnx
```

Zstandard support is provided through an AOT-safe source-generated P/Invoke binding. Systems that read or write Zstandard records need `libzstd` installed. Baseline uncompressed operation does not invoke it.

To produce a native executable, select the runtime identifier for the target system:

```sh
dotnet publish src/Sdaf.Cli/Sdaf.Cli.csproj -c Release -r linux-arm64
```

## Decode with the CLI

```sh
sdaf decode capture.sdaf --format json --output capture.json
sdaf decode capture.sdaf --format cbor --output capture.cbor
sdaf decode capture.sdaf --format csv --output samples.csv
```

The `decode` verb is optional, and `--output -` (or no output option) writes to standard output. JSON and CBOR include record metadata and decoded content. CSV is emitted only for decoded `DATA` samples and uses this stable long-form header:

```text
sequence,stream_id,sample_index,time_ticks,channel_id,channel_name,element,raw,physical,unit
```

Long-form CSV remains usable when schemas or channel sets change within one file. `TEXT` and `BLOB` records are represented in JSON and CBOR because they do not have a tabular sample shape.

## Library examples

Decode a stream sequentially:

```csharp
using Sdaf;

using var reader = SdafReader.Open("capture.sdaf");
foreach (SdafRecord record in reader.ReadRecords())
{
    if (record is SdafDataRecord { Samples: not null } data)
    {
        foreach (SdafSample sample in data.Samples)
            Console.WriteLine($"{sample.Index}: {sample.Values[0].PhysicalValue}");
    }
}
```

Write an uncompressed sample record:

```csharp
using Sdaf;

var schema = new SdafSchema(1, 1,
[
    new SdafSchemaObject(SdafObjectKind.Stream, 1,
    [
        SdafTlv.Utf8(1, "adc"),
        SdafTlv.UInt32(104, 1),
    ]),
    new SdafSchemaObject(SdafObjectKind.Channel, 1,
    [
        SdafTlv.Utf8(1, "adc0"),
        SdafTlv.UInt32(200, 1),
        SdafTlv.UInt8(201, (byte)SdafLogicalType.UnsignedInteger),
        SdafTlv.UInt16(202, 12),
        SdafTlv.UInt16(203, 12),
    ]),
]);

using var output = File.Create("capture.sdaf");
using var writer = new SdafWriter(output);
writer.WriteSchema(schema);
writer.WriteData(new SdafDataWriteOptions
{
    SchemaId = 1,
    StreamId = 1,
    SampleCount = 2,
    TimestampMode = SdafTimestampMode.None,
    Packing = SdafPacking.Lsb0Dense,
}, Convert.FromHexString("BC3A12"));
writer.WriteEnd();
```

`WriteData` accepts the canonical decoded payload defined by the specification. Select `SdafCompression.Zstandard` or `SdafCompression.CompressedNumeric` to transform it before storage. The reader exposes both the stored payload and, when the schema and transforms are supported, the canonical decoded payload and typed samples.

## Validation and limits

`SdafReader` validates file and record CRC-32C values, reserved fields, envelope sizes, trailer association, UTF-8, schema object/TLV bounds and references, channel layouts, padding/sign-extension bits, transform ordering, and decoded sizes. An EOF inside the final record is treated as an abrupt but valid end; a complete corrupt record throws `SdafFormatException` after any earlier records have been yielded.

Allocation limits are configurable with `SdafLimits`. Defaults follow the draft's desktop guidance, while individual managed arrays are additionally limited by the runtime's `Int32` indexing limit. The reader is forward-only and intentionally permits one record enumeration.
