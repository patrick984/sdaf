using System.Buffers.Binary;
using System.Text;

namespace Sdaf;

internal static class Bin
{
    internal static readonly UTF8Encoding Utf8 = new(false, true);

    internal static ushort U16(ReadOnlySpan<byte> value) =>
        BinaryPrimitives.ReadUInt16LittleEndian(value);

    internal static uint U32(ReadOnlySpan<byte> value) =>
        BinaryPrimitives.ReadUInt32LittleEndian(value);

    internal static ulong U64(ReadOnlySpan<byte> value) =>
        BinaryPrimitives.ReadUInt64LittleEndian(value);

    internal static int I32(ReadOnlySpan<byte> value) =>
        BinaryPrimitives.ReadInt32LittleEndian(value);

    internal static long I64(ReadOnlySpan<byte> value) =>
        BinaryPrimitives.ReadInt64LittleEndian(value);

    internal static double F64(ReadOnlySpan<byte> value) =>
        BitConverter.Int64BitsToDouble(I64(value));

    internal static float F32(ReadOnlySpan<byte> value) =>
        BitConverter.Int32BitsToSingle(I32(value));

    internal static void U16(Span<byte> value, ushort data) =>
        BinaryPrimitives.WriteUInt16LittleEndian(value, data);

    internal static void U32(Span<byte> value, uint data) =>
        BinaryPrimitives.WriteUInt32LittleEndian(value, data);

    internal static void U64(Span<byte> value, ulong data) =>
        BinaryPrimitives.WriteUInt64LittleEndian(value, data);

    internal static void I32(Span<byte> value, int data) =>
        BinaryPrimitives.WriteInt32LittleEndian(value, data);

    internal static void I64(Span<byte> value, long data) =>
        BinaryPrimitives.WriteInt64LittleEndian(value, data);

    internal static bool AllZero(ReadOnlySpan<byte> value)
    {
        foreach (byte b in value)
            if (b != 0)
                return false;
        return true;
    }

    internal static byte[] ReadExactly(Stream stream, int count)
    {
        byte[] result = GC.AllocateUninitializedArray<byte>(count);
        stream.ReadExactly(result);
        return result;
    }

    internal static bool TryReadExactly(Stream stream, Span<byte> value)
    {
        int read = 0;
        while (read < value.Length)
        {
            int current = stream.Read(value[read..]);
            if (current == 0)
                return false;
            read += current;
        }
        return true;
    }

    internal static int CheckedInt(ulong value, string name)
    {
        if (value > int.MaxValue)
            throw new SdafFormatException($"{name} exceeds this implementation's array limit.");
        return (int)value;
    }
}

public static class SdafCrc32C
{
    private static readonly uint[] Table = CreateTable();

    public static uint Compute(ReadOnlySpan<byte> data)
    {
        uint crc = uint.MaxValue;
        foreach (byte b in data)
            crc = Table[(crc ^ b) & 0xff] ^ (crc >> 8);
        return crc ^ uint.MaxValue;
    }

    private static uint[] CreateTable()
    {
        uint[] table = new uint[256];
        for (uint i = 0; i < table.Length; i++)
        {
            uint value = i;
            for (int bit = 0; bit < 8; bit++)
                value = (value >> 1) ^ ((value & 1) != 0 ? 0x82f63b78u : 0);
            table[i] = value;
        }
        return table;
    }
}
