# SDAF: Self-Describing Data Acquisition Format

Draft specification 0.4 — proposed format version 1.0

Status: design draft, not yet stable

## 1. Purpose

SDAF is a sequential, self-describing binary format for measurement and data-acquisition logs. It is intended to be:

- writable by a small, allocation-free C implementation;
- self-describing without an external schema;
- readable after power loss or a truncated final write;
- efficient for fixed-width numeric samples, including non-byte widths;
- capable of periodic, irregular and asynchronous streams;
- extensible without making the version-1 decoder complex; and
- sufficiently regular that an unfamiliar file can be investigated with a hex editor.

Version 1 standardizes an uncompressed core, timestamped variable-length binary items, explicit clock descriptions and an optional compressed numeric profile. Compression and typed transforms are layered on independently decodable records. Baseline and embedded implementations are not required to implement compression.

The words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** and **MAY** are normative.

## 2. Design rules

1. Structural integers are fixed-width little-endian. Structural metadata never uses varints.
2. Every record begins with the ASCII marker `SDRC`, has an explicit total header size and payload size, and is independently checksummed.
3. A reader may skip an unknown record or unknown TLV using its length.
4. A file does not require a footer or a clean-close record to be valid.
5. Schemas precede the data that use them and may be repeated verbatim.
6. A baseline writer needs only file, schema, data and text records. Compression, indexes and end records are optional.
7. Reserved bytes are zero. Padding bits are zero. This makes corruption and undocumented extensions conspicuous.

## 3. Primitive representations

| Name | Representation |
| --- | --- |
| `u8`, `i8` | 8-bit integer |
| `u16`, `i16` | 16-bit little-endian integer |
| `u32`, `i32` | 32-bit little-endian integer |
| `u64`, `i64` | 64-bit little-endian integer |
| `f32`, `f64` | IEEE 754 binary32/binary64, little-endian |
| `bool` | `u8`; only 0 and 1 are valid |
| `utf8` | un-terminated UTF-8 byte sequence whose containing field supplies the byte count |
| UUID | 16 uninterpreted bytes in displayed/network order; no mixed-endian encoding |

Signed integers use two's complement. Unless a field says otherwise, offsets are absolute byte offsets from the first file byte.

## 4. File header

Every file begins with this 64-byte header:

| Offset | Size | Field | Required value or meaning |
| ---: | ---: | --- | --- |
| 0 | 8 | magic | `53 44 41 46 0d 0a 1a 0a`, ASCII `SDAF` followed by CR LF SUB LF |
| 8 | 1 | major | `1` |
| 9 | 1 | minor | `0` for this specification |
| 10 | 2 | header_size | `64` |
| 12 | 4 | byte_order_check | `0x12345678` |
| 16 | 4 | feature_flags | zero in version 1 |
| 20 | 4 | reserved | zero |
| 24 | 8 | created_unix_ns | signed Unix nanoseconds; `INT64_MIN` if unknown |
| 32 | 16 | file_uuid | random UUID bytes; all zero if unavailable |
| 48 | 8 | first_record_offset | `64` |
| 56 | 4 | header_crc32c | CRC-32C of all 64 bytes with this field zero |
| 60 | 4 | reserved | zero |

The unusual eight-byte magic helps distinguish SDAF from text and detects newline conversion. A reader MUST reject a different major version. It MAY accept a larger `header_size` by skipping unknown trailing header bytes after validating the CRC over the declared header.

## 5. Common record envelope

Records follow consecutively. Every record starts with a 32-byte common header:

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | marker | ASCII `SDRC` (`53 44 52 43`) |
| 4 | 2 | record_type | See section 6 |
| 6 | 2 | flags | Bits 0–7 are common envelope flags; bits 8–15 are type-specific |
| 8 | 2 | header_size | Common plus type-specific header; minimum 32 |
| 10 | 1 | record_version | Version of this record type; initially 1 |
| 11 | 1 | reserved | zero |
| 12 | 4 | sequence | Starts at zero and normally increments by one |
| 16 | 8 | payload_size | Stored payload bytes, excluding the header |
| 24 | 4 | header_crc32c | CRC-32C of `header_size` bytes with this field zero |
| 28 | 4 | payload_crc32c | Leading payload CRC-32C, or zero when a CRC trailer is selected |

The type-specific header immediately follows the common header and is included in `header_size` and the header CRC. The payload immediately follows the complete header. A payload CRC trailer, when selected, immediately follows the payload. There is no implicit alignment or padding between these parts or between records.

CRC-32C means the Castagnoli polynomial, reflected polynomial `0x82f63b78`, initial value `0xffffffff`, reflected input/output, and final XOR `0xffffffff`. The check value for ASCII `123456789` is `0xe3069283`; the CRC of zero bytes is `0x00000000`.

### 5.1 Payload CRC placement

Common envelope flag bit 0 is `PAYLOAD_CRC_IN_TRAILER`. Common flag bits 1–7 are reserved and MUST be zero in version 1. Type-specific flag bits are defined by each record type and otherwise MUST be zero.

When `PAYLOAD_CRC_IN_TRAILER` is clear:

- `payload_crc32c` in the common header contains the CRC-32C of exactly the stored payload bytes;
- the CRC of an empty payload is `0x00000000`; and
- no payload trailer is present.

This leading-CRC representation is canonical for writers that already hold the complete stored payload. It adds no bytes beyond the common header.

When `PAYLOAD_CRC_IN_TRAILER` is set:

- `payload_crc32c` in the common header MUST be zero;
- the writer calculates CRC-32C while emitting the stored payload; and
- the following fixed 16-byte trailer immediately follows the last payload byte:

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | marker | ASCII `SDCT` (`53 44 43 54`) |
| 4 | 2 | trailer_size | `16` |
| 6 | 1 | trailer_version | `1` |
| 7 | 1 | flags | zero |
| 8 | 4 | sequence | MUST equal the common header sequence |
| 12 | 4 | payload_crc32c | CRC-32C of exactly the stored payload bytes |

The trailer itself is not included in `payload_size` or the payload CRC. The repeated sequence number associates the trailer with its record and helps reject a misplaced candidate. A reader MUST validate the marker, size, version, flags, sequence and payload CRC before delivering the record.

Trailer mode permits a writer to emit a payload directly from its source without buffering it solely to calculate the CRC, but `payload_size` MUST still be known before the common header is emitted. For uncompressed fixed-layout data this size is normally calculable from the schema and sample count.

