meta:
  id: sdaf_v1
  title: Self-Describing Data Acquisition Format version 1
  file-extension: sdaf
  endian: le
  license: CC0-1.0
doc: |
  Non-normative strict structural description of SDAF version 1. CRC-32C,
  cross-object schema references, transform semantics and decompressed-size
  validation remain the responsibility of the calling inspection tool.
seq:
  - id: file_header
    type: file_header
  - id: records
    type: record
    repeat: eos
types:
  file_header:
    seq:
      - id: magic
        contents: [0x53, 0x44, 0x41, 0x46, 0x0d, 0x0a, 0x1a, 0x0a]
      - id: major
        type: u1
        valid: 1
      - id: minor
        type: u1
        valid: 0
      - id: header_size
        type: u2
        valid: 64
      - id: byte_order_check
        type: u4
        valid: 0x12345678
      - id: feature_flags
        type: u4
        valid: 0
      - id: reserved_0
        type: u4
        valid: 0
      - id: created_unix_ns
        type: s8
      - id: file_uuid
        size: 16
      - id: first_record_offset
        type: u8
        valid: 64
      - id: header_crc32c
        type: u4
      - id: reserved_1
        type: u4
        valid: 0

  record:
    seq:
      - id: marker
        contents: [0x53, 0x44, 0x52, 0x43]
      - id: record_type
        type: u2
      - id: flags
        type: u2
        valid:
          expr: (_ & 0x00fe) == 0
      - id: header_size
        type: u2
        valid:
          min: 32
      - id: record_version
        type: u1
        valid: 1
      - id: reserved
        type: u1
        valid: 0
      - id: sequence
        type: u4
      - id: payload_size
        type: u8
      - id: header_crc32c
        type: u4
      - id: payload_crc32c
        type: u4
        valid:
          expr: ((flags & 1) == 0) or (_ == 0)
      - id: type_header
        size: header_size - 32
        type:
          switch-on: record_type
          cases:
            '1': schema_header
            '2': data_header
            '3': text_header
            '4': index_header
            '5': end_header
            '6': blob_header
      - id: payload
        size: payload_size
        type:
          switch-on: record_type
          cases:
            '1': schema_payload
            '4': index_payload
            '3': utf8_payload
            '0x7fff': utf8_payload
      - id: trailer
        type: crc_trailer
        if: (flags & 1) != 0

  crc_trailer:
    seq:
      - id: marker
        contents: [0x53, 0x44, 0x43, 0x54]
      - id: trailer_size
        type: u2
        valid: 16
      - id: trailer_version
        type: u1
        valid: 1
      - id: flags
        type: u1
        valid: 0
      - id: sequence
        type: u4
        valid:
          expr: _ == _parent.sequence
      - id: payload_crc32c
        type: u4

  schema_header:
    seq:
      - id: schema_id
        type: u4
        valid:
          min: 1
      - id: schema_revision
        type: u4
        valid:
          min: 1
      - id: object_count
        type: u4
      - id: reserved
        type: u4
        valid: 0

  data_header:
    seq:
      - id: schema_id
        type: u4
        valid:
          min: 1
      - id: schema_revision
        type: u4
        valid:
          min: 1
      - id: stream_id
        type: u4
        valid:
          min: 1
      - id: sample_count
        type: u4
      - id: first_sample_index
        type: u8
      - id: start_time_ticks
        type: s8
      - id: period_numerator
        type: u8
      - id: period_denominator
        type: u8
      - id: timestamp_mode
        type: u1
        valid:
          any-of: [1, 2, 3, 4]
      - id: layout
        type: u1
        valid:
          any-of: [1, 2]
      - id: packing
        type: u1
        valid:
          any-of: [1, 2]
      - id: transform_count
        type: u1
      - id: timestamp_bytes
        type: u4
      - id: decoded_sample_bytes
        type: u8

  text_header:
    seq:
      - id: schema_id
        type: u4
      - id: stream_id
        type: u4
      - id: time_ticks
        type: s8
      - id: severity
        type: u1
        valid:
          max: 6
      - id: encoding
        type: u1
        valid: 1
      - id: reserved_0
        type: u2
        valid: 0
      - id: source_id
        type: u4
      - id: event_code
        type: s4
      - id: schema_revision
        type: u4

  blob_header:
    seq:
      - id: schema_id
        type: u4
        valid:
          min: 1
      - id: schema_revision
        type: u4
        valid:
          min: 1
      - id: stream_id
        type: u4
        valid:
          min: 1
      - id: reserved_0
        type: u4
        valid: 0
      - id: item_index
        type: u8
      - id: time_ticks
        type: s8
      - id: decoded_bytes
        type: u8
      - id: transform_count
        type: u1
        valid:
          any-of: [0, 1]
      - id: reserved_1
        size: 7
        contents: [0, 0, 0, 0, 0, 0, 0]

  index_header:
    seq:
      - id: entry_count
        type: u4
      - id: reserved
        size: 12
        contents: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]

  end_header:
    seq:
      - id: total_record_count
        type: u8
      - id: total_data_record_count
        type: u8
      - id: last_index_offset
        type: u8
      - id: reserved
        size: 8
        contents: [0, 0, 0, 0, 0, 0, 0, 0]

  schema_payload:
    seq:
      - id: objects
        type: schema_object
        repeat: eos

  schema_object:
    seq:
      - id: object_kind
        type: u1
        valid:
          any-of: [1, 2, 3, 4, 5, 6]
      - id: flags
        type: u1
        valid: 0
      - id: header_size
        type: u2
        valid: 12
      - id: object_size
        type: u4
        valid:
          min: 12
      - id: object_id
        type: u4
        valid:
          min: 1
      - id: body
        size: object_size - 12
        type: tlv_list

  tlv_list:
    seq:
      - id: entries
        type: tlv
        repeat: eos

  tlv:
    seq:
      - id: tag
        type: u2
      - id: wire_type
        type: u1
      - id: flags
        type: u1
        valid: 0
      - id: value_size
        type: u4
      - id: value
        size: value_size

  index_payload:
    seq:
      - id: entries
        type: index_entry
        repeat: eos

  index_entry:
    seq:
      - id: record_offset
        type: u8
      - id: sequence
        type: u4
      - id: stream_id
        type: u4
      - id: first_sample_index
        type: u8
      - id: sample_count
        type: u4
      - id: reserved
        type: u4
        valid: 0
      - id: first_time_ticks
        type: s8
      - id: last_time_ticks
        type: s8

  utf8_payload:
    seq:
      - id: text
        type: str
        encoding: UTF-8
        size-eos: true
