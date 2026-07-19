using System.Buffers;
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.IO.Hashing;
using System.Runtime.CompilerServices;
using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;
using System.Runtime.InteropServices;
using System.Text.Unicode;
using Microsoft.Win32.SafeHandles;

static class Program
{
    const int MAX_STACKALLOC_SIZE = 8 * 1024 * 1024; // 8 MB (Linux default stack size)
    const int INPUT_CHUNK_SIZE = 128 * 1024;
    const int EXTRA_BUFFER_SIZE = 1024;
    const int MIN_INPUT_LINE_SIZE = 16;
    const int MAX_INPUT_LINES = (INPUT_CHUNK_SIZE + EXTRA_BUFFER_SIZE + MIN_INPUT_LINE_SIZE - 1) / MIN_INPUT_LINE_SIZE;
    const int STATISTICS_SHARD_COUNT = 64;
    const int STATISTICS_SHARD_SHIFT = 58;

    static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.WriteLine("Usage: Program <inputFile> <outputFile>");
            return 1;
        }
        var (inputFile, outputFile) = (args[0], args[1]);
        if (!File.Exists(inputFile))
        {
            Console.WriteLine($"Input file '{inputFile}' does not exist.");
            return 1;
        }

        var inputFileInfo = new FileInfo(inputFile);
        var chunkSize = Math.Min(inputFileInfo.Length, INPUT_CHUNK_SIZE);
        var chunks = (int)Math.Ceiling((double)inputFileInfo.Length / chunkSize);
        using var inputHandle = File.OpenHandle(
            inputFile,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            FileOptions.RandomAccess
        );
#if DEBUG
        const int _alignment = 4;
        Console.WriteLine($"""

            Input file | {inputFileInfo.Length / 1024 / 1024,_alignment} MiB
            Chunk size | {chunkSize / 1024 / 1024,_alignment} MiB
            Chunks     | {chunks,_alignment} chunks
            """);