Trailer mode does not make an unknown compressed output size streamable as one record. A compressor that cannot predict its stored output size must buffer the compressed chunk, perform a sizing pass, seek back to patch the header where the storage permits it, or use smaller independently framed records. Version 1 deliberately has no unknown-length record mode.

The total stored record size is therefore:

```text
header_size + payload_size + (PAYLOAD_CRC_IN_TRAILER ? 16 : 0)
```

The recommended embedded strategy remains to build bounded chunks in caller-owned storage when practical. Writers with a known payload size may instead use trailer mode to avoid buffering solely for CRC calculation.

### 5.2 Recovery scan

After a malformed record, a recovery reader MAY search byte-by-byte for `SDRC`. A candidate is accepted only when:

- `header_size >= 32` and is within the implementation limit;
- `payload_size` and any required 16-byte trailer are within the implementation limit and remaining file extent;
- reserved fields and common or type-specific flag bits are valid for the claimed version;
- the header CRC is correct; and
- the record type/version combination is understood or safely skippable.

Sequence numbers diagnose missing data but are not identities and may restart in a concatenated or recovered fragment.

## 6. Record types

| Value | FourCC name | Meaning | Required in a basic file? |
| ---: | --- | --- | ---: |
| `0x0001` | `SCMA` | Schema definition | Yes |
| `0x0002` | `DATA` | Fixed-width sample table | Yes when samples exist |
| `0x0003` | `TEXT` | Timestamped UTF-8 message | No |
| `0x0004` | `INDX` | Optional chunk index | No |
| `0x0005` | `END!` | Clean-close summary | No |
| `0x0006` | `BLOB` | Timestamped variable-length binary item | No |
| `0x7fff` | `NOTE` | Human-readable UTF-8 note | No |

The FourCC names are documentary mnemonics; the on-disk common header stores the numeric value. Values `0x8000`–`0xffff` are private-use record types. A reader MUST skip unknown records with a valid envelope.

## 7. Schema encoding

A `SCMA` record has a 16-byte type-specific header, making `header_size = 48`:

| Relative offset | Size | Field |
| ---: | ---: | --- |
| 32 | 4 | schema_id (`u32`, nonzero) |
| 36 | 4 | schema_revision (`u32`, starts at 1) |
| 40 | 4 | object_count (`u32`) |
| 44 | 4 | reserved, zero |

Its payload is exactly `object_count` consecutive schema objects.

### 7.1 Schema object envelope

| Offset within object | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | object_kind |
| 1 | 1 | flags, zero in version 1 |
| 2 | 2 | header_size, `12` in version 1 |
| 4 | 4 | object_size including this envelope and all TLVs |
| 8 | 4 | object_id, nonzero and unique within its kind |
| 12 | … | TLVs |

Objects MUST NOT be nested. Relationships use numeric IDs.

| Kind | Value | Meaning |
| --- | ---: | --- |
| File metadata | 1 | Describes the recording/application |
| Stream | 2 | A sequence of chunks sharing acquisition and layout rules |
| Channel | 3 | A field sampled in a stream |
| Clock | 4 | Identifies a timestamp source and its native counter |
| Value map | 5 | Assigns names and descriptions to integer values |
| Bitfield | 6 | Describes named bit ranges within an integer channel |

### 7.2 TLV envelope

Each object contains zero or more TLVs:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | tag |
| 2 | 1 | wire_type |
| 3 | 1 | flags, zero in version 1 |
| 4 | 4 | value_size |
| 8 | … | value |

No alignment is inserted after a TLV. Unknown tags and wire types are skipped using `value_size`.

Tags MUST occur at most once in an object unless their definition explicitly says they are repeatable. Unknown repeated tags are skipped independently.

Wire types are: 1=`u8`, 2=`u16`, 3=`u32`, 4=`u64`, 5=`i64`, 6=`f64`, 7=`utf8`, 8=`bytes`, 9=`rational_u64` (a `u64` numerator followed by a `u64` denominator), 10=`bool`, and 11=`rational_i64` (an `i64` numerator followed by a `u64` denominator). Rational denominators MUST be nonzero.

### 7.3 Common tags

| Tag | Name | Wire type |
| ---: | --- | --- |
| 1 | name | utf8 |
| 2 | description | utf8 |
| 3 | application | utf8 |
| 4 | application_version | utf8 |
| 5 | device_serial | utf8 |
| 6 | source_protocol | utf8 |
| 7 | source_message_type | utf8 |
| 8 | source_class_id | u32 |
| 9 | source_message_id | u32 |
| 10 | source_protocol_version | utf8 |

The source tags are optional machine-readable provenance. They identify the originating protocol and message without changing the SDAF payload semantics. For example, a stream converted from an MRU protocol can retain its protocol name, message name, class and numeric message ID. A decoder MUST NOT require knowledge of the source protocol to interpret ordinary SDAF channels.

### 7.4 Stream tags

| Tag | Name | Wire type | Meaning |
| ---: | --- | --- | --- |
| 100 | time_domain | u8 | 0 unknown, 1 Unix UTC, 2 monotonic device time, 3 TAI, 4 GPS |
| 101 | time_unit | rational_u64 | Seconds per timestamp tick |
| 102 | nominal_period | rational_u64 | Seconds per sample; absent for aperiodic streams |
| 103 | default_layout | u8 | 1 interleaved, 2 planar |
| 104 | channel_count | u32 | Validation aid |
| 105 | clock_id | u32 | Referenced Clock object; zero or absent when timestamps have no identified source |
| 106 | stream_kind | u8 | 1 sample table, 2 text events, 3 binary items, 4 clock correlation |
| 107 | content_type | utf8 | Media type or stable protocol-specific content identifier |
| 108 | source_clock_id | u32 | Clock-correlation streams only |
| 109 | target_clock_id | u32 | Clock-correlation streams only |
| 110 | event_code_map_id | u32 | Text-event streams; referenced Value map for `event_code` |
| 111 | source_id_map_id | u32 | Text-event streams; referenced Value map for `source_id` |

Absent `stream_kind` means 1 for compatibility. `channel_count` MUST be zero for text-event and binary-item streams. A sample-table or clock-correlation stream uses `DATA`; a text-event stream uses `TEXT`; a binary-item stream uses `BLOB`. Binary-item streams MUST have a nonempty `content_type`.

