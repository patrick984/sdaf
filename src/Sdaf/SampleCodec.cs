namespace Sdaf;

internal sealed record ChannelInfo(
    uint Id,
    string Name,
    SdafLogicalType Type,
    int LogicalBits,
    int StorageBits,
    uint Elements,
    double Scale,
    double Offset,
    string? Unit
);

internal static class SampleCodec
{
    internal static ChannelInfo[] GetChannels(SdafSchema schema, uint streamId)
    {
        return schema
            .Channels(streamId)
            .Select(channel =>
            {
                SdafTlv? type = channel.Find(201);
                SdafTlv? logicalBits = channel.Find(202);
                SdafTlv? storageBits = channel.Find(203);
                if (type is null)
                    throw new SdafFormatException($"Channel {channel.Id} has no logical_type.");
                SdafLogicalType logicalType = (SdafLogicalType)type.AsUInt8();
                int sb =
                    storageBits?.AsUInt16()
                    ?? throw new SdafFormatException($"Channel {channel.Id} has no storage_bits.");
                int lb =
                    logicalBits?.AsUInt16()
                    ?? (
                        logicalType == SdafLogicalType.FixedBytes
                            ? sb
                            : throw new SdafFormatException(
                                $"Channel {channel.Id} has no logical_bits."
                            )
                    );
                uint elements = channel.Find(207)?.AsUInt32() ?? 1;
                if (elements == 0)
                    throw new SdafFormatException(
                        $"Channel {channel.Id} has zero elements_per_sample."
                    );
                ValidateType(channel.Id, logicalType, lb, sb);
                (double scale, double offset) = GetScaleOffset(channel);
                return new ChannelInfo(
                    channel.Id,
                    channel.Name,
                    logicalType,
                    lb,
                    sb,
                    elements,
                    scale,
                    offset,
                    channel.Find(204)?.AsString()
                );
            })
            .ToArray();
    }

    internal static ulong ExpectedCanonicalBytes(
        ChannelInfo[] channels,
        uint samples,
        uint timestampBytes,
        SdafPacking packing
    )
    {
        ulong bitsPerSample = 0;
        ulong bytesPerSample = 0;
        foreach (ChannelInfo channel in channels)
        {
            bitsPerSample = checked(
                bitsPerSample + checked((ulong)channel.StorageBits * channel.Elements)
            );
            bytesPerSample = checked(
                bytesPerSample + checked((ulong)((channel.StorageBits + 7) / 8) * channel.Elements)
            );
        }
        ulong sampleBytes =
            packing == SdafPacking.Lsb0Dense
                ? checked((checked(bitsPerSample * samples) + 7) / 8)
                : checked(bytesPerSample * samples);
        return checked(sampleBytes + timestampBytes);
    }

    internal static IReadOnlyList<SdafSample> DecodeSamples(
        byte[] payload,
        ChannelInfo[] channels,
        SdafDataHeader header
    )
    {
        int timestampBytes = checked((int)header.TimestampBytes);
        ReadOnlySpan<byte> sampleArea = payload.AsSpan(timestampBytes);
        long?[] times = DecodeTimes(payload.AsSpan(0, timestampBytes), header);
        int totalLanes = checked(channels.Sum(c => checked((int)c.Elements)));
        SdafSampleValue[][] values = new SdafSampleValue[checked((int)header.SampleCount)][];
        for (int i = 0; i < values.Length; i++)
            values[i] = new SdafSampleValue[totalLanes];

        ulong bitOffset = 0;
        int byteOffset = 0;
        foreach (
            (int sample, ChannelInfo channel, uint element, int lane) in EnumerateOrder(
                channels,
                header.SampleCount,
                header.Layout
            )
        )
        {
            if (channel.Type == SdafLogicalType.FixedBytes)
            {
                byte[] bytesValue = ReadFixedBytes(
                    sampleArea,
                    ref bitOffset,
                    ref byteOffset,
                    channel.StorageBits,
                    header.Packing
                );
                values[sample][lane] = new SdafSampleValue(
                    channel.Id,
                    channel.Name,
                    element,
                    channel.Type,
                    0,
                    0,
                    double.NaN,
                    double.NaN,
                    bytesValue,
                    channel.Unit
                );
                continue;
            }
            ulong raw =
                header.Packing == SdafPacking.Lsb0Dense
                    ? ReadBits(sampleArea, ref bitOffset, channel.StorageBits)
                    : ReadAligned(sampleArea, ref byteOffset, channel.StorageBits);
            ValidateExtension(raw, channel);
            values[sample][lane] = ToValue(channel, element, raw);
        }
        if (header.Packing == SdafPacking.Lsb0Dense && bitOffset % 8 != 0)
        {
            int used = (int)(bitOffset % 8);
            if ((sampleArea[^1] >> used) != 0)
                throw new SdafFormatException(
                    "Dense sample payload has nonzero final padding bits."
                );
        }

        SdafSample[] result = new SdafSample[values.Length];
        for (int i = 0; i < result.Length; i++)
            result[i] = new SdafSample(header.FirstSampleIndex + (ulong)i, times[i], values[i]);
        return result;
    }