#endif

        var statisticsShards = new StatisticsShard[STATISTICS_SHARD_COUNT];
        for (int i = 0; i < statisticsShards.Length; i++)
        {
            statisticsShards[i] = new StatisticsShard();
        }

        Parallel.For(0, chunks, i => ProcessChunk(inputHandle, i * chunkSize, chunkSize, statisticsShards));

        WriteOutput(outputFile, statisticsShards);

        return 0;
    }

    static unsafe void ProcessChunk(
        SafeFileHandle file,
        long cursorOffset,
        long chunkSize,
        StatisticsShard[] statisticsShards)
    {
        Debug.Assert(file is not null && !file.IsInvalid, "File handle should be valid.");
        Debug.Assert(cursorOffset >= 0, "Cursor offset should be non-negative.");
        Debug.Assert(chunkSize > 0, "Chunk size should be positive.");

        var bufferSize = (int)chunkSize + EXTRA_BUFFER_SIZE;
        using RentArray<byte> bufferArray = new(bufferSize);
        Span<byte> buffer = bufferArray.Array.AsSpan(0, bufferSize);

        scoped ReadOnlySpan<byte> data;
        var hasLeadingNewLine = false;
        if (cursorOffset != 0)
        {
            hasLeadingNewLine =
                RandomAccess.Read(file, buffer[..1], cursorOffset - 1) == 1
                && buffer[0] == '\n';
        }

        var bytesRead = RandomAccess.Read(file, buffer[..(int)chunkSize], cursorOffset);
        data = buffer[..bytesRead];
        if (!hasLeadingNewLine) // Matches when the chunk start is in the middle of a line or when the first chunk (skip header line).
        {
            var firstNewLineIndex = data.IndexOf((byte)'\n'); // ReadOnlySpan<byte>.IndexOf method is already vectorized: https://github.com/dotnet/dotnet/blob/c506080fe921205a9b2b374ed3fd37b297c6d74a/src/runtime/src/libraries/System.Private.CoreLib/src/System/SpanHelpers.T.cs#L1657
            if (firstNewLineIndex == -1)
            {
                return; // Skip this chunk as it is in the middle of a line, and the previous chunk will handle it.
            }
            data = data[(firstNewLineIndex + 1)..]; // Skip the first line because the previous chunk will handle it.
        }

        if (!data.EndsWith((byte)'\n'))
        {
            var extraBuffer = buffer.Slice(
                (int)Unsafe.ByteOffset(ref MemoryMarshal.GetReference(buffer), ref MemoryMarshal.GetReference(data)) + data.Length, // (&data[0] + data.Length) - &buffer[0]
                EXTRA_BUFFER_SIZE
            );
            var extraBytesRead = RandomAccess.Read(file, extraBuffer, cursorOffset + bytesRead);
            var firstNewLineIndex = extraBuffer[..extraBytesRead].IndexOf((byte)'\n');
            if (firstNewLineIndex == -1)
            {
                throw new InvalidOperationException("The chunk size is too small to contain a complete line. Please increase the chunk size.");
            }
            data = MemoryMarshal.CreateReadOnlySpan(ref MemoryMarshal.GetReference(data), data.Length + firstNewLineIndex + 1); // data and extraBuffer are contiguous in memory, so we can create a new ReadOnlySpan<byte> that includes both.
        }
        Debug.Assert(data.EndsWith((byte)'\n'), "Data should end with a newline character.");
#if DEBUG
        if (cursorOffset == 0)
        {
            const int _alignment = 7;
            Console.WriteLine($"""
                
                < chunk[0] >
                Actual size | {data.Length,_alignment} bytes ({data.Length * 100 / buffer.Length} % used)
                Lines       | {data.Count((byte)'\n'),_alignment} lines
                """);
        }
#endif

        using RentArray<ulong> dataKeysArray = new(MAX_INPUT_LINES);
        var dataKeys = dataKeysArray.Array.AsSpan(0, MAX_INPUT_LINES);

        Span<uint> lineStatistics = stackalloc uint[MAX_INPUT_LINES];
        using RentArray<ulong> groupKeyHashesArray = new(MAX_INPUT_LINES);
        var groupKeyHashes = groupKeyHashesArray.Array.AsSpan(0, MAX_INPUT_LINES);

#if DEBUG
        var stopwatch = Stopwatch.StartNew();
#endif
        int lineCount = 0;
        ref var dataRef = ref MemoryMarshal.GetReference(data);
        for (int i = 0; i < data.Length; i++)
        {
            Debug.Assert(data[i] != (byte)'\n', "Data should not contain newline characters at this point.");
            ref var startRef = ref Unsafe.Add(ref dataRef, i);
            var followingData = MemoryMarshal.CreateReadOnlySpan(ref startRef, data.Length - i);
            var nextNewLineIndex = followingData.IndexOf((byte)'\n');
            Debug.Assert(nextNewLineIndex != -1, "There should be a newline character in the following data.");

            var line = followingData[..nextNewLineIndex];
            var parsedData = Data.Parse(line);
            Debug.Assert(parsedData.MessageLength <= ushort.MaxValue, "Message length should fit in 16 bits.");
            Debug.Assert(parsedData.StampCount <= ushort.MaxValue, "Stamp count should fit in 16 bits.");
            lineStatistics[lineCount] =
                (ushort)parsedData.MessageLength
                | ((uint)(ushort)parsedData.StampCount << 16);
            groupKeyHashes[lineCount] = parsedData.GetGroupKeyHashCode();
            var channelOffset = (uint)Unsafe.ByteOffset(
                ref dataRef,
                ref MemoryMarshal.GetReference(parsedData.ChannelPath)
            );
            Debug.Assert(parsedData.ChannelPath.Length <= ushort.MaxValue, "Channel path should fit in 16 bits.");
            dataKeys[lineCount] =
                channelOffset
                | ((ulong)(ushort)parsedData.ChannelPath.Length << 32)
                | ((ulong)(byte)parsedData.YearMonth.Month << 48);

            lineCount++;
            i += nextNewLineIndex; // Move the index to the next newline character
        }
#if DEBUG
        stopwatch.Stop();
        var lineParsingTime = stopwatch.Elapsed;
        stopwatch.Restart();
        TimeSpan sortingTime;
#endif

        using RentArray<ulong> orderedGroupKeyHashesArray = new(lineCount);
        using RentArray<int> orderedLineIndexesArray = new(lineCount);
        var orderedGroupKeyHashes = orderedGroupKeyHashesArray.Array.AsSpan(0, lineCount);
        var orderedLineIndexes = orderedLineIndexesArray.Array.AsSpan(0, lineCount);
        PartitionByShard(groupKeyHashes[..lineCount], orderedGroupKeyHashes, orderedLineIndexes);
#if DEBUG
        stopwatch.Stop();
        sortingTime = stopwatch.Elapsed;
        stopwatch.Restart();
#endif

        for (int i = 0; i < lineCount;)
        {
            var shardIndex = (int)(orderedGroupKeyHashes[i] >> STATISTICS_SHARD_SHIFT);
            int shardEnd = i + 1;
            while (shardEnd < lineCount && (int)(orderedGroupKeyHashes[shardEnd] >> STATISTICS_SHARD_SHIFT) == shardIndex)
            {
                shardEnd++;
            }
            var shard = statisticsShards[shardIndex];
            lock (shard.Sync)
            {
                while (i < shardEnd)
                {
                    var groupHash = orderedGroupKeyHashes[i];
                    var lineIndex = orderedLineIndexes[i];
                    var packedStatistics = lineStatistics[lineIndex];
                    var messageLength = (ushort)packedStatistics;
                    var stampCount = (ushort)(packedStatistics >> 16);
                    var groupKey = dataKeys[lineIndex];
                    var channelOffset = (int)(uint)groupKey;
                    var channelLength = (int)(ushort)(groupKey >> 32);
                    var month = (int)(byte)(groupKey >> 48);
                    ref var entry = ref shard.GetValueRefOrAddDefault(groupHash, out var exists);
                    if (exists)
                    {
                        ref var previous = ref entry.Statistics;
                        previous.MinMessageLength = Math.Min(previous.MinMessageLength, messageLength);
                        previous.MaxMessageLength = Math.Max(previous.MaxMessageLength, messageLength);
                        previous.TotalMessageLength += messageLength;
                        previous.TotalMessageCount++;
                        previous.TotalStampCount += stampCount;
                    }
                    else
                    {
                        entry = new StatisticsEntry(
                            data.Slice(channelOffset, channelLength).ToArray(),
                            month,
                            new DataGroupStatistics
                            {
                                MinMessageLength = messageLength,
                                MaxMessageLength = messageLength,
                                TotalMessageLength = messageLength,
                                TotalMessageCount = 1,
                                TotalStampCount = stampCount
                            }
                        );
                    }
                    i++;
                }
            }
        }
#if DEBUG
        stopwatch.Stop();
        var analyzingTime = stopwatch.Elapsed;
        if (cursorOffset == 0)
        {
            const int _alignment = 7;
            Console.WriteLine($"""
                    
                < chunk[0] >
                Line parsing | {lineParsingTime.TotalMilliseconds,_alignment:F3} ms
                Sorting      | {sortingTime.TotalMilliseconds,_alignment:F3} ms
                Analyzing    | {analyzingTime.TotalMilliseconds,_alignment:F3} ms
                """);
        }
#endif
    }

    static void PartitionByShard(
        ReadOnlySpan<ulong> groupKeyHashes,
        Span<ulong> orderedGroupKeyHashes,
        Span<int> orderedLineIndexes)
    {
        Span<int> shardOffsets = stackalloc int[STATISTICS_SHARD_COUNT];
        foreach (var groupHash in groupKeyHashes)
        {
            shardOffsets[(int)(groupHash >> STATISTICS_SHARD_SHIFT)]++;
        }

        int position = 0;
        for (int i = 0; i < shardOffsets.Length; i++)
        {
            var count = shardOffsets[i];
            shardOffsets[i] = position;
            position += count;
        }

        for (int i = 0; i < groupKeyHashes.Length; i++)
        {
            var groupHash = groupKeyHashes[i];
            var shardIndex = (int)(groupHash >> STATISTICS_SHARD_SHIFT);
            var destinationIndex = shardOffsets[shardIndex]++;
            orderedGroupKeyHashes[destinationIndex] = groupHash;
            orderedLineIndexes[destinationIndex] = i;
        }
    }

    static void WriteOutput(string filename, StatisticsShard[] statisticsShards)
    {
        using var outputFileStream = File.Open(filename, FileMode.OpenOrCreate, FileAccess.Write, FileShare.None);
        outputFileStream.SetLength(0); // Clear the output file before writing
        Span<byte> outputBuffer = stackalloc byte[MAX_STACKALLOC_SIZE / 2]; // 4 MB
        int bufferPosition = 0;
        Span<byte> keyBuffer = stackalloc byte[1024];
        foreach (var shard in statisticsShards)
        {
            for (int i = 0; i < shard.Keys.Length; i++)
            {
                if (shard.Keys[i] == 0)
                {
                    continue;
                }
                var entry = shard.Values[i];
                var value = entry.Statistics;
                entry.ChannelPath.CopyTo(keyBuffer);
                keyBuffer[entry.ChannelPath.Length] = (byte)',';
                var yearMonthBytes = DateTimeHelper.FormatYearMonth(keyBuffer[(entry.ChannelPath.Length + 1)..], 2027, entry.Month);
                var key = keyBuffer[..(entry.ChannelPath.Length + 1 + yearMonthBytes)];

                if (bufferPosition + key.Length > outputBuffer.Length)
                {
                    outputFileStream.Write(outputBuffer[..bufferPosition]);
                    bufferPosition = 0;
                }
                var entryStart = bufferPosition;
                key.CopyTo(outputBuffer[bufferPosition..]);
                bufferPosition += key.Length;

                if (!Utf8.TryWrite(outputBuffer[bufferPosition..],
                    $"={value.MinMessageLength}/{(double)value.TotalMessageLength / value.TotalMessageCount:F2}/{value.MaxMessageLength}/{value.TotalMessageCount}/{value.TotalStampCount}\n",
                    out var bytesWritten))
                {
                    outputFileStream.Write(outputBuffer[..entryStart]);
                    key.CopyTo(outputBuffer);
                    bufferPosition = key.Length;

                    if (!Utf8.TryWrite(outputBuffer[bufferPosition..],
                        $"={value.MinMessageLength}/{(double)value.TotalMessageLength / value.TotalMessageCount:F2}/{value.MaxMessageLength}/{value.TotalMessageCount}/{value.TotalStampCount}\n",
                        out bytesWritten))
                    {
                        throw new InvalidOperationException("Output buffer is too small to write a single line.");
                    }
                }
                bufferPosition += bytesWritten;
            }
        }
        if (bufferPosition > 0)
        {
            outputFileStream.Write(outputBuffer[..bufferPosition]);
        }
    }

}