Times in a stream are signed tick counts in the declared time domain. Unix UTC excludes leap seconds, matching conventional Unix time. Applications needing unambiguous leap-second handling should use TAI. When `clock_id` is present, `time_domain` and `time_unit` MAY be omitted and are inherited from the referenced Clock object. If duplicated, they MUST agree.

### 7.5 Channel tags

| Tag | Name | Wire type | Required? |
| ---: | --- | --- | ---: |
| 200 | stream_id | u32 | Yes |
| 201 | logical_type | u8 | Yes |
| 202 | logical_bits | u16 | Numeric types |
| 203 | storage_bits | u16 | Fixed-width numeric types |
| 204 | unit | utf8 | No |
| 205 | scale | f64 | No; default 1 |
| 206 | offset | f64 | No; default 0 |
| 207 | elements_per_sample | u32 | No; default 1 |
| 208 | semantic | utf8 | No, e.g. `voltage` |
| 209 | value_map_id | u32 | No; referenced Value map object for a whole-channel enum |
| 210 | bitfield_id | u32 | No; referenced Bitfield object |
| 211 | scale_rational | rational_i64 | No; exact alternative to tag 205 |
| 212 | offset_rational | rational_i64 | No; exact alternative to tag 206 |

Logical types are: 1=unsigned integer, 2=signed integer, 3=IEEE float, 4=boolean, 5=fixed bytes. Text messages use `TEXT` records rather than sample-table strings in version 1.

For integers, `1 <= logical_bits <= storage_bits <= 64`. `storage_bits` MAY be any integer, not only a multiple of eight. For floats, `(logical_bits, storage_bits)` MUST be `(32,32)` or `(64,64)`. For boolean both are 1. `fixed bytes` requires `storage_bits` to be a multiple of eight.

Tags 205 and 211 are mutually exclusive, as are tags 206 and 212. A rational tag, when present, supplies the exact value. Otherwise the corresponding `f64` tag applies. The defaults remain scale 1 and offset 0.

The physical value is:

```text
physical = raw * scale + offset
```

`raw` remains the authoritative lossless value. Scale, offset and unit supply interpretation.

### 7.6 Clock objects

Clock objects use these tags in addition to common tags:

| Tag | Name | Wire type | Meaning |
| ---: | --- | --- | --- |
| 300 | time_domain | u8 | Same values as stream tag 100 |
| 301 | tick_period | rational_u64 | Seconds per stored timestamp tick |
| 302 | counter_bits | u16 | Native wrapping counter width; zero for unknown or non-wrapping clocks |
| 303 | reset_scope | u8 | 0 unknown, 1 file, 2 device boot/session, 3 externally defined epoch |
| 304 | epoch_description | utf8 | Optional human-readable definition of tick zero |

`time_domain` and `tick_period` are required. `counter_bits` defaults to zero and, when nonzero, describes the originating counter rather than limiting stored SDAF timestamps. `reset_scope` defaults to zero. A writer MUST unwrap a wrapping source counter into signed `i64` ticks before storing it. It MUST create a new Clock object and new dependent streams after a source reset that cannot be represented as a continuation of the same tick sequence.

A clock-correlation stream (`stream_kind = 4`) MUST reference nonzero source and target Clock objects using tags 108 and 109. It is an ordinary fixed-width `DATA` stream with timestamp mode 4 and MUST contain scalar signed-integer channels with semantics `clock.source_ticks` and `clock.target_ticks`. It MAY also contain channels with semantics `clock.source_uncertainty_ticks`, `clock.target_uncertainty_ticks` and `clock.status`. This represents device-to-UTC, device-to-GPS or other clock mappings without a specialized record type.

### 7.7 Value-map and bitfield objects

A Value map object contains repeated tag 400 (`entry`, wire type `bytes`). Each entry is:

| Size | Field |
| ---: | --- |
| 8 | raw value as `u64`; signed interpretation uses the channel's declared width and type |
| 2 | UTF-8 name size |
| 2 | UTF-8 description size |
| name size | name bytes |
| description size | description bytes |

Entry names MUST be nonempty and unique within the object. Raw values MUST also be unique. A Value map referenced by channel tag 209 describes the entire channel value.

An entry TLV's `value_size` MUST equal 12 plus its two declared string sizes.

A Bitfield object requires tag 500 (`storage_bits`, `u16`) and contains repeated tag 501 (`member`, wire type `bytes`). Each member is:

| Size | Field |
| ---: | --- |
| 2 | least-significant bit offset |
| 2 | bit width |
| 4 | referenced Value map object ID, zero if none |
| 2 | flags; bit 0 means reserved, all other bits zero in version 1 |
| 2 | UTF-8 name size |
| 2 | UTF-8 description size |
| name size | name bytes |
| description size | description bytes |

Members MUST fit within `storage_bits` and MUST NOT overlap. A non-reserved member with no Value map is interpreted as a boolean when one bit wide and as an unsigned integer otherwise. The Bitfield `storage_bits` MUST equal that of a channel referencing it through tag 210. Tags 209 and 210 MUST NOT both appear on the same channel; enum members inside a bitfield reference Value maps from their member descriptors.

A member TLV's `value_size` MUST equal 14 plus its two declared string sizes.

### 7.8 Schema evolution

A `(schema_id, schema_revision)` pair is immutable. A changed channel set or interpretation requires a greater revision and a new `SCMA` record before the first referencing `DATA`, `TEXT` or `BLOB` record. IDs MUST NOT be reused for a different meaning within a file.

All numeric object references in a schema revision MUST resolve to an object of the required kind in that same `SCMA` record. A schema revision is therefore independently interpretable and does not require merging objects from earlier revisions.

Repeating an identical schema record is allowed and recommended after a configurable amount of data for recovery. Repetition does not create a new revision.

## 8. Data records

A `DATA` record has this 64-byte type-specific header, making `header_size = 96`:

| Relative offset | Size | Field |
| ---: | ---: | --- |
| 32 | 4 | schema_id |
| 36 | 4 | schema_revision |
| 40 | 4 | stream_id |
| 44 | 4 | sample_count |
| 48 | 8 | first_sample_index |
| 56 | 8 | start_time_ticks (`i64`) |
| 64 | 8 | period_numerator |
| 72 | 8 | period_denominator |
| 80 | 1 | timestamp_mode |
| 81 | 1 | layout |
| 82 | 1 | packing |
| 83 | 1 | transform_count |
| 84 | 4 | timestamp_bytes |
| 88 | 8 | decoded_sample_bytes |