    internal static byte[] DecodeTransforms(
        byte[] encoded,
        IReadOnlyList<SdafTransform> transforms,
        ChannelInfo[] channels,
        SdafDataHeader header
    )
    {
        if (transforms.Count == 0)
            return encoded;
        if (transforms.Count == 1 && transforms[0].Id == 16)
            return Zstandard.Decompress(
                encoded,
                Bin.CheckedInt(header.DecodedSampleBytes, "decoded_sample_bytes")
            );

        if (
            transforms.Count != 4
            || transforms[0].Id != 1
            || transforms[1].Id != 2
            || transforms[2].Id != 3
            || transforms[3].Id != 16
        )
            throw new SdafFormatException(
                "Typed numeric transforms must be exactly [1, 2, 3, 16]."
            );
        if (
            channels.Length == 0
            || channels.Any(c =>
                c.Type is not (SdafLogicalType.UnsignedInteger or SdafLogicalType.SignedInteger)
            )
        )
            throw new SdafFormatException("Compressed numeric profile requires integer channels.");
        int bits = channels[0].LogicalBits;
        if (channels.Any(c => c.LogicalBits != bits))
            throw new SdafFormatException(
                "Compressed numeric profile requires homogeneous logical widths."
            );
        ulong laneCount = checked((ulong)channels.Sum(c => checked((int)c.Elements)));
        ulong valueCount = checked((ulong)header.SampleCount * laneCount);
        int width = (bits + 7) / 8;
        ulong intermediateSize = checked(
            header.TimestampBytes + checked(valueCount * (ulong)width)
        );
        byte[] intermediate = Zstandard.Decompress(
            encoded,
            Bin.CheckedInt(intermediateSize, "compressed numeric intermediate")
        );
        ReadOnlySpan<byte> shuffled = intermediate.AsSpan(checked((int)header.TimestampBytes));
        ulong[] zigzag = Unshuffle(shuffled, checked((int)valueCount), width, bits);
        int[] laneOrder = GetLaneOrder(channels, header.SampleCount, header.Layout);
        ulong[] previous = new ulong[checked((int)laneCount)];
        ulong mask = Mask(bits);
        ulong[] raw = new ulong[zigzag.Length];
        for (int i = 0; i < zigzag.Length; i++)
        {
            ulong z = zigzag[i];
            ulong delta = ((z >> 1) ^ (ulong)-(long)(z & 1)) & mask;
            int lane = laneOrder[i];
            raw[i] = previous[lane] = (previous[lane] + delta) & mask;
        }
        byte[] canonical = new byte[
            Bin.CheckedInt(header.DecodedSampleBytes, "decoded_sample_bytes")
        ];
        intermediate.AsSpan(0, checked((int)header.TimestampBytes)).CopyTo(canonical);
        WriteCanonical(
            raw,
            canonical.AsSpan(checked((int)header.TimestampBytes)),
            channels,
            header
        );
        return canonical;
    }

