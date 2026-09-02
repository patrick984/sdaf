#!/usr/bin/env python3
"""Verify hashes and expected outcomes for the bundled SDAF fixtures."""

from __future__ import annotations

import hashlib
import json
import struct
from pathlib import Path

import zstandard as zstd

from generate_fixtures import crc32c


ROOT = Path(__file__).resolve().parent
EXPECTED_HEADER_SIZES = {1: 48, 2: 96, 3: 64, 4: 48, 5: 64, 6: 80}


class Invalid(Exception):
    def __init__(self, reason: str):
        self.reason = reason
        super().__init__(reason)


def parse_tlvs(body: bytes) -> dict[int, list[tuple[int, bytes]]]:
    fields: dict[int, list[tuple[int, bytes]]] = {}
    offset = 0
    while offset < len(body):
        if len(body) - offset < 8:
            raise Invalid("tlv_overrun")
        tag, wire, flags, size = struct.unpack_from("<HBBI", body, offset)
        if flags != 0 or size > len(body) - offset - 8:
            raise Invalid("tlv_overrun")
        value = body[offset + 8 : offset + 8 + size]
        fields.setdefault(tag, []).append((wire, value))
        if wire in (9, 11) and (size != 16 or struct.unpack_from("<Q", value, 8)[0] == 0):
            raise Invalid("tlv_value")
        offset += 8 + size
    return fields


def one_u(fields: dict, tag: int, default: int | None = None) -> int:
    if tag not in fields:
        if default is None:
            raise Invalid("schema_required_tag")
        return default
    wire, value = fields[tag][0]
    formats = {1: "<B", 2: "<H", 3: "<I", 4: "<Q"}
    if wire not in formats or len(value) != struct.calcsize(formats[wire]):
        raise Invalid("tlv_value")
    return struct.unpack(formats[wire], value)[0]


def one_text(fields: dict, tag: int, default: str = "") -> str:
    if tag not in fields:
        return default
    wire, value = fields[tag][0]
    if wire != 7:
        raise Invalid("tlv_value")
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise Invalid("tlv_value") from exc


def parse_schema(payload: bytes, object_count: int) -> dict:
    offset = 0
    channels: list[dict] = []
    objects = []
    for _ in range(object_count):
        if len(payload) - offset < 12:
            raise Invalid("schema_object_overrun")
        kind, flags, header_size, object_size, object_id = struct.unpack_from(
            "<BBHII", payload, offset
        )
        if flags != 0 or header_size != 12 or object_size < 12 or object_id == 0:
            raise Invalid("schema_object")
        if object_size > len(payload) - offset:
            raise Invalid("schema_object_overrun")
        fields = parse_tlvs(payload[offset + 12 : offset + object_size])
        objects.append((kind, object_id, fields))
        if kind == 3:
            channels.append(
                {
                    "id": object_id,
                    "name": one_text(fields, 1),
                    "stream_id": one_u(fields, 200),
                    "logical_type": one_u(fields, 201),
                    "logical_bits": one_u(fields, 202),
                    "storage_bits": one_u(fields, 203),
                    "elements": one_u(fields, 207, 1),
                    "scale": (
                        struct.unpack("<qQ", fields[211][0][1])
                        if 211 in fields and fields[211][0][0] == 11 and len(fields[211][0][1]) == 16
                        else (1, 1)
                    ),
                }
            )
        offset += object_size
    if offset != len(payload):
        raise Invalid("schema_payload_size")
    return {"objects": objects, "channels": channels}


def descriptors(payload: bytes, count: int) -> tuple[list[int], bytes]:
    ids = []
    offset = 0
    for _ in range(count):
        if len(payload) - offset < 8:
            raise Invalid("transform_descriptor")
        transform_id, version, flags, parameter_size = struct.unpack_from(
            "<HBBI", payload, offset
        )
        if version != 1 or flags != 0 or parameter_size > len(payload) - offset - 8:
            raise Invalid("transform_descriptor")
        ids.append(transform_id)
        offset += 8 + parameter_size
    return ids, payload[offset:]