The referenced stream MUST have `stream_kind` 1 or 4.

`decoded_sample_bytes` is the byte length after decompression and inverse transforms, including any timestamp area and the canonical packed sample area. It allows bounded allocation and detects malformed codec output.

### 8.1 Timestamp modes

| Value | Mode | Stored timestamp area |
| ---: | --- | --- |
| 1 | Periodic | None; time for sample `n` is `start_time_ticks + floor(n * period_numerator / period_denominator)` |
| 2 | Delta | `sample_count` signed `i64` deltas; sample time is `start_time_ticks + delta[n]` |
| 3 | Explicit | `sample_count` signed `i64` absolute tick values |
| 4 | None | No timestamps; `start_time_ticks`, period fields and `timestamp_bytes` are zero |

For mode 1, the period fraction is expressed in the stream's timestamp ticks and its denominator MUST be nonzero. Modes 2 and 3 require `timestamp_bytes = sample_count * 8`. Mode 4 is intended for indexed sequences without time.

The timestamp area, if any, precedes sample values in the decoded payload and is always ordinary byte-aligned little-endian `i64`, unaffected by numeric packing.

### 8.2 Layout

Layout 1 is interleaved: all channel fields for sample 0, then all fields for sample 1. Layout 2 is planar: all values for the lowest channel ID, then the next channel ID. Channels are ordered by ascending channel ID, and fixed array elements are stored in ascending element index.

Version 1 requires every `DATA` stream to have a fixed number of bits per sample. Variable-length UTF-8 messages use `TEXT`, and variable-length binary or externally encoded items use `BLOB`. Fixed byte arrays remain valid `DATA` channels when every sample has the same size.

### 8.3 Canonical bit packing

Packing 1 is `lsb0-dense`:

1. Each field contributes exactly `storage_bits` bits.
2. Values are concatenated in layout order with no implicit alignment.
3. Bit zero of a value is emitted first.
4. The first emitted bit occupies bit zero (least significant bit) of the first output byte.
5. Signed values are two's-complement values truncated to `storage_bits`.
6. If `logical_bits < storage_bits`, unused high bits MUST be sign extension for signed integers and zero for unsigned integers.
7. Unused high bits in the final payload byte MUST be zero.

Packing 2 is `byte-aligned`: each field begins at the next byte and occupies `ceil(storage_bits/8)` bytes, least-significant byte first. Its unused high bits follow rule 6. This mode permits direct storage of common DMA buffers, such as a 12-bit ADC delivered in 16-bit words.

Example for packing 1: two unsigned 12-bit values `0xabc`, `0x123` produce bytes `bc 3a 12`. The bit accumulator is the mathematical integer `0x123abc`, emitted little-endian in three bytes.

### 8.4 Transform descriptors

If `transform_count` is nonzero, descriptors occur at the beginning of the stored payload, before encoded data. Each descriptor is:

| Size | Field |
| ---: | --- |
| 2 | transform_id |
| 1 | transform_version |
| 1 | flags, zero in version 1 |
| 4 | parameter_size |
| parameter_size | parameters |

The descriptors are not transformed. The bytes following all descriptors are transformed data. Encoding applies descriptors in listed order; decoding applies them in reverse order.

The following IDs are assigned or reserved:

| ID | Transform | Version-1 status |
| ---: | --- | --- |
| 1 | Delta by typed field | Standard compressed numeric profile |
| 2 | Zigzag signed-to-unsigned | Standard compressed numeric profile |
| 3 | Byte shuffle | Standard compressed numeric profile |
| 4 | Bit-plane transpose | Reserved for future specification |
| 16 | Zstandard frame | Standard optional profile |
| 17 | LZ4 frame | Reserved for future specification |

Only identity (`transform_count = 0`) is required for baseline conformance. Version 1 defines transforms 1–3 only as members of the exact compressed numeric profile in section 8.5. A writer MUST NOT emit one of transforms 1–3 separately, omit one, repeat one, or place them in another order. This restriction keeps the initial interoperability and test surface small; a future minor version may define additional valid compositions.

Transform 4 remains reserved. Writers MUST NOT emit it until a bit-plane profile is standardized. Benchmarking found that delta, zigzag and bit-plane transpose reduced representative compressed payloads by only about 4% relative to the byte-shuffle profile, while substantially reducing decode throughput. Version 1 therefore favors the simpler and faster byte-shuffle profile.

### 8.5 Compressed numeric profile

The standard compressed numeric profile is optional. It is intended for homogeneous integer acquisition streams and applies this exact pipeline:

```text
canonical decoded payload
→ per-field delta
→ zigzag
→ 256-value byte shuffle
→ Zstandard
```

The transform descriptor list MUST contain exactly IDs `1, 2, 3, 16`, in that order. Every descriptor has version 1, flags zero and `parameter_size = 0`.

A `DATA` record is eligible for this profile only when:

- every channel is an unsigned or signed integer;
- every scalar and fixed-array element has the same `logical_bits` value `n`;
- `1 <= n <= 64`;
- the number of logical values is determinable from the schema and `sample_count`; and
- the sample area contains no variable-length fields.

Each fixed-array element is a distinct field lane. Lanes are identified by channel ID followed by array-element index. The transformations preserve the record's declared interleaved or planar layout order.

The timestamp area is not processed by delta, zigzag or byte shuffle. It remains an unchanged prefix to the transformed sample area and is included with that area in the final Zstandard frame.

#### 8.5.1 Delta

Delta maintains one previous raw value for each field lane. All previous values are initialized to zero at the beginning of every `DATA` record; no state crosses a record boundary.

For an `n`-bit lane, encoding is:

```text
delta = (current_raw - previous_raw) mod 2^n
previous_raw = current_raw
```

Decoding is:

```text
current_raw = (previous_raw + delta) mod 2^n
previous_raw = current_raw
```

Signed channel values participate using their `n`-bit two's-complement bit pattern. Samples are visited in declared layout order, while previous state remains independent per lane.

#### 8.5.2 Zigzag

Each `n`-bit delta is interpreted as a signed two's-complement integer `d` in the range `[-2^(n-1), 2^(n-1)-1]`, then mapped to an unsigned `n`-bit value:

```text
d >= 0: zigzag = 2*d
d <  0: zigzag = -2*d - 1
```