    internal static byte[] EncodeCompressedNumeric(
        ReadOnlySpan<byte> canonical,
        ChannelInfo[] channels,
        SdafDataHeader header
    )
    {
        if (
            channels.Length == 0
            || channels.Any(c =>
                c.Type is not (SdafLogicalType.UnsignedInteger or SdafLogicalType.SignedInteger)
            )
        )
            throw new ArgumentException("Compressed numeric profile requires integer channels.");
        int bits = channels[0].LogicalBits;
        if (channels.Any(c => c.LogicalBits != bits))
            throw new ArgumentException(
                "Compressed numeric profile requires homogeneous logical widths."
            );
        ulong[] raw = ReadRawInOrder(canonical[(int)header.TimestampBytes..], channels, header);
        int[] laneOrder = GetLaneOrder(channels, header.SampleCount, header.Layout);
        ulong mask = Mask(bits);
        ulong sign = bits == 64 ? 1UL << 63 : 1UL << (bits - 1);
        ulong[] previous = new ulong[channels.Sum(c => checked((int)c.Elements))];
        ulong[] zigzag = new ulong[raw.Length];
        for (int i = 0; i < raw.Length; i++)
        {
            int lane = laneOrder[i];
            ulong delta = (raw[i] - previous[lane]) & mask;
            previous[lane] = raw[i];
            long signed =
                bits == 64 ? unchecked((long)delta)
                : (delta & sign) == 0 ? (long)delta
                : (long)(delta | ~mask);
            zigzag[i] = unchecked(((ulong)(signed << 1)) ^ (ulong)(signed >> 63)) & mask;
        }
        int width = (bits + 7) / 8;
        byte[] shuffled = Shuffle(zigzag, width);
        byte[] intermediate = new byte[checked((int)header.TimestampBytes + shuffled.Length)];
        canonical[..(int)header.TimestampBytes].CopyTo(intermediate);
        shuffled.CopyTo(intermediate, (int)header.TimestampBytes);
        return Zstandard.Compress(intermediate);
    }

    internal static IEnumerable<(
        int Sample,
        ChannelInfo Channel,
        uint Element,
        int Lane
    )> EnumerateOrder(ChannelInfo[] channels, uint sampleCount, SdafLayout layout)
    {
        int[] starts = new int[channels.Length];
        int lane = 0;
        for (int c = 0; c < channels.Length; c++)
        {
            starts[c] = lane;
            lane += checked((int)channels[c].Elements);
        }
        if (layout == SdafLayout.Interleaved)
        {
            for (int sample = 0; sample < sampleCount; sample++)
            for (int c = 0; c < channels.Length; c++)
            for (uint element = 0; element < channels[c].Elements; element++)
                yield return (sample, channels[c], element, starts[c] + checked((int)element));
        }
        else
        {
            for (int c = 0; c < channels.Length; c++)
            for (int sample = 0; sample < sampleCount; sample++)
            for (uint element = 0; element < channels[c].Elements; element++)
                yield return (sample, channels[c], element, starts[c] + checked((int)element));
        }
    }

    private static long?[] DecodeTimes(ReadOnlySpan<byte> area, SdafDataHeader h)
    {
        long?[] result = new long?[checked((int)h.SampleCount)];
        for (int i = 0; i < result.Length; i++)
        {
            result[i] = h.TimestampMode switch
            {
                SdafTimestampMode.Periodic => checked(
                    h.StartTimeTicks
                    + (long)(((UInt128)(uint)i * h.PeriodNumerator) / h.PeriodDenominator)
                ),
                SdafTimestampMode.Delta => checked(
                    h.StartTimeTicks + Bin.I64(area.Slice(i * 8, 8))
                ),
                SdafTimestampMode.Explicit => Bin.I64(area.Slice(i * 8, 8)),
                _ => null,
            };
        }
        return result;
    }

    private static ulong ReadBits(ReadOnlySpan<byte> area, ref ulong bitOffset, int width)
    {
        ulong value = 0;
        for (int bit = 0; bit < width; bit++, bitOffset++)
            if ((area[checked((int)(bitOffset >> 3))] & (1 << (int)(bitOffset & 7))) != 0)
                value |= 1UL << bit;
        return value;
    }

    private static ulong ReadAligned(ReadOnlySpan<byte> area, ref int offset, int bits)
    {
        int count = (bits + 7) / 8;
        ulong value = 0;
        for (int i = 0; i < count; i++)
            value |= (ulong)area[offset + i] << (i * 8);
        offset += count;
        return value;
    }

