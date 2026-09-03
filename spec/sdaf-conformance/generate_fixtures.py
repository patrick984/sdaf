#!/usr/bin/env python3
"""Generate the SDAF version-1 conformance fixture suite.

The generated files are deterministic for a given Zstandard implementation.
Compressed-frame bytes are not themselves normative; the manifest records the
SHA-256 hashes of the bundled suite and the required decoded values.
"""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

import zstandard as zstd


ROOT = Path(__file__).resolve().parent
VALID = ROOT / "valid"
CORRUPT = ROOT / "corrupt"
ANNOTATIONS = ROOT / "annotations"

MAGIC = b"SDAF\r\n\x1a\n"
RECORD_MARKER = b"SDRC"
TRAILER_MARKER = b"SDCT"
CREATED_NS = 1_700_000_000_000_000_000
FILE_UUID = bytes.fromhex("00112233445566778899aabbccddeeff")

SCMA = 0x0001
DATA = 0x0002
BLOB = 0x0006


def crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for octet in data:
        crc ^= octet
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


assert crc32c(b"123456789") == 0xE3069283


def file_header() -> bytes:
    header = bytearray(64)
    struct.pack_into(
        "<8sBBHII I q 16s Q I I",
        header,
        0,
        MAGIC,
        1,
        0,
        64,
        0x12345678,
        0,
        0,
        CREATED_NS,
        FILE_UUID,
        64,
        0,
        0,
    )
    struct.pack_into("<I", header, 56, crc32c(header))
    return bytes(header)


def record(
    record_type: int,
    sequence: int,
    type_header: bytes,
    payload: bytes,
    *,
    trailer_crc: bool = False,
    type_flags: int = 0,
) -> bytes:
    flags = type_flags | (1 if trailer_crc else 0)
    header_size = 32 + len(type_header)
    common = bytearray(
        struct.pack(
            "<4sHHHBBIQII",
            RECORD_MARKER,
            record_type,
            flags,
            header_size,
            1,
            0,
            sequence,
            len(payload),
            0,
            0 if trailer_crc else crc32c(payload),
        )
    )
    header = common + type_header
    struct.pack_into("<I", header, 24, crc32c(header))
    trailer = b""
    if trailer_crc:
        trailer = struct.pack(
            "<4sHBBII", TRAILER_MARKER, 16, 1, 0, sequence, crc32c(payload)
        )
    return bytes(header) + payload + trailer


def tlv(tag: int, wire_type: int, value: bytes) -> bytes:
    return struct.pack("<HBBI", tag, wire_type, 0, len(value)) + value


def u8(tag: int, value: int) -> bytes:
    return tlv(tag, 1, struct.pack("<B", value))


def u16(tag: int, value: int) -> bytes:
    return tlv(tag, 2, struct.pack("<H", value))


def u32(tag: int, value: int) -> bytes:
    return tlv(tag, 3, struct.pack("<I", value))


def text(tag: int, value: str) -> bytes:
    return tlv(tag, 7, value.encode("utf-8"))


def rational_u64(tag: int, numerator: int, denominator: int) -> bytes:
    return tlv(tag, 9, struct.pack("<QQ", numerator, denominator))


def rational_i64(tag: int, numerator: int, denominator: int) -> bytes:
    return tlv(tag, 11, struct.pack("<qQ", numerator, denominator))


def schema_object(kind: int, object_id: int, *fields: bytes) -> bytes:
    body = b"".join(fields)
    return struct.pack("<BBHII", kind, 0, 12, 12 + len(body), object_id) + body


def stream_object(
    stream_id: int,
    name: str,
    channel_count: int,
    *,
    stream_kind: int = 1,
    content_type: str | None = None,
) -> bytes:
    fields = [
        text(1, name),
        u8(100, 1),
        rational_u64(101, 1, 1000),
        u8(103, 1),
        u32(104, channel_count),
        u8(106, stream_kind),
    ]
    if content_type is not None:
        fields.append(text(107, content_type))
    return schema_object(2, stream_id, *fields)