def unpack_dense(data: bytes, count: int, bits: int) -> list[int]:
    accumulator = 0
    available = 0
    offset = 0
    result = []
    mask = (1 << bits) - 1
    for _ in range(count):
        while available < bits:
            if offset >= len(data):
                raise Invalid("decoded_size")
            accumulator |= data[offset] << available
            available += 8
            offset += 1
        result.append(accumulator & mask)
        accumulator >>= bits
        available -= bits
    if any(data[offset:]) or accumulator != 0:
        raise Invalid("padding_bits")
    return result


def decode_numeric_profile(
    encoded: bytes, channels: list[dict], stream_id: int, sample_count: int, timestamp_bytes: int
) -> list[int]:
    stream_channels = sorted(
        (channel for channel in channels if channel["stream_id"] == stream_id),
        key=lambda channel: channel["id"],
    )
    if not stream_channels:
        raise Invalid("schema_reference")
    widths = {channel["logical_bits"] for channel in stream_channels}
    types = {channel["logical_type"] for channel in stream_channels}
    if len(widths) != 1 or not types.issubset({1, 2}):
        raise Invalid("profile_eligibility")
    bits = widths.pop()
    lanes = sum(channel["elements"] for channel in stream_channels)
    value_count = sample_count * lanes
    width = (bits + 7) // 8
    try:
        intermediate = zstd.ZstdDecompressor().decompress(encoded)
    except zstd.ZstdError as exc:
        raise Invalid("zstandard") from exc
    expected = timestamp_bytes + value_count * width
    if len(intermediate) != expected:
        raise Invalid("decoded_size")
    sample_area = intermediate[timestamp_bytes:]

    zigzagged = []
    source_offset = 0
    for first in range(0, value_count, 256):
        block_count = min(256, value_count - first)
        block_size = block_count * width
        block = sample_area[source_offset : source_offset + block_size]
        if len(block) != block_size:
            raise Invalid("decoded_size")
        source_offset += block_size
        for value_index in range(block_count):
            value = 0
            for byte_lane in range(width):
                value |= block[byte_lane * block_count + value_index] << (8 * byte_lane)
            if value >> bits:
                raise Invalid("padding_bits")
            zigzagged.append(value)

    mask = (1 << bits) - 1
    previous = [0] * lanes
    values = []
    for index, zigzag in enumerate(zigzagged):
        delta_signed = zigzag // 2 if zigzag % 2 == 0 else -(zigzag // 2) - 1
        lane = index % lanes
        value = (previous[lane] + delta_signed) & mask
        previous[lane] = value
        values.append(value)
    return values


def decode_uncompressed(
    payload: bytes,
    channels: list[dict],
    stream_id: int,
    sample_count: int,
    timestamp_bytes: int,
    layout: int,
    packing: int,
) -> tuple[list[int], dict[str, int | float]]:
    stream_channels = sorted(
        (channel for channel in channels if channel["stream_id"] == stream_id),
        key=lambda channel: channel["id"],
    )
    sample_area = payload[timestamp_bytes:]
    value_count = sample_count * sum(channel["elements"] for channel in stream_channels)
    values: list[int] = []
    named: dict[str, int | float] = {}
    if packing == 1:
        widths = {channel["storage_bits"] for channel in stream_channels}
        if len(widths) != 1:
            raise Invalid("profile_eligibility")
        values = unpack_dense(sample_area, value_count, widths.pop())
        return values, named

    ordered_fields = []
    if layout == 1:
        for _ in range(sample_count):
            for channel in stream_channels:
                ordered_fields.extend([channel] * channel["elements"])
    else:
        for channel in stream_channels:
            ordered_fields.extend([channel] * (sample_count * channel["elements"]))
    offset = 0
    for channel in ordered_fields:
        width = (channel["storage_bits"] + 7) // 8
        if len(sample_area) - offset < width:
            raise Invalid("decoded_size")
        raw = int.from_bytes(sample_area[offset : offset + width], "little")
        offset += width
        mask = (1 << channel["storage_bits"]) - 1
        raw &= mask
        if channel["logical_type"] == 2 and raw & (1 << (channel["storage_bits"] - 1)):
            raw -= 1 << channel["storage_bits"]
        values.append(raw)
        if channel["name"]:
            named[f"raw_{channel['name']}"] = raw
            numerator, denominator = channel["scale"]
            if (numerator, denominator) != (1, 1):
                named[f"physical_{channel['name']}"] = raw * numerator / denominator
    if offset != len(sample_area):
        raise Invalid("decoded_size")
    return values, named