Thus `0, -1, +1, -2, +2` map to `0, 1, 2, 3, 4`. Decoding applies the inverse mapping before inverse delta.

#### 8.5.3 Byte shuffle

Zigzag values are first serialized individually into `ceil(n/8)` bytes, least-significant byte first. Unused high bits in the final byte MUST be zero. This byte-aligned intermediate representation is used regardless of the record's canonical `packing` value.

Values are divided in declared layout order into blocks of 256 values. The last block contains the remaining 1–255 values and is not padded. Within a block of `k` values occupying `b = ceil(n/8)` bytes each, output consists of byte lane zero from all `k` values, followed by byte lane one from all `k` values, through byte lane `b-1`:

```text
v0.byte0 v1.byte0 ... v(k-1).byte0
v0.byte1 v1.byte1 ... v(k-1).byte1
...
v0.byte(b-1) ... v(k-1).byte(b-1)
```

Blocks are concatenated without padding. The complete pre-Zstandard byte count is exactly:

```text
timestamp_bytes + logical_value_count * ceil(n/8)
```

Here `logical_value_count` is `sample_count` multiplied by the sum of `elements_per_sample` over all channels in the stream. A decoder MUST validate both multiplications and the final addition for integer overflow before allocating or decompressing.

#### 8.5.4 Zstandard framing

Transform 16 version 1 has no parameters. Its input is all bytes produced by the preceding transform, or the canonical decoded payload when Zstandard is the only transform. Its output is exactly one standard Zstandard frame. Frames are independent between records. Dictionaries and cross-record state are forbidden in version 1.

When Zstandard is present it MUST be the final encoding transform. For Zstandard alone, its decompressed size MUST equal `decoded_sample_bytes`. For the compressed numeric profile, its decompressed size MUST equal the intermediate size given in section 8.5.3; inverse typed transforms then produce exactly `decoded_sample_bytes`. Implementations MUST reject any other size at either boundary.

A buffered writer SHOULD emit the compressed profile only when the complete payload, including all transform descriptors, is smaller than the canonical untransformed payload. Otherwise it SHOULD emit untransformed dense data. This prevents incompressible ADC data from growing because of frame and descriptor overhead.

### 8.6 Operational chunk-size guidance

Chunk size is not part of the wire-format profile. Benchmarks found little additional compression benefit beyond 4,096 samples per channel. Writers SHOULD normally use between 1,024 and 16,384 samples per channel, with 4,096 as a reasonable starting point. Smaller records improve latency and corruption isolation; larger records reduce fixed record overhead. Applications MAY choose sizes outside this range.

## 9. Variable-length item records

`TEXT` and `BLOB` each store one independently checksummed item per record. They do not introduce variable-length fields into the fixed sample-table layout.

### 9.1 Text records

A `TEXT` record has a 32-byte type-specific header, making `header_size = 64`:

| Relative offset | Size | Field |
| ---: | ---: | --- |
| 32 | 4 | schema_id, or zero |
| 36 | 4 | stream_id, or zero for global messages |
| 40 | 8 | time_ticks (`i64`) |
| 48 | 1 | severity |
| 49 | 1 | encoding, 1=UTF-8 |
| 50 | 2 | reserved, zero |
| 52 | 4 | source_id |
| 56 | 4 | event_code (`i32`) |
| 60 | 4 | schema_revision; zero when `schema_id` is zero |

The referenced stream MUST have `stream_kind = 2` and `channel_count = 0`. For a stream message, `schema_id`, `schema_revision` and `stream_id` MUST all be nonzero. A global message has all three fields zero and does not use symbolic source or event-code mappings.

The complete payload is the message bytes, without a terminator. Severity values are 0=unspecified, 1=trace, 2=debug, 3=info, 4=warning, 5=error, 6=fatal. Invalid UTF-8 is a malformed record.

The type-specific common-header flag `TEXT_SOURCE_ID_PRESENT` is bit 8 (`0x0100`), and `TEXT_EVENT_CODE_PRESENT` is bit 9 (`0x0200`). An absent field MUST be zero. `source_id` identifies a component or subsystem, while `event_code` carries a structured diagnostic, alarm or application error code without embedding it in the message text. Stream tags 110 and 111 provide their optional Value maps.

### 9.2 Binary item records

A `BLOB` record has a 48-byte type-specific header, making `header_size = 80`:

| Relative offset | Size | Field |
| ---: | ---: | --- |
| 32 | 4 | schema_id |
| 36 | 4 | schema_revision |
| 40 | 4 | stream_id |
| 44 | 4 | reserved, zero |
| 48 | 8 | item_index |
| 56 | 8 | time_ticks (`i64`); `INT64_MIN` if unknown |
| 64 | 8 | decoded_bytes |
| 72 | 1 | transform_count |
| 73 | 7 | reserved, zero |

`schema_id`, `schema_revision` and `stream_id` MUST all be nonzero. The referenced stream MUST have `stream_kind = 3` and `channel_count = 0`. `item_index` is strictly increasing within a stream but need not begin at zero. `time_ticks` uses the stream's Clock object or its inline time-domain tags. `INT64_MIN` denotes an item for which no acquisition time is known.

The payload begins with `transform_count` descriptors in the format of section 8.4, followed by the encoded item bytes. Version 1 permits either no transform or exactly one transform 16 Zstandard descriptor. Typed transforms 1–3 MUST NOT be applied to `BLOB`.

With no transform, the bytes following the descriptor area have length `decoded_bytes`. With Zstandard, they are one independent standard frame whose decompressed size MUST equal `decoded_bytes`. Dictionaries and cross-record state are forbidden. The payload CRC covers descriptors and encoded bytes. A writer SHOULD use Zstandard only when the complete payload is smaller than the uncompressed representation.

`content_type` and source-provenance tags describe the decoded bytes. A BLOB-aware reader can list, timestamp, extract and integrity-check an item without understanding its content type. Unsupported content does not prevent scanning subsequent records.

## 10. Optional index and clean close

An index is an accelerator, never authoritative data. A reader MUST be able to rebuild it by scanning records.

An `INDX` payload is an array of 48-byte entries:

| Size | Field |
| ---: | --- |
| 8 | record_offset |
| 4 | sequence |
| 4 | stream_id |
| 8 | first_sample_index |
| 4 | sample_count |
| 4 | reserved |
| 8 | first_time_ticks |
| 8 | last_time_ticks |