def channel_object(
    channel_id: int,
    stream_id: int,
    name: str,
    logical_type: int,
    bits: int,
    *,
    unit: str | None = None,
    bitfield_id: int | None = None,
    scale: tuple[int, int] | None = None,
) -> bytes:
    fields = [
        text(1, name),
        u32(200, stream_id),
        u8(201, logical_type),
        u16(202, bits),
        u16(203, bits),
    ]
    if unit is not None:
        fields.append(text(204, unit))
    if bitfield_id is not None:
        fields.append(u32(210, bitfield_id))
    if scale is not None:
        fields.append(rational_i64(211, scale[0], scale[1]))
    return schema_object(3, channel_id, *fields)


def value_map_entry(raw: int, name: str, description: str = "") -> bytes:
    name_bytes = name.encode("utf-8")
    desc_bytes = description.encode("utf-8")
    return struct.pack("<QHH", raw, len(name_bytes), len(desc_bytes)) + name_bytes + desc_bytes


def value_map_object(object_id: int) -> bytes:
    fields = [
        text(1, "GNSS fix status"),
        tlv(400, 8, value_map_entry(0, "no_fix")),
        tlv(400, 8, value_map_entry(1, "fix_2d")),
        tlv(400, 8, value_map_entry(2, "fix_3d")),
        tlv(400, 8, value_map_entry(3, "sbas")),
    ]
    return schema_object(5, object_id, *fields)


def bitfield_member(
    offset: int,
    width: int,
    map_id: int,
    name: str,
    description: str = "",
    *,
    reserved: bool = False,
) -> bytes:
    name_bytes = name.encode("utf-8")
    desc_bytes = description.encode("utf-8")
    return (
        struct.pack(
            "<HHIHHH",
            offset,
            width,
            map_id,
            1 if reserved else 0,
            len(name_bytes),
            len(desc_bytes),
        )
        + name_bytes
        + desc_bytes
    )


def bitfield_object(object_id: int) -> bytes:
    return schema_object(
        6,
        object_id,
        text(1, "navigation status"),
        u16(500, 16),
        tlv(501, 8, bitfield_member(0, 1, 0, "sensor_failure")),
        tlv(501, 8, bitfield_member(4, 3, 1, "gnss_fix")),
        tlv(501, 8, bitfield_member(7, 9, 0, "reserved", reserved=True)),
    )


def schema_record(sequence: int, schema_id: int, objects: list[bytes]) -> bytes:
    payload = b"".join(objects)
    type_header = struct.pack("<IIII", schema_id, 1, len(objects), 0)
    return record(SCMA, sequence, type_header, payload)


def data_header(
    schema_id: int,
    stream_id: int,
    sample_count: int,
    decoded_bytes: int,
    *,
    timestamp_mode: int = 1,
    layout: int = 1,
    packing: int = 1,
    transform_count: int = 0,
) -> bytes:
    if timestamp_mode == 1:
        start_time, period_num, period_den = 1_000_000, 1, 1
    else:
        start_time, period_num, period_den = 0, 0, 0
    return struct.pack(
        "<IIIIQqQQBBBBIQ",
        schema_id,
        1,
        stream_id,
        sample_count,
        0,
        start_time,
        period_num,
        period_den,
        timestamp_mode,
        layout,
        packing,
        transform_count,
        0,
        decoded_bytes,
    )


def blob_header(decoded_bytes: int, *, item_index: int, transform_count: int) -> bytes:
    return struct.pack(
        "<IIIIQqQB7s",
        4,
        1,
        20,
        0,
        item_index,
        123_456_789,
        decoded_bytes,
        transform_count,
        b"\0" * 7,
    )


def descriptor(transform_id: int) -> bytes:
    return struct.pack("<HBBI", transform_id, 1, 0, 0)


def dense_pack(values: list[int], bits: int) -> bytes:
    accumulator = 0
    used = 0
    output = bytearray()
    mask = (1 << bits) - 1
    for value in values:
        accumulator |= (value & mask) << used
        used += bits
        while used >= 8:
            output.append(accumulator & 0xFF)
            accumulator >>= 8
            used -= 8
    if used:
        output.append(accumulator & 0xFF)
    return bytes(output)