def inspect(path: Path) -> tuple[str, dict]:
    data = path.read_bytes()
    if len(data) < 64:
        return "truncated_file_header", {}
    if data[:8] != b"SDAF\r\n\x1a\n":
        raise Invalid("file_magic")
    major, minor, header_size = struct.unpack_from("<BBH", data, 8)
    if major != 1 or minor != 0 or header_size != 64:
        raise Invalid("file_header")
    stored_header_crc = struct.unpack_from("<I", data, 56)[0]
    header = bytearray(data[:64])
    struct.pack_into("<I", header, 56, 0)
    if crc32c(header) != stored_header_crc:
        raise Invalid("file_header_crc")

    offset = 64
    schemas: dict[tuple[int, int], dict] = {}
    decoded: dict = {}
    valid_records = 0
    while offset < len(data):
        if len(data) - offset < 32:
            return "truncated_final_header", {"valid_records": valid_records}
        if data[offset : offset + 4] != b"SDRC":
            raise Invalid("record_marker")
        record_type, flags, record_header_size = struct.unpack_from("<HHH", data, offset + 4)
        version = data[offset + 10]
        reserved = data[offset + 11]
        sequence = struct.unpack_from("<I", data, offset + 12)[0]
        payload_size = struct.unpack_from("<Q", data, offset + 16)[0]
        stored_header_crc = struct.unpack_from("<I", data, offset + 24)[0]
        stored_payload_crc = struct.unpack_from("<I", data, offset + 28)[0]
        if flags & 0x00FE:
            raise Invalid("reserved_flag")
        if version != 1 or reserved != 0 or record_header_size < 32:
            raise Invalid("record_header")
        expected_header_size = EXPECTED_HEADER_SIZES.get(record_type)
        if expected_header_size is not None and record_header_size != expected_header_size:
            raise Invalid("record_header_size")
        total_size = record_header_size + payload_size + (16 if flags & 1 else 0)
        if total_size > len(data) - offset:
            return "truncated_final_payload", {"valid_records": valid_records}

        header = bytearray(data[offset : offset + record_header_size])
        struct.pack_into("<I", header, 24, 0)
        if crc32c(header) != stored_header_crc:
            raise Invalid("record_header_crc")
        payload_start = offset + record_header_size
        payload = data[payload_start : payload_start + payload_size]
        actual_payload_crc = crc32c(payload)
        if flags & 1:
            if stored_payload_crc != 0:
                raise Invalid("payload_crc_placement")
            trailer = data[payload_start + payload_size : payload_start + payload_size + 16]
            marker, size, trailer_version, trailer_flags, trailer_sequence, trailer_crc = struct.unpack(
                "<4sHBBII", trailer
            )
            if marker != b"SDCT":
                raise Invalid("trailer_marker")
            if size != 16 or trailer_version != 1 or trailer_flags != 0:
                raise Invalid("trailer_header")
            if trailer_sequence != sequence:
                raise Invalid("trailer_sequence")
            if trailer_crc != actual_payload_crc:
                raise Invalid("payload_crc")
        elif stored_payload_crc != actual_payload_crc:
            raise Invalid("payload_crc")

        if record_type == 1:
            schema_id, revision, object_count, schema_reserved = struct.unpack_from(
                "<IIII", data, offset + 32
            )
            if schema_id == 0 or revision == 0 or schema_reserved != 0:
                raise Invalid("schema_header")
            schemas[(schema_id, revision)] = parse_schema(payload, object_count)
        elif record_type == 2:
            fields = struct.unpack_from("<IIIIQqQQBBBBIQ", data, offset + 32)
            schema_id, revision, stream_id, sample_count = fields[:4]
            timestamp_mode, layout, packing, transform_count = fields[8:12]
            timestamp_bytes, decoded_bytes = fields[12:14]
            if timestamp_mode not in (1, 2, 3, 4) or layout not in (1, 2) or packing not in (1, 2):
                raise Invalid("data_header")
            schema = schemas.get((schema_id, revision))
            if schema is None:
                raise Invalid("schema_reference")
            ids, encoded = descriptors(payload, transform_count)
            if transform_count:
                if ids != [1, 2, 3, 16]:
                    raise Invalid("transform_order")
                values = decode_numeric_profile(
                    encoded, schema["channels"], stream_id, sample_count, timestamp_bytes
                )
                decoded["decoded_values"] = values
            else:
                if len(encoded) != decoded_bytes:
                    raise Invalid("decoded_size")
                values, named = decode_uncompressed(
                    encoded,
                    schema["channels"],
                    stream_id,
                    sample_count,
                    timestamp_bytes,
                    layout,
                    packing,
                )
                decoded["decoded_values"] = values
                decoded.update(named)
        elif record_type == 6:
            schema_id, revision, stream_id, blob_reserved, item_index, time_ticks, decoded_bytes, count = struct.unpack_from(
                "<IIIIQqQB", data, offset + 32
            )
            if blob_reserved != 0 or any(data[offset + 73 : offset + 80]):
                raise Invalid("blob_header")
            ids, encoded = descriptors(payload, count)
            if ids == []:
                item = encoded
            elif ids == [16]:
                try:
                    item = zstd.ZstdDecompressor().decompress(encoded)
                except zstd.ZstdError as exc:
                    raise Invalid("zstandard") from exc
            else:
                raise Invalid("transform_order")
            if len(item) != decoded_bytes:
                raise Invalid("decoded_size")
            decoded["decoded_hex"] = item.hex()

        valid_records += 1
        offset += total_size
    return "accept", {"valid_records": valid_records, **decoded}