For a `BLOB` entry, `first_sample_index` contains `item_index` and `sample_count` is 1. For records without a known time, both time fields are `INT64_MIN`.

Its type-specific header contains `entry_count` (`u32`) followed by 12 reserved zero bytes, so `header_size = 48`.

An `END!` record has no payload and a 32-byte type-specific header (`header_size = 64`) containing total record count (`u64`), total data record count (`u64`), last index offset (`u64`, zero if none), and 8 reserved zero bytes.

An `END!` record proves only that the writer attempted a clean close. CRC validation remains required.

## 11. Reader limits and validation

Every implementation MUST impose documented limits before allocation. A suggested desktop baseline is:

- header size: 64 KiB;
- payload size: 1 GiB per record;
- decoded data size: 4 GiB per record;
- schema objects: 65,535;
- channels per stream: 4,096;
- UTF-8 field or message: 16 MiB.

Embedded readers may use much smaller limits. Exceeding a local limit is not evidence that the file is malformed.

A conforming reader validates integer overflow in all size, count and offset calculations, including addition of a CRC trailer. It MUST NOT trust `decoded_sample_bytes`, `decoded_bytes`, object counts, TLV lengths or index offsets until their containing checksums and bounds are validated.

## 12. Truncation and corruption behavior

- EOF between records is a valid abrupt end.
- EOF inside a record header, payload or required CRC trailer makes only that final incomplete record unavailable.
- A complete header with a missing or CRC-invalid payload or trailer is not delivered.
- Earlier valid records remain readable.
- A missing `END!` or `INDX` record does not invalidate the file.
- A reader may stop at the first damaged record or enter recovery scan mode.
- A record referencing an unseen schema revision is retained as opaque bytes but cannot be generically decoded.

A logger SHOULD flush complete record boundaries to durable storage. It SHOULD NOT rewrite earlier bytes during normal acquisition.

## 13. Conformance classes

**Baseline writer:** writes the file header, `SCMA`, untransformed `DATA`, optional `TEXT`, and correct leading payload CRCs. It need not seek, index, compress, write CRC trailers or allocate dynamically.

**Baseline reader:** reads records using either leading or trailing payload CRCs, skips unknown records/TLVs, supports both packing modes and all timestamp modes, and tolerates a missing final record.

**Streaming writer:** additionally supports `PAYLOAD_CRC_IN_TRAILER` for size-known payloads that it emits without first buffering solely to calculate the CRC.

**Indexed writer/reader:** additionally supports `INDX` and `END!`.

**Zstandard reader/writer:** additionally supports transform 16 version 1.

**Compressed numeric profile writer/reader:** additionally supports the exact transform chain in section 8.5, including its eligibility checks, independent per-record delta state, zigzag mapping, 256-value byte shuffle and Zstandard framing.

**Binary-item writer/reader:** writes or reads `BLOB` records, including uncompressed items. Zstandard-compressed BLOB items additionally require the Zstandard class.

The compressed numeric profile is optional for all implementations. In particular, an embedded implementation MAY conform as a baseline writer, baseline reader or streaming writer without linking Zstandard or implementing transforms 1–3. A reader that does not support the profile can still identify and skip its records using the common envelope and transform descriptors.

Clock, Value map and Bitfield schema objects are baseline-reader metadata. A baseline reader MUST parse or skip their TLVs safely and preserve their IDs when presenting schema information, but it need not perform clock correlation or render symbolic enum and bitfield values. Implementations MAY expose those higher-level interpretations as capabilities rather than separate conformance classes.

## 14. Normative payload examples

Multi-byte structural integers below are little-endian. Spaces separate bytes.

### 14.1 Packed 12-bit periodic ADC

Schema meaning:

- stream 1, periodic at one sample per millisecond;
- channel 1 `adc0`, unsigned 12-bit;
- channel 2 `adc1`, unsigned 12-bit;
- interleaved, `lsb0-dense`.

Three samples are:

| Sample | adc0 | adc1 |
| ---: | ---: | ---: |
| 0 | `0x000` | `0xfff` |
| 1 | `0x123` | `0x456` |
| 2 | `0xabc` | `0x789` |

The six values in layout order are `000, fff, 123, 456, abc, 789`. The canonical nine-byte sample payload is:

```text
00 f0 ff 23 61 45 bc 9a 78
```

Pairwise interpretation is visible as:

```text
000 + fff -> 00 f0 ff
123 + 456 -> 23 61 45
abc + 789 -> bc 9a 78
```

For a periodic `DATA` record, `timestamp_bytes = 0`, `decoded_sample_bytes = 9`, `sample_count = 3`, and period is `1/1` when the stream's time unit is one millisecond per tick.

### 14.2 Byte-aligned 12-bit ADC

The same first two values stored with `storage_bits = 16` and packing 2 are:

```text
bc 0a 23 01
```

The high nibble of each 16-bit unsigned word is required to be zero.

### 14.3 Signed 12-bit values

Signed values `-1`, `-2048`, `2047` have 12-bit two's-complement forms `fff`, `800`, `7ff`. Dense packing produces:

```text
ff 0f 80 ff 07
```

The final high nibble is zero because it lies outside the final value.

### 14.4 Irregular temperature stream

Given `start_time_ticks = 1,000,000`, timestamp mode 2 and deltas `0, 101, 205`, the 24-byte timestamp area is:

```text
00 00 00 00 00 00 00 00
65 00 00 00 00 00 00 00
cd 00 00 00 00 00 00 00
```

If the three signed 16-bit raw temperatures are `2312`, `2314`, `2311`, their planar sample area is:

```text
08 09 0a 09 07 09
```

With scale `0.01` and unit `degC`, these represent 23.12, 23.14 and 23.11 °C. The decoded payload is the timestamp area followed immediately by the sample area, totaling 30 bytes.

### 14.5 Text message

The message `ADC overrange` is 13 UTF-8 bytes:

```text
41 44 43 20 6f 76 65 72 72 61 6e 67 65
```

It has no NUL terminator. Its byte count comes from the common header's `payload_size`. For a source component 3 and application error code 1007, the common-header flags are `0x0300`, `source_id = 3`, and `event_code = 1007`.

### 14.6 Truncated final record

Consider a valid file ending with one complete `DATA` record followed by:

```text
53 44 52 43 02 00 00 00 60 00 01 00
```

These are only the first 12 bytes of another record header. A reader reports the preceding data and ignores this incomplete record. It does not require an `END!` record.