def compressed_numeric_payload(values: list[int], bits: int, lanes: int) -> bytes:
    mask = (1 << bits) - 1
    previous = [0] * lanes
    zigzagged: list[int] = []
    for index, value in enumerate(values):
        lane = index % lanes
        delta = (value - previous[lane]) & mask
        previous[lane] = value
        signed = delta if delta < (1 << (bits - 1)) else delta - (1 << bits)
        zigzagged.append(2 * signed if signed >= 0 else -2 * signed - 1)

    width = (bits + 7) // 8
    shuffled = bytearray()
    for first in range(0, len(zigzagged), 256):
        block = zigzagged[first : first + 256]
        encoded = [value.to_bytes(width, "little") for value in block]
        for byte_lane in range(width):
            shuffled.extend(value[byte_lane] for value in encoded)

    compressor = zstd.ZstdCompressor(
        level=3, write_content_size=True, write_checksum=True, write_dict_id=False
    )
    frame = compressor.compress(bytes(shuffled))
    return b"".join(descriptor(value) for value in (1, 2, 3, 16)) + frame


def minimal_file(*, trailer_crc: bool) -> bytes:
    objects = [
        stream_object(1, "adc", 2),
        channel_object(1, 1, "adc0", 1, 12, unit="code"),
        channel_object(2, 1, "adc1", 1, 12, unit="code"),
    ]
    samples = bytes.fromhex("00 f0 ff 23 61 45 bc 9a 78")
    return (
        file_header()
        + schema_record(0, 1, objects)
        + record(
            DATA,
            1,
            data_header(1, 1, 3, len(samples)),
            samples,
            trailer_crc=trailer_crc,
        )
    )


def compressed_numeric_file() -> bytes:
    objects = [
        stream_object(1, "compressed_adc", 2),
        channel_object(1, 1, "adc0", 1, 12, unit="code"),
        channel_object(2, 1, "adc1", 1, 12, unit="code"),
    ]
    values = [0x100, 0x200, 0x101, 0x1FF, 0x0FF, 0x201]
    decoded = dense_pack(values, 12)
    payload = compressed_numeric_payload(values, 12, 2)
    return (
        file_header()
        + schema_record(0, 2, objects)
        + record(
            DATA,
            1,
            data_header(2, 1, 3, len(decoded), transform_count=4),
            payload,
        )
    )


def rational_symbolic_file() -> bytes:
    objects = [
        stream_object(2, "navigation", 2),
        channel_object(1, 2, "status", 1, 16, bitfield_id=1),
        channel_object(2, 2, "temperature", 2, 16, unit="degC", scale=(1, 100)),
        value_map_object(1),
        bitfield_object(1),
    ]
    payload = struct.pack("<Hh", 0x0031, 2312)
    return (
        file_header()
        + schema_record(0, 3, objects)
        + record(
            DATA,
            1,
            data_header(
                3,
                2,
                1,
                len(payload),
                timestamp_mode=4,
                packing=2,
            ),
            payload,
        )
    )


def blob_file(*, compressed: bool) -> bytes:
    objects = [
        stream_object(
            20,
            "source_frames",
            0,
            stream_kind=3,
            content_type="application/vnd.sbg.ecom",
        )
    ]
    source = bytes.fromhex("aa 55 01 02 03")
    if compressed:
        compressor = zstd.ZstdCompressor(
            level=3, write_content_size=True, write_checksum=True, write_dict_id=False
        )
        payload = descriptor(16) + compressor.compress(source)
    else:
        payload = source
    return (
        file_header()
        + schema_record(0, 4, objects)
        + record(
            BLOB,
            1,
            blob_header(len(source), item_index=42, transform_count=1 if compressed else 0),
            payload,
        )
    )


def record_offsets(data: bytes) -> list[int]:
    offsets: list[int] = []
    offset = 64
    while offset + 32 <= len(data):
        if data[offset : offset + 4] != RECORD_MARKER:
            break
        header_size = struct.unpack_from("<H", data, offset + 8)[0]
        flags = struct.unpack_from("<H", data, offset + 6)[0]
        payload_size = struct.unpack_from("<Q", data, offset + 16)[0]
        offsets.append(offset)
        offset += header_size + payload_size + (16 if flags & 1 else 0)
    return offsets