def main() -> None:
    manifest = json.loads((ROOT / "manifest.json").read_text(encoding="utf-8"))
    failures = []
    for fixture in manifest["fixtures"]:
        path = ROOT / fixture["path"]
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != fixture["sha256"]:
            failures.append(f"{fixture['path']}: SHA-256 mismatch")
            continue
        try:
            outcome, details = inspect(path)
        except Invalid as exc:
            outcome, details = exc.reason, {}

        expected = fixture["expected"]
        if expected == "accept":
            ok = outcome == "accept"
            for key, value in fixture.get("assertions", {}).items():
                actual = details.get(key)
                if isinstance(value, float) and isinstance(actual, (float, int)):
                    matches = abs(actual - value) < 1e-12
                else:
                    matches = actual == value
                if not matches:
                    ok = False
        elif expected == "reject_file":
            ok = outcome == fixture["failure"]
        elif expected == "accept_prior_records_ignore_incomplete_final":
            ok = outcome == fixture["failure"] and details.get("valid_records", 0) >= 1
        else:
            ok = outcome == fixture["failure"]
        if not ok:
            failures.append(
                f"{fixture['path']}: expected {expected}/{fixture.get('failure')}, got {outcome} {details}"
            )

    if failures:
        raise SystemExit("\n".join(failures))
    print(f"verified {len(manifest['fixtures'])} SDAF fixtures")


if __name__ == "__main__":
    main()
