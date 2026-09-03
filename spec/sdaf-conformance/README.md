# SDAF version-1 conformance fixtures

This directory accompanies the SDAF version-1 specification. It contains
machine-readable valid files, deliberately corrupt files, a JSON manifest and a
non-normative Kaitai Struct description.

## Contents

- `valid/`: files that a reader supporting the indicated feature must accept.
- `corrupt/`: files whose affected record must not be delivered.
- `annotations/`: record offsets and envelope metadata for valid files.
- `manifest.json`: expected outcome, SHA-256 and decoded assertions for every
  fixture.
- `generate_fixtures.py`: deterministic fixture construction and corruption
  injection.
- `verify_fixtures.py`: independent structural, CRC, transform and manifest
  verification for the bundled files.
- `sdaf-v1.ksy`: strict structural description for Kaitai-based inspection.

The corrupt fixtures may contain valid records before the named fault. A reader
may stop at the fault or enter recovery mode as allowed by the specification,
but it must never deliver the damaged or semantically invalid record as valid.
The two truncated-final-record fixtures are valid abrupt file endings; their
incomplete final record is unavailable while all earlier records remain valid.

## Regeneration

The generator requires Python 3 and the `zstandard` package:

```sh
python3 generate_fixtures.py
python3 verify_fixtures.py
```

The bundled SHA-256 hashes identify this exact suite release. Zstandard does not
require encoders to produce a unique byte sequence, so compressed-frame bytes
are not normative. Their decoded assertions in `manifest.json` are normative.

## Conformance use

A baseline reader is tested against all fixtures that do not name Zstandard or
the compressed numeric profile. Readers claiming optional features are also
tested against the relevant compressed fixtures. Inspection tools may use the
Kaitai grammar, but conformance is determined by the SDAF specification and the
manifest, not by generated Kaitai code.

The Kaitai grammar validates constants, bounds represented in the grammar,
reserved fields and record structure. Kaitai Struct has no built-in CRC-32C
process, so a conformance harness must independently validate file-header,
record-header and payload CRCs. It must also perform semantic checks that span
schema references, decoded transform sizes and codec output.