    private static void ValidateExtension(ulong raw, ChannelInfo channel)
    {
        if (channel.StorageBits == channel.LogicalBits)
            return;
        ulong logicalMask = Mask(channel.LogicalBits);
        ulong highMask = Mask(channel.StorageBits) & ~logicalMask;
        ulong expected =
            channel.Type == SdafLogicalType.SignedInteger
            && (raw & (1UL << (channel.LogicalBits - 1))) != 0
                ? highMask
                : 0;
        if ((raw & highMask) != expected)
            throw new SdafFormatException($"Channel {channel.Id} has invalid unused high bits.");
    }

    private static SdafSampleValue ToValue(ChannelInfo c, uint element, ulong stored)
    {
        ulong raw = stored & Mask(c.LogicalBits);
        long signed =
            c.Type == SdafLogicalType.SignedInteger
                ? SignExtend(raw, c.LogicalBits)
                : unchecked((long)raw);
        byte[]? bytes = null;
        double numeric = c.Type switch
        {
            SdafLogicalType.SignedInteger => signed,
            SdafLogicalType.Float when c.LogicalBits == 32 => BitConverter.Int32BitsToSingle(
                unchecked((int)(uint)raw)
            ),
            SdafLogicalType.Float => BitConverter.Int64BitsToDouble(unchecked((long)raw)),
            SdafLogicalType.FixedBytes => double.NaN,
            _ => raw,
        };
        if (c.Type == SdafLogicalType.FixedBytes)
        {
            bytes = new byte[c.StorageBits / 8];
            for (int i = 0; i < bytes.Length; i++)
                bytes[i] = (byte)(raw >> (i * 8));
        }
        return new SdafSampleValue(
            c.Id,
            c.Name,
            element,
            c.Type,
            raw,
            signed,
            numeric,
            numeric * c.Scale + c.Offset,
            bytes,
            c.Unit
        );
    }

    private static ulong[] ReadRawInOrder(
        ReadOnlySpan<byte> area,
        ChannelInfo[] channels,
        SdafDataHeader h
    )
    {
        ulong count = checked(
            (ulong)h.SampleCount * (ulong)channels.Sum(c => checked((int)c.Elements))
        );
        ulong[] values = new ulong[checked((int)count)];
        ulong bits = 0;
        int bytes = 0;
        int i = 0;
        foreach (var item in EnumerateOrder(channels, h.SampleCount, h.Layout))
            values[i++] =
                h.Packing == SdafPacking.Lsb0Dense
                    ? ReadBits(area, ref bits, item.Channel.StorageBits)
                    : ReadAligned(area, ref bytes, item.Channel.StorageBits);
        return values;
    }

    private static void WriteCanonical(
        ulong[] values,
        Span<byte> area,
        ChannelInfo[] channels,
        SdafDataHeader header
    )
    {
        ulong bitOffset = 0;
        int byteOffset = 0;
        int i = 0;
        foreach (var item in EnumerateOrder(channels, header.SampleCount, header.Layout))
        {
            ChannelInfo channel = item.Channel;
            ulong value = StoredForm(values[i++], channel);
            if (header.Packing == SdafPacking.Lsb0Dense)
            {
                for (int bit = 0; bit < channel.StorageBits; bit++, bitOffset++)
                    if ((value & (1UL << bit)) != 0)
                        area[(int)(bitOffset >> 3)] |= (byte)(1 << (int)(bitOffset & 7));
            }
            else
            {
                int width = (channel.StorageBits + 7) / 8;
                for (int b = 0; b < width; b++)
                    area[byteOffset++] = (byte)(value >> (b * 8));
            }
        }
    }

    private static int[] GetLaneOrder(
        ChannelInfo[] channels,
        uint sampleCount,
        SdafLayout layout
    ) => EnumerateOrder(channels, sampleCount, layout).Select(x => x.Lane).ToArray();

    private static ulong[] Unshuffle(ReadOnlySpan<byte> input, int count, int width, int bits)
    {
        ulong[] values = new ulong[count];
        int offset = 0;
        for (int first = 0; first < count; first += 256)
        {
            int block = Math.Min(256, count - first);
            for (int lane = 0; lane < width; lane++)
            for (int i = 0; i < block; i++)
                values[first + i] |= (ulong)input[offset++] << (lane * 8);
        }
        ulong mask = Mask(bits);
        if (values.Any(x => (x & ~mask) != 0))
            throw new SdafFormatException("Byte-shuffle output has nonzero unused high bits.");
        return values;
    }

