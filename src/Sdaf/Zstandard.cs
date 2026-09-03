using System.Runtime.InteropServices;

namespace Sdaf;

internal static partial class Zstandard
{
    private const string Library = "libzstd";

    [LibraryImport(Library, EntryPoint = "ZSTD_compressBound")]
    private static partial nuint CompressBound(nuint srcSize);

    [LibraryImport(Library, EntryPoint = "ZSTD_compress")]
    private static unsafe partial nuint CompressNative(
        byte* dst,
        nuint dstCapacity,
        byte* src,
        nuint srcSize,
        int level
    );

    [LibraryImport(Library, EntryPoint = "ZSTD_decompress")]
    private static unsafe partial nuint DecompressNative(
        byte* dst,
        nuint dstCapacity,
        byte* src,
        nuint compressedSize
    );

    [LibraryImport(Library, EntryPoint = "ZSTD_isError")]
    private static partial uint IsError(nuint code);

    [LibraryImport(Library, EntryPoint = "ZSTD_getErrorName")]
    private static partial nint GetErrorName(nuint code);

    internal static unsafe byte[] Compress(ReadOnlySpan<byte> source, int level = 3)
    {
        int bound = checked((int)CompressBound((nuint)source.Length));
        byte[] destination = GC.AllocateUninitializedArray<byte>(bound);
        fixed (byte* src = source)
        fixed (byte* dst = destination)
        {
            nuint size = CompressNative(
                dst,
                (nuint)destination.Length,
                src,
                (nuint)source.Length,
                level
            );
            Check(size);
            Array.Resize(ref destination, checked((int)size));
            return destination;
        }
    }

    internal static unsafe byte[] Decompress(ReadOnlySpan<byte> source, int expectedSize)
    {
        byte[] destination = GC.AllocateUninitializedArray<byte>(expectedSize);
        fixed (byte* src = source)
        fixed (byte* dst = destination)
        {
            nuint size = DecompressNative(
                dst,
                (nuint)destination.Length,
                src,
                (nuint)source.Length
            );
            Check(size);
            if (size != (nuint)expectedSize)
                throw new SdafFormatException(
                    $"Zstandard output was {size} bytes; expected {expectedSize}."
                );
        }
        return destination;
    }

    private static void Check(nuint code)
    {
        if (IsError(code) == 0)
            return;
        string message = Marshal.PtrToStringAnsi(GetErrorName(code)) ?? "unknown error";
        throw new SdafFormatException($"Zstandard error: {message}.");
    }
}