def repair_header_crc(data: bytearray, offset: int) -> None:
    header_size = struct.unpack_from("<H", data, offset + 8)[0]
    struct.pack_into("<I", data, offset + 24, 0)
    struct.pack_into("<I", data, offset + 24, crc32c(data[offset : offset + header_size]))


def repair_payload_and_header_crc(data: bytearray, offset: int) -> None:
    header_size = struct.unpack_from("<H", data, offset + 8)[0]
    payload_size = struct.unpack_from("<Q", data, offset + 16)[0]
    payload = data[offset + header_size : offset + header_size + payload_size]
    struct.pack_into("<I", data, offset + 28, crc32c(payload))
    repair_header_crc(data, offset)


def malformed_tlv_file() -> bytes:
    malformed_object = struct.pack("<BBHII", 2, 0, 12, 20, 1)
    malformed_object += struct.pack("<HBBI", 1, 7, 0, 10)
    scma = record(SCMA, 0, struct.pack("<IIII", 9, 1, 1, 0), malformed_object)
    return file_header() + scma


def annotate(data: bytes) -> dict:
    records = []
    for offset in record_offsets(data):
        record_type, flags, header_size = struct.unpack_from("<HHH", data, offset + 4)
        sequence = struct.unpack_from("<I", data, offset + 12)[0]
        payload_size = struct.unpack_from("<Q", data, offset + 16)[0]
        records.append(
            {
                "offset": offset,
                "record_type": record_type,
                "sequence": sequence,
                "flags": flags,
                "header_size": header_size,
                "payload_size": payload_size,
                "payload_crc_placement": "trailer" if flags & 1 else "header",
                "stored_size": header_size + payload_size + (16 if flags & 1 else 0),
            }
        )
    return {
        "file_size": len(data),
        "header_crc32c": f"0x{struct.unpack_from('<I', data, 56)[0]:08x}",
        "records": records,
    }