    private static byte[] Shuffle(ulong[] values, int width)
    {
        byte[] result = new byte[checked(values.Length * width)];
        int offset = 0;
        for (int first = 0; first < values.Length; first += 256)
        {
            int block = Math.Min(256, values.Length - first);
            for (int lane = 0; lane < width; lane++)
            for (int i = 0; i < block; i++)
                result[offset++] = (byte)(values[first + i] >> (lane * 8));
        }
        return result;
    }

    private static void ValidateType(uint id, SdafLogicalType type, int logical, int storage)
    {
        bool valid = type switch
        {
            SdafLogicalType.UnsignedInteger or SdafLogicalType.SignedInteger => logical
                is >= 1
                    and <= 64
                && storage >= logical
                && storage <= 64,
            SdafLogicalType.Float => (logical == 32 && storage == 32)
                || (logical == 64 && storage == 64),
            SdafLogicalType.Boolean => logical == 1 && storage == 1,
            SdafLogicalType.FixedBytes => storage >= 8 && storage % 8 == 0 && logical == storage,
            _ => false,
        };
        if (!valid)
            throw new SdafFormatException($"Channel {id} has invalid logical/storage width.");
    }

    private static (double, double) GetScaleOffset(SdafSchemaObject channel)
    {
        if (channel.Find(205) is not null && channel.Find(211) is not null)
            throw new SdafFormatException($"Channel {channel.Id} has both scale forms.");
        if (channel.Find(206) is not null && channel.Find(212) is not null)
            throw new SdafFormatException($"Channel {channel.Id} has both offset forms.");
        double scale = channel.Find(211) is { } sr
            ? Ratio(sr)
            : channel.Find(205)?.AsFloat64() ?? 1;
        double offset = channel.Find(212) is { } or
            ? Ratio(or)
            : channel.Find(206)?.AsFloat64() ?? 0;
        return (scale, offset);
        static double Ratio(SdafTlv tlv)
        {
            var r = tlv.AsRationalInt64();
            return (double)r.Numerator / r.Denominator;
        }
    }

    private static ulong Mask(int bits) => bits == 64 ? ulong.MaxValue : (1UL << bits) - 1;

    private static long SignExtend(ulong value, int bits) =>
        bits == 64
            ? unchecked((long)value)
            : unchecked((long)((value ^ (1UL << (bits - 1))) - (1UL << (bits - 1))));

    private static ulong StoredForm(ulong raw, ChannelInfo channel)
    {
        raw &= Mask(channel.LogicalBits);
        if (
            channel.Type == SdafLogicalType.SignedInteger
            && channel.StorageBits > channel.LogicalBits
            && (raw & (1UL << (channel.LogicalBits - 1))) != 0
        )
            raw |= Mask(channel.StorageBits) & ~Mask(channel.LogicalBits);
        return raw;
    }

    private static byte[] ReadFixedBytes(
        ReadOnlySpan<byte> area,
        ref ulong bitOffset,
        ref int byteOffset,
        int storageBits,
        SdafPacking packing
    )
    {
        byte[] result = new byte[storageBits / 8];
        if (packing == SdafPacking.ByteAligned)
        {
            area.Slice(byteOffset, result.Length).CopyTo(result);
            byteOffset += result.Length;
            return result;
        }
        for (int bit = 0; bit < storageBits; bit++, bitOffset++)
            if ((area[(int)(bitOffset >> 3)] & (1 << (int)(bitOffset & 7))) != 0)
                result[bit >> 3] |= (byte)(1 << (bit & 7));
        return result;
    }
}

internal sealed record SdafDataHeader(
    uint SampleCount,
    ulong FirstSampleIndex,
    long StartTimeTicks,
    ulong PeriodNumerator,
    ulong PeriodDenominator,
    SdafTimestampMode TimestampMode,
    SdafLayout Layout,
    SdafPacking Packing,
    uint TimestampBytes,
    ulong DecodedSampleBytes
);