### 14.7 Zstandard record framing

For Zstandard as the only transform, the stored payload is:

```text
10 00 01 00 00 00 00 00  <one complete Zstandard frame>
```

The first eight bytes are the transform descriptor: ID 16, version 1, flags 0, parameter size 0. `payload_size` includes both descriptor and frame. Whichever CRC placement is selected, the payload CRC covers both the descriptor and frame. `decoded_sample_bytes` is the size of the decompressed frame and excludes the descriptor.

For the compressed numeric profile, four eight-byte descriptors precede the frame:

```text
01 00 01 00 00 00 00 00
02 00 01 00 00 00 00 00
03 00 01 00 00 00 00 00
10 00 01 00 00 00 00 00
<one complete Zstandard frame>
```

### 14.8 Delta, zigzag and byte-shuffle vector

Consider an interleaved two-channel unsigned 12-bit stream with three samples:

| Sample | channel 0 | channel 1 |
| ---: | ---: | ---: |
| 0 | `0x100` | `0x200` |
| 1 | `0x101` | `0x1ff` |
| 2 | `0x0ff` | `0x201` |

With independent per-channel previous values initialized to zero, the delta values in layout order are:

```text
100 200 001 fff ffe 002
```

Interpreting the deltas as signed 12-bit values and applying zigzag gives:

```text
200 400 002 001 003 004
```

Their byte-aligned little-endian representation is:

```text
00 02  00 04  02 00  01 00  03 00  04 00
```

The record has fewer than 256 values, so it forms one final partial shuffle block. Byte shuffling produces these normative pre-Zstandard bytes:

```text
00 00 02 01 03 04  02 04 00 00 00 00
```

### 14.9 Payload CRC trailer

For a record with sequence number 7 whose stored payload is the nine ASCII bytes `123456789`, the CRC-32C is `0xe3069283`. With `PAYLOAD_CRC_IN_TRAILER` set, header `payload_crc32c` is zero and the trailer is:

```text
53 44 43 54 10 00 01 00 07 00 00 00 83 92 06 e3
```

This decodes as marker `SDCT`, trailer size 16, version 1, flags zero, sequence 7 and payload CRC `0xe3069283`.

### 14.10 Uncompressed binary item

Consider binary-item stream 20 with `content_type = application/vnd.sbg.ecom`, item index 42 and time 123456789 ticks. An uncompressed five-byte source frame is:

```text
aa 55 01 02 03
```

The `BLOB` header has `stream_id = 20`, `item_index = 42`, `time_ticks = 123456789`, `decoded_bytes = 5` and `transform_count = 0`. Its `payload_size` is 5 and the stored payload is exactly the five bytes above. No length prefix or padding occurs inside the payload.

If the same item uses Zstandard, `transform_count = 1` and the payload is:

```text
10 00 01 00 00 00 00 00  <one complete Zstandard frame>
```

The frame decompresses to the five normative source bytes. `decoded_bytes` remains 5, while `payload_size` includes the eight-byte descriptor and the frame.

### 14.11 Wrapping device clock and UTC correlation

Clock object 1 describes a device counter with monotonic-device domain, tick period `1/1000000`, `counter_bits = 32` and boot/session reset scope. Clock object 2 describes Unix UTC with tick period `1/1000000000`, `counter_bits = 0` and an externally defined Unix epoch.

Suppose two successive native device counter readings are:

```text
fffffff0
00000018
```

They are 40 microseconds apart across the 32-bit wrap. Stored source ticks MUST be unwrapped to:

```text
4294967280
4294967320
```

If the first reading correlates to Unix UTC tick `1700000000000000000`, an ideal second correlation sample is:

| source ticks | target UTC nanoseconds |
| ---: | ---: |
| 4294967280 | 1700000000000000000 |
| 4294967320 | 1700000000000040000 |

The correlation stream has `stream_kind = 4`, `source_clock_id = 1`, `target_clock_id = 2` and timestamp mode 4. Its two required signed 64-bit channels use semantics `clock.source_ticks` and `clock.target_ticks`.

### 14.12 Exact rational scale TLV

An exact scale of `1/1048576` uses channel tag 211, wire type 11 and this complete TLV:

```text
d3 00 0b 00 10 00 00 00
01 00 00 00 00 00 00 00
00 00 10 00 00 00 00 00
```

This decodes as tag `0x00d3`, `rational_i64`, flags zero, value size 16, signed numerator 1 and unsigned denominator 1048576.

## 15. Machine-readable conformance suite

The version-1 specification is accompanied by the `sdaf-conformance` suite. The suite is part of the version-1 interoperability material and contains:

- `manifest.json`, which identifies the suite version, SHA-256 of every fixture, required outcome, exercised features and decoded-value assertions;
- `valid/*.sdaf`, containing complete accepted examples;
- `corrupt/*.sdaf`, containing isolated envelope, truncation and semantic faults;
- `annotations/*.json`, containing record offsets and envelope fields for valid fixtures;
- `generate_fixtures.py`, which constructs the suite and injects each documented fault;
- `verify_fixtures.py`, which independently verifies hashes, CRCs, structure, transform decoding and expected outcomes; and
- `sdaf-v1.ksy`, the non-normative Kaitai Struct description defined in section 15.3.

The bundled suite version 1 freezes complete CRC-bearing file examples. Sections 14.1–14.12 remain normative byte-level examples; the valid fixtures add complete file and record envelopes around those encodings.

### 15.1 Valid fixtures

| Fixture | Required interpretation |
| --- | --- |
| `minimal-leading.sdaf` | Two unsigned 12-bit ADC channels, dense packing and leading payload CRC |
| `minimal-trailing.sdaf` | The same decoded ADC values using a payload CRC trailer |
| `compressed-numeric.sdaf` | The exact delta, zigzag, 256-value byte-shuffle and Zstandard profile from section 8.5 |
| `rational-symbolic.sdaf` | Exact rational scale `1/100`, a Value map and a multi-member Bitfield |
| `blob-uncompressed.sdaf` | Five decoded binary bytes stored without a transform |
| `blob-zstd.sdaf` | The same five decoded binary bytes in one independent Zstandard frame |

A reader claiming the feature exercised by a valid fixture MUST accept the file and produce the assertions in `manifest.json`. A reader that does not implement an optional transform MAY report that the record is unsupported and skip it using the common envelope; it MUST NOT report the record as corrupt solely because the transform is unsupported.