sealed class StatisticsShard
{
    const int CAPACITY = 4096;
    const int MASK = CAPACITY - 1;

    public Lock Sync { get; } = new();
    public ulong[] Keys { get; } = new ulong[CAPACITY];
    public StatisticsEntry[] Values { get; } = new StatisticsEntry[CAPACITY];

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ref StatisticsEntry GetValueRefOrAddDefault(ulong key, out bool exists)
    {
        Debug.Assert(key != 0, "Zero is reserved as the empty slot.");
        var index = (int)key & MASK;
        while (true)
        {
            var existingKey = Keys[index];
            if (existingKey == key)
            {
                exists = true;
                return ref Values[index];
            }
            if (existingKey == 0)
            {
                Keys[index] = key;
                exists = false;
                return ref Values[index];
            }
            index = (index + 1) & MASK;
        }
    }
}

struct StatisticsEntry(byte[] channelPath, int month, DataGroupStatistics statistics)
{
    public byte[] ChannelPath = channelPath;
    public int Month = month;
    public DataGroupStatistics Statistics = statistics;
}

readonly struct RentArray<T>(int length) : IDisposable
{
    public readonly T[] Array { get; init; } = ArrayPool<T>.Shared.Rent(length);

    public void Dispose()
    {
        ArrayPool<T>.Shared.Return(Array);
    }
}