def write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main() -> None:
    for directory in (VALID, CORRUPT, ANNOTATIONS):
        directory.mkdir(parents=True, exist_ok=True)

    valid_files = {
        "minimal-leading.sdaf": (
            minimal_file(trailer_crc=False),
            ["dense_12_bit", "leading_payload_crc"],
            {"decoded_values": [0, 4095, 291, 1110, 2748, 1929]},
        ),
        "minimal-trailing.sdaf": (
            minimal_file(trailer_crc=True),
            ["dense_12_bit", "trailing_payload_crc"],
            {"decoded_values": [0, 4095, 291, 1110, 2748, 1929]},
        ),
        "compressed-numeric.sdaf": (
            compressed_numeric_file(),
            ["delta", "zigzag", "byte_shuffle_256", "zstandard"],
            {"decoded_values": [256, 512, 257, 511, 255, 513]},
        ),
        "rational-symbolic.sdaf": (
            rational_symbolic_file(),
            ["rational_scale", "value_map", "bitfield"],
            {"raw_status": 49, "raw_temperature": 2312, "physical_temperature": 23.12},
        ),
        "blob-uncompressed.sdaf": (
            blob_file(compressed=False),
            ["blob", "identity"],
            {"decoded_hex": "aa55010203"},
        ),
        "blob-zstd.sdaf": (
            blob_file(compressed=True),
            ["blob", "zstandard"],
            {"decoded_hex": "aa55010203"},
        ),
    }

    for name, (data, _, _) in valid_files.items():
        write(VALID / name, data)
        (ANNOTATIONS / f"{name}.json").write_text(
            json.dumps(annotate(data), indent=2) + "\n", encoding="utf-8"
        )

    corrupt_files: dict[str, tuple[bytes, str]] = {}

    data = bytearray(valid_files["minimal-leading.sdaf"][0])
    data[56] ^= 0x01
    corrupt_files["bad-file-header-crc.sdaf"] = (bytes(data), "file_header_crc")

    data = bytearray(valid_files["minimal-leading.sdaf"][0])
    data[64 + 24] ^= 0x01
    corrupt_files["bad-record-header-crc.sdaf"] = (bytes(data), "record_header_crc")

    data = bytearray(valid_files["minimal-leading.sdaf"][0])
    data_offset = record_offsets(data)[1]
    payload_offset = data_offset + struct.unpack_from("<H", data, data_offset + 8)[0]
    data[payload_offset] ^= 0x01
    corrupt_files["bad-leading-payload-crc.sdaf"] = (bytes(data), "payload_crc")

    data = bytearray(valid_files["minimal-trailing.sdaf"][0])
    data_offset = record_offsets(data)[1]
    header_size = struct.unpack_from("<H", data, data_offset + 8)[0]
    payload_size = struct.unpack_from("<Q", data, data_offset + 16)[0]
    trailer_offset = data_offset + header_size + payload_size
    data[trailer_offset] ^= 0x01
    corrupt_files["bad-trailer-marker.sdaf"] = (bytes(data), "trailer_marker")

    data = bytearray(valid_files["minimal-trailing.sdaf"][0])
    data_offset = record_offsets(data)[1]
    header_size = struct.unpack_from("<H", data, data_offset + 8)[0]
    payload_size = struct.unpack_from("<Q", data, data_offset + 16)[0]
    trailer_offset = data_offset + header_size + payload_size
    struct.pack_into("<I", data, trailer_offset + 8, 99)
    corrupt_files["bad-trailer-sequence.sdaf"] = (bytes(data), "trailer_sequence")

    data = valid_files["minimal-leading.sdaf"][0] + RECORD_MARKER + b"\x02\x00\x00\x00\x60\x00\x01\x00"
    corrupt_files["truncated-record-header.sdaf"] = (data, "truncated_final_header")

    data = valid_files["minimal-leading.sdaf"][0][:-2]
    corrupt_files["truncated-payload.sdaf"] = (data, "truncated_final_payload")

    corrupt_files["malformed-tlv-overrun.sdaf"] = (malformed_tlv_file(), "tlv_overrun")

    data = bytearray(valid_files["compressed-numeric.sdaf"][0])
    data_offset = record_offsets(data)[1]
    payload_offset = data_offset + struct.unpack_from("<H", data, data_offset + 8)[0]
    struct.pack_into("<H", data, payload_offset, 2)
    struct.pack_into("<H", data, payload_offset + 8, 1)
    repair_payload_and_header_crc(data, data_offset)
    corrupt_files["invalid-transform-order.sdaf"] = (bytes(data), "transform_order")

    data = bytearray(valid_files["blob-zstd.sdaf"][0])
    blob_offset = record_offsets(data)[1]
    struct.pack_into("<Q", data, blob_offset + 64, 6)
    repair_header_crc(data, blob_offset)
    corrupt_files["blob-decoded-size-mismatch.sdaf"] = (bytes(data), "decoded_size")

    data = bytearray(valid_files["minimal-leading.sdaf"][0])
    data_offset = record_offsets(data)[1]
    flags = struct.unpack_from("<H", data, data_offset + 6)[0]
    struct.pack_into("<H", data, data_offset + 6, flags | 0x0002)
    repair_header_crc(data, data_offset)
    corrupt_files["reserved-envelope-flag.sdaf"] = (bytes(data), "reserved_flag")

    for name, (data, _) in corrupt_files.items():
        write(CORRUPT / name, data)

    fixture_entries = []
    for name, (data, features, assertions) in valid_files.items():
        fixture_entries.append(
            {
                "path": f"valid/{name}",
                "sha256": hashlib.sha256(data).hexdigest(),
                "expected": "accept",
                "features": features,
                "assertions": assertions,
            }
        )
    for name, (data, reason) in corrupt_files.items():
        if reason == "file_header_crc":
            expected = "reject_file"
        elif reason.startswith("truncated_final_"):
            expected = "accept_prior_records_ignore_incomplete_final"
        else:
            expected = "reject_affected_record"
        fixture_entries.append(
            {
                "path": f"corrupt/{name}",
                "sha256": hashlib.sha256(data).hexdigest(),
                "expected": expected,
                "failure": reason,
            }
        )

    manifest = {
        "suite": "SDAF version-1 conformance fixtures",
        "suite_version": 1,
        "format_major": 1,
        "format_minor": 0,
        "crc": "CRC-32C Castagnoli",
        "compressed_frame_note": "Zstandard frame bytes are not normative; decoded assertions are normative.",
        "fixtures": fixture_entries,
    }
    (ROOT / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