Zstandard permits multiple conforming encoded byte sequences for the same input. The bundled compressed fixture bytes and their SHA-256 hashes identify this suite release, but they do not require a writer to reproduce the same Zstandard frame. The decoded assertions, descriptor order, independent-frame requirement and decoded-size checks are normative.

### 15.2 Corrupt and truncated fixtures

The suite includes faults covering:

- file-header, record-header and leading payload CRC failures;
- trailer marker and repeated-sequence failures;
- a truncated final common header and a truncated final payload;
- a schema TLV whose declared value overruns its object;
- a checksum-valid but forbidden transform order;
- a checksum-valid Zstandard `BLOB` whose declared decoded size is wrong; and
- a checksum-valid reserved common-envelope flag.

The manifest assigns one of three outcomes:

1. `reject_file`: the file header is invalid and no records are delivered;
2. `reject_affected_record`: preceding valid records MAY be delivered, but the affected record MUST NOT be delivered as valid; or
3. `accept_prior_records_ignore_incomplete_final`: the abrupt file ending is valid, all earlier complete records remain available and the incomplete final record is ignored.

A recovery reader MAY resume at a later valid record as described in section 5.2. Recovery behavior beyond rejecting the affected record is not required by a fixture unless the manifest explicitly adds a recovery assertion in a future suite version.

### 15.3 Non-normative Kaitai Struct description

`sdaf-v1.ksy` is a strict structural description intended for inspection, reverse engineering and generated parsers. It validates version-1 constants, reserved fields, bounded record components, known type-specific headers, schema object envelopes, TLV bounds, index entries and CRC-trailer structure.

The Kaitai description is not normative. Kaitai Struct has no built-in SDAF CRC-32C process, and the grammar does not replace checks that require decoded codec output or cross-object knowledge. A conforming reader or conformance harness still MUST independently validate:

- file-header, record-header and payload CRC-32C values;
- schema object counts, unique IDs and numeric references;
- DATA layout, packing, channel and decoded-size relationships;
- transform eligibility, descriptor ordering and inverse-transform results;
- Zstandard frame output sizes; and
- index references and any application limits.

If generated Kaitai code and this specification disagree, this specification and the manifest's normative decoded assertions take precedence.

## 16. Remaining open issues before version 1.0

1. Validate the optional compressed numeric profile using real ADC captures. A microcontroller benchmark is desirable before considering the profile for any future embedded conformance requirement, but is not required for version 1 because the profile remains optional.
2. Validate the Clock, clock-correlation and `BLOB` designs using a real MRU capture containing a 32-bit timestamp wrap, UTC synchronization, diagnostic text, raw GNSS/RTCM data and at least one device restart.

## Appendix A: Embedded writer shape

A baseline C API can remain callback-based and allocation-free:

```c
typedef int (*sdaf_write_fn)(void *context, const void *data, size_t size);

typedef struct {
    sdaf_write_fn write;
    void *write_context;
    uint32_t next_sequence;
    uint8_t *chunk_buffer;
    size_t chunk_capacity;
} sdaf_writer;

int sdaf_begin(sdaf_writer *writer, const sdaf_file_info *info);
int sdaf_write_schema(sdaf_writer *writer, const sdaf_schema *schema);
int sdaf_write_data(sdaf_writer *writer, const sdaf_chunk_info *info,
                    const void *stored_payload, size_t stored_size);
int sdaf_write_text(sdaf_writer *writer, const sdaf_text_info *info,
                    const char *utf8, size_t size);
int sdaf_write_blob(sdaf_writer *writer, const sdaf_blob_info *info,
                    const void *bytes, size_t size);
int sdaf_finish(sdaf_writer *writer); /* optional index/end */
```

The API structures are not an on-disk ABI. Implementations serialize each field explicitly and MUST NOT write C structs directly.

## Appendix B: Recommended extension

`.sdaf` is the filename extension.

## Appendix C: MRU and packet-source mapping

This appendix is informative. It describes the intended mapping for a motion reference unit using a binary protocol such as SBG ECom; the core format has no vendor-specific record types.

Use one stream per stable decoded message family and set source-provenance tags when known. Fixed IMU, attitude, quaternion, navigation, GNSS position, velocity, DVL, ship-motion, odometer, depth and status messages map to `DATA`. Prefer separate scalar channels when vector components have distinct meanings, such as North/East/Down or quaternion W/X/Y/Z.

Mixed-width or floating-point MRU streams MAY use identity or Zstandard alone. The compressed numeric profile applies only when its homogeneous-integer eligibility rules are met. A writer SHOULD NOT split a semantically atomic source message into several streams solely to qualify for that compression profile.

Identify the device timebase with a Clock object. A 32-bit microsecond-since-boot counter is described with a tick period of `1/1000000`, `counter_bits = 32` and boot/session reset scope. Store unwrapped `i64` ticks. Create a new clock and dependent streams after a device restart. Represent each reliable device-to-UTC or device-to-GPS observation in a clock-correlation `DATA` stream, including validity/status and uncertainty when available.

Store packed device status words as their original unsigned integer channels and reference Bitfield and Value map objects. This preserves unknown and reserved bits while allowing a generic reader to display known solution modes, validity flags, sensor states and event-offset flags symbolically.

Static scaled integers SHOULD use exact rational scale tags when possible. If a source protocol changes scale per sample according to another status field, SDAF version 1 does not encode a conditional expression. The writer SHOULD store converted physical floating-point values and retain the status word. When exact source reconstruction is required, it SHOULD additionally retain the original source frame in a `BLOB` stream.

Variable-length GNSS, RTCM, NMEA, proprietary, session-information and paged FFT payloads map to `BLOB`. Set source-provenance and `content_type` tags, and preserve one source frame or page per BLOB record. Unknown future source messages can be retained the same way without a schema revision to SDAF itself.

Diagnostic strings map to `TEXT`, with the device component in `source_id`, its diagnostic or error number in `event_code`, and Value maps where available. Hardware-event batches MAY either remain fixed `DATA` records containing their base timestamp, offsets and status, or be expanded into individual aperiodic samples with explicit timestamps.

Variable satellite/signal lists SHOULD either be retained verbatim as `BLOB` or normalized into fixed streams. A normalized representation can use one satellite sample and one or more signal samples sharing a group index and acquisition timestamp. General nested structures are intentionally not introduced into the version-1 sample layout.