readonly ref struct Data
{
    public readonly ReadOnlySpan<byte> ChannelPath { get; init; }
    public readonly (int Year, int Month) YearMonth { get; init; }
    public readonly int MessageLength { get; init; }
    public readonly int StampCount { get; init; }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ulong GetGroupKeyHashCode()
    {
        return XxHash3.HashToUInt64(ChannelPath) ^ ((ulong)YearMonth.Month * 0x9E3779B97F4A7C15UL);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Data Parse(ReadOnlySpan<byte> line)
    {
        Debug.Assert(line.Length > 14, "Input line should contain all fields.");
        Debug.Assert(line[10] == (byte)',', "Timestamp should contain exactly 10 digits.");

        ref var lineStart = ref MemoryMarshal.GetReference(line);
        var timestampPrefix = ParseTimestampPrefix(ref lineStart);
        ref var channelPathStart = ref Unsafe.Add(ref lineStart, 11);
        int channelPathLength = 0;
        while (Unsafe.Add(ref channelPathStart, channelPathLength) != (byte)',')
        {
            channelPathLength++;
        }
        Debug.Assert(channelPathLength > 0, "Channel path should not be empty.");
        var channelPath = MemoryMarshal.CreateReadOnlySpan(ref channelPathStart, channelPathLength);

        ref var messageLengthStart = ref Unsafe.Add(ref channelPathStart, channelPathLength + 1);
        int messageLengthLength = 0;
        while (Unsafe.Add(ref messageLengthStart, messageLengthLength) != (byte)',')
        {
            messageLengthLength++;
        }
        Debug.Assert(messageLengthLength > 0, "Message length should not be empty.");
        var messageLength = MemoryMarshal.CreateReadOnlySpan(ref messageLengthStart, messageLengthLength);
        ref var stampCountStart = ref Unsafe.Add(ref messageLengthStart, messageLengthLength + 1);
        var stampCountLength = line.Length - 13 - channelPathLength - messageLengthLength;

        return new Data
        {
            ChannelPath = channelPath,
            YearMonth = (2027, GetMonth(timestampPrefix)),
            MessageLength = ParseUnsigned(messageLength),
            StampCount = ParseUnsigned(MemoryMarshal.CreateReadOnlySpan(ref stampCountStart, stampCountLength))
        };

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        static int ParseTimestampPrefix(ref byte first)
        {
            Debug.Assert(Ssse3.IsSupported, "SSSE3 should be supported.");
            var ascii = Vector128.CreateScalar(Unsafe.ReadUnaligned<ulong>(ref first)).AsByte();
            var digits = Sse2.Subtract(ascii, Vector128.Create((byte)'0'));
            var pairs = Ssse3.MultiplyAddAdjacent(
                digits,
                Vector128.Create((sbyte)10, 1, 10, 1, 10, 1, 10, 1, 0, 0, 0, 0, 0, 0, 0, 0)
            );
            var groups = Sse2.MultiplyAddAdjacent(
                pairs,
                Vector128.Create((short)100, 1, 100, 1, 0, 0, 0, 0)
            );
            return groups.GetElement(0) * 10_000 + groups.GetElement(1);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        static int ParseUnsigned(ReadOnlySpan<byte> digits)
        {
            int value = 0;
            ref var first = ref MemoryMarshal.GetReference(digits);
            for (int i = 0; i < digits.Length; i++)
            {
                value = value * 10 + Unsafe.Add(ref first, i) - (byte)'0';
            }
            return value;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        static int GetMonth(int timestampPrefix)
        {
            Debug.Assert(timestampPrefix >= 17_987_616 && timestampPrefix < 18_302_976, "Timestamp should be in 2027.");
            if (timestampPrefix < 18_144_000) // 2027-07-01
            {
                if (timestampPrefix < 18_065_376) // 2027-04-01
                {
                    if (timestampPrefix < 18_038_592) // 2027-03-01
                    {
                        return timestampPrefix < 18_014_400 ? 1 : 2;
                    }
                    return 3;
                }
                if (timestampPrefix < 18_118_080) // 2027-06-01
                {
                    return timestampPrefix < 18_091_296 ? 4 : 5;
                }
                return 6;
            }
            if (timestampPrefix < 18_223_488) // 2027-10-01
            {
                if (timestampPrefix < 18_197_568) // 2027-09-01
                {
                    return timestampPrefix < 18_170_784 ? 7 : 8;
                }
                return 9;
            }
            if (timestampPrefix < 18_276_192) // 2027-12-01
            {
                return timestampPrefix < 18_250_272 ? 10 : 11;
            }
            return 12;
        }
    }
}

struct DataGroupStatistics
{
    public int MinMessageLength;
    public int MaxMessageLength;
    public long TotalMessageLength;
    public int TotalMessageCount;
    public int TotalStampCount;
}

static class DateTimeHelper
{
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static (int Year, int Month) GetYearMonthFromTimestamp(long timestamp)
    {
        var days = (int)(timestamp / TimeSpan.SecondsPerDay); // days since 1970-01-01
        if (days <= 730484) // Before 1999-12-31, use slow path
        {
            var dateOnly = DateOnly.FromDayNumber(days + 719162); // 719162 is the number of days from 0001-01-01 to 1970-01-01
            return (dateOnly.Year, dateOnly.Month);
        }

        int year = 2000; // Start from the year 2000 (400-year cycle)

        var (div, rem) = Math.DivRem(days, 365 * 400 + 97); // 400-year cycles
        year += div * 400;
        days = rem;
        if (days <= 366)
        {
            return (year, GetMonthFromYearDaysLeap(days));
        }

        (div, rem) = Math.DivRem(days, 365 * 100 + 24); // 100-year cycles
        Debug.Assert(div <= 3, "div should be less than or equal to 3 for 100-year cycles.");
        year += div * 100;
        days = rem;
        if (days <= 365)
        {
            return (year, GetMonthFromYearDays(days));
        }

        (div, rem) = Math.DivRem(days, 365 * 4 + 1); // 4-year cycles
        Debug.Assert(div <= 24, "div should be less than or equal to 24 for 4-year cycles.");
        year += div * 4;
        days = rem;
        if (days <= 366)
        {
            return (year, GetMonthFromYearDaysLeap(days));
        }

        (div, rem) = Math.DivRem(days, 365); // 1-year cycles
        Debug.Assert(div <= 3, "div should be less than or equal to 3 for 1-year cycles.");
        Debug.Assert(rem <= 365, "rem should be less than or equal to 365 for 1-year cycles.");
        return (year + div, GetMonthFromYearDays(rem));

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        static int GetMonthFromYearDays(int yearDays)
        {
            return yearDays switch
            {
                <= 31 => 1, // +31
                <= 59 => 2, // +28
                <= 90 => 3, // +31
                <= 120 => 4, // +30
                <= 151 => 5, // +31
                <= 181 => 6, // +30
                <= 212 => 7, // +31
                <= 243 => 8, // +31
                <= 273 => 9, // +30
                <= 304 => 10, // +31
                <= 334 => 11, // +30
                _ => 12,
            };
        }
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        static int GetMonthFromYearDaysLeap(int yearDays)
        {
            return yearDays switch
            {
                <= 31 => 1, // +31
                <= 60 => 2, // +29
                <= 91 => 3, // +31
                <= 121 => 4, // +30
                <= 152 => 5, // +31
                <= 182 => 6, // +30
                <= 213 => 7, // +31
                <= 244 => 8, // +31
                <= 274 => 9, // +30
                <= 305 => 10, // +31
                <= 335 => 11, // +30
                _ => 12,
            };
        }
    }

    public const int YearMonthStringLength = 7; // "YYYY-MM"

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static int FormatYearMonth(Span<byte> dest, int year, int month)
    {
        if (dest.Length < YearMonthStringLength)
        {
            throw new ArgumentException("Destination span is too small to hold the formatted year-month string.");
        }
        for (int i = 3; i >= 0; i--)
        {
            dest[i] = (byte)('0' + (year % 10));
            year /= 10;
        }
        dest[4] = (byte)'-';
        dest[5] = (byte)('0' + (month / 10));
        dest[6] = (byte)('0' + (month % 10));
        return YearMonthStringLength;
    }
}

sealed class Utf8StringEqualityComparer : IEqualityComparer<byte[]>, IAlternateEqualityComparer<ReadOnlySpan<byte>, byte[]>
{
    public static IEqualityComparer<byte[]> Default { get; } = new Utf8StringEqualityComparer();

    public bool Equals(byte[]? x, byte[]? y)
    {
        if (x is null && y is null)
        {
            return true;
        }
        if (x is null || y is null)
        {
            return false;
        }
        return x.AsSpan().SequenceEqual(y);
    }

    public int GetHashCode([DisallowNull] byte[] obj)
    {
        return GetHashCode(obj.AsSpan());
    }

    public byte[] Create(ReadOnlySpan<byte> alternate)
    {
        return alternate.ToArray();
    }

    public bool Equals(ReadOnlySpan<byte> alternate, byte[] other)
    {
        return other.AsSpan().SequenceEqual(alternate);
    }

    public int GetHashCode(ReadOnlySpan<byte> alternate)
    {
        return unchecked((int)XxHash3.HashToUInt64(alternate));
    }
}

static class BinarySearchHelper
{
    public static int LastIndexOf<T>(ReadOnlySpan<T> span, T value, Comparison<T> comparer)
    {
        if (span.Length == 0)
        {
            return -1;
        }
        int begin = 0;
        int end = span.Length - 1;
        while (begin < end)
        {
            int idx = (begin + end + 1) >> 1; // Math.Ceil((begin + end) / 2);
            if (comparer(span[idx], value) > 0) // span[idx] > value
            {
                end = idx - 1;
            }
            else
            {
                begin = idx;
            }
        }
        return (comparer(span[begin], value) == 0) ? begin : -1;
    }
}
