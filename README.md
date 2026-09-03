# SDAF implementations

This repository contains C99 and .NET 10 encoders, decoders, and command-line converters for SDAF draft 0.4 (format version 1.0). The C implementation builds with a conforming C99 compiler. The C# implementation is compatible with trimming and Native AOT and uses no reflection-based serialization.

## Repository layout

```text
.
├── implementations/
│   ├── c/                  C99 library, CLI, Unity tests, and CMake project
│   ├── dotnet/             C# library, CLI, TUnit tests, and .NET solution
│   └── prototyping/        Experimental benchmark programs and scripts
└── spec/
    ├── SDAF-Format-v1-draft.md
    └── sdaf-conformance/   Conformance fixtures, annotations, and generators
```

The principal implementation directories are:

- `implementations/c/include/sdaf`: public C99 API.
- `implementations/c/src`: C99 CRC, schema, codec, decoder, and encoder implementation.
- `implementations/c/cli`: `sdaf-c` command-line converter.
- `implementations/c/tests`: Throw The Switch Unity unit and conformance tests.
- `implementations/dotnet/Sdaf`: C# record model, CRC-32C, schema parser and validator, sample decoder, encoder, and Zstandard transforms.
- `implementations/dotnet/Sdaf.Cli`: `sdaf` command-line converter for JSON, CBOR, and long-form CSV.
- `implementations/dotnet/Sdaf.Tests`: TUnit unit, round-trip, CLI, and bundled conformance-fixture tests.

The decoder supports leading and trailing payload CRCs, both sample layouts, dense and byte-aligned packing, all timestamp modes, schema metadata, `TEXT`, `BLOB`, `INDX`, `END!`, `NOTE`, Zstandard-only records, and the complete delta/zigzag/byte-shuffle/Zstandard numeric profile. Unknown record types and unknown `DATA` transforms remain safely skippable.

## Build and test C99

The CMake build requires a C99 compiler. It detects `libzstd` with pkg-config. The first test configuration downloads the pinned official Unity v2.7.0 release with CMake `FetchContent`.

```sh
cmake -S implementations/c -B implementations/c/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build implementations/c/build
ctest --test-dir implementations/c/build --output-on-failure
```

CTest registers the Unity runner as one aggregate test named `sdaf_c_tests`; that executable runs the individual Unity cases. Use `ctest --test-dir implementations/c/build --verbose` or run `implementations/c/build/sdaf_c_tests` directly to see every case.

Disable tests with `-DSDAF_BUILD_TESTS=OFF`. A baseline build without `libzstd` still reads and writes uncompressed SDAF and safely retains supported-envelope records containing unavailable transforms without presenting them as corrupt. Compression-writing functions return `SDAF_ERROR_UNSUPPORTED` when Zstandard was not built.

Run the C CLI with the same output choices as the .NET tool:

```sh
implementations/c/build/sdaf-c decode capture.sdaf --format json --output capture.json
implementations/c/build/sdaf-c decode capture.sdaf --format cbor --output capture.cbor
implementations/c/build/sdaf-c decode capture.sdaf --format csv --output samples.csv
```

The C decoder API consumes a memory range or file and owns every allocation in the resulting `sdaf_document`:

```c
#include <sdaf/sdaf.h>

sdaf_document document;
sdaf_status status = sdaf_decode_file("capture.sdaf", NULL, &document);
if (status == SDAF_OK) {
    size_t i;
    for (i = 0; i < document.record_count; ++i) {
        const sdaf_record *record = &document.records[i];
        if (record->envelope.type == SDAF_RECORD_DATA &&
            record->value.data.samples != NULL) {
            /* Consume record->value.data.samples here. */
        }
    }
}
sdaf_document_free(&document);
```

On a complete malformed record, `sdaf_decode` returns an error while leaving earlier valid records in the document for inspection. An incomplete final record returns `SDAF_OK` and is omitted. Always call `sdaf_document_free`, including after an error. `sdaf_encoder_init` creates an in-memory output buffer; call the type-specific write functions, then use `sdaf_encoder_write_file` or the `data`/`size` members, and finally call `sdaf_encoder_free`.

The default `sdaf_limits` follow the draft’s desktop guidance and can be reduced before decoding. Limit failures use `SDAF_ERROR_LIMIT`, separately from malformed input. Schema TLV byte buffers supplied to the encoder remain owned by the caller.

## Build and test C#

Run the .NET commands from its implementation directory so the .NET 10 SDK picks up the colocated `global.json` configuration:

```sh
cd implementations/dotnet
dotnet build Sdaf.slnx
dotnet test --solution Sdaf.slnx
```

The test project uses TUnit on Microsoft.Testing.Platform. The `implementations/dotnet/global.json` file selects that runner for the .NET 10 `dotnet test` command.

Zstandard support is provided through an AOT-safe source-generated P/Invoke binding. Systems that read or write Zstandard records need `libzstd` installed. Baseline uncompressed operation does not invoke it.

To produce a native executable, select the runtime identifier for the target system:

```sh
dotnet publish Sdaf.Cli/Sdaf.Cli.csproj -c Release -r linux-arm64
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
