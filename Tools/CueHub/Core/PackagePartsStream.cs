using System.Security.Cryptography;

namespace Cue.Hub;

/// <summary>順序付き Part を固定 Memory 量で結合する読取専用 Stream。Handle を所有し、同時呼出しは非対応</summary>
public sealed class PackagePartsStream : Stream
{
    private const int k_bufferSize = 131072;
    private readonly FileStream[] m_files;
    private readonly long[] m_offsets;
    private long m_position;
    private bool m_isDisposed;

    /// <summary>通常 File だけを開き、検証から展開の終了まで書換え / 削除を共有しない</summary>
    public PackagePartsStream(IReadOnlyList<string> a_files)
    {
        if (a_files.Count is < 1 or > 512) throw new InvalidDataException("Package Part 数が許容範囲外です");
        m_files = new FileStream[a_files.Count];
        m_offsets = new long[a_files.Count + 1];
        try
        {
            for (var index = 0; index < a_files.Count; ++index)
            {
                var file = SafePaths.regular_file(a_files[index]);
                m_files[index] = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read,
                    1, FileOptions.Asynchronous | FileOptions.RandomAccess);
                m_offsets[index + 1] = checked(m_offsets[index] + m_files[index].Length);
            }
        }
        catch
        {
            foreach (var file in m_files) file?.Dispose();
            throw;
        }
    }

    public override bool CanRead => !m_isDisposed;
    public override bool CanSeek => !m_isDisposed;
    public override bool CanWrite => false;
    public override long Length { get { require_open(); return m_offsets[^1]; } }
    public override long Position
    {
        get { require_open(); return m_position; }
        set { Seek(value, SeekOrigin.Begin); }
    }

    /// <summary>各 Part と結合全体の署名済み Hash / 容量を照合し、成功 / 失敗のいずれも Position を先頭へ戻す</summary>
    public async Task verify_async(IReadOnlyList<ReleasePackagePart> a_parts, string a_totalHash, CancellationToken a_cancel)
    {
        require_open();
        try
        {
            if (a_parts.Count != m_files.Length || !is_hash(a_totalHash))
                throw new InvalidDataException("Package Part の検証情報が不正です");
            using var total = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buffer = new byte[k_bufferSize];
            for (var index = 0; index < m_files.Length; ++index)
            {
                a_cancel.ThrowIfCancellationRequested();
                var part = a_parts[index];
                var file = m_files[index];
                if (part.Size <= 0 || file.Length != part.Size || !is_hash(part.Sha256))
                    throw new InvalidDataException("Package Part の容量が一致しません");
                using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                file.Position = 0;
                for (int count; (count = await file.ReadAsync(buffer, a_cancel)) != 0;)
                {
                    hash.AppendData(buffer, 0, count);
                    total.AppendData(buffer, 0, count);
                }
                if (!Convert.ToHexString(hash.GetHashAndReset()).Equals(part.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Package Part の SHA256 が一致しません");
            }
            a_cancel.ThrowIfCancellationRequested();
            if (!Convert.ToHexString(total.GetHashAndReset()).Equals(a_totalHash, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("結合 Package の SHA256 が一致しません");
        }
        finally
        {
            m_position = 0;
            foreach (var file in m_files) file.Position = 0;
        }
    }

    public override int Read(byte[] a_buffer, int a_offset, int a_count) => Read(a_buffer.AsSpan(a_offset, a_count));

    public override int Read(Span<byte> a_buffer)
    {
        require_open();
        var received = 0;
        while (received < a_buffer.Length && m_position < Length)
        {
            var index = find_part();
            var file = m_files[index];
            file.Position = m_position - m_offsets[index];
            var count = file.Read(a_buffer.Slice(received, (int)Math.Min(a_buffer.Length - received, file.Length - file.Position)));
            if (count == 0) throw new EndOfStreamException("Package Part の読取りが途中で終了しました");
            received += count;
            m_position += count;
        }
        return received;
    }

    public override Task<int> ReadAsync(byte[] a_buffer, int a_offset, int a_count, CancellationToken a_cancel) =>
        ReadAsync(a_buffer.AsMemory(a_offset, a_count), a_cancel).AsTask();

    public override async ValueTask<int> ReadAsync(Memory<byte> a_buffer, CancellationToken a_cancel = default)
    {
        require_open();
        a_cancel.ThrowIfCancellationRequested();
        var received = 0;
        while (received < a_buffer.Length && m_position < Length)
        {
            var index = find_part();
            var file = m_files[index];
            file.Position = m_position - m_offsets[index];
            var count = await file.ReadAsync(a_buffer.Slice(received,
                (int)Math.Min(a_buffer.Length - received, file.Length - file.Position)), a_cancel);
            if (count == 0) throw new EndOfStreamException("Package Part の読取りが途中で終了しました");
            received += count;
            m_position += count;
        }
        return received;
    }

    public override long Seek(long a_offset, SeekOrigin a_origin)
    {
        require_open();
        long position;
        try
        {
            position = checked(a_origin switch
            {
                SeekOrigin.Begin => a_offset,
                SeekOrigin.Current => m_position + a_offset,
                SeekOrigin.End => Length + a_offset,
                _ => throw new ArgumentOutOfRangeException(nameof(a_origin))
            });
        }
        catch (OverflowException) { throw new IOException("Package Stream の Seek 先が範囲外です"); }
        if (position < 0) throw new IOException("Package Stream の先頭より前には Seek できません");
        m_position = position;
        return position;
    }

    public override void Flush() { require_open(); }
    public override void SetLength(long a_value) => throw new NotSupportedException("Package Stream は読取専用です");
    public override void Write(byte[] a_buffer, int a_offset, int a_count) => throw new NotSupportedException("Package Stream は読取専用です");

    protected override void Dispose(bool a_isDisposing)
    {
        if (!m_isDisposed && a_isDisposing)
            foreach (var file in m_files) file.Dispose();
        m_isDisposed = true;
        base.Dispose(a_isDisposing);
    }

    public override async ValueTask DisposeAsync()
    {
        if (!m_isDisposed)
            foreach (var file in m_files) await file.DisposeAsync();
        m_isDisposed = true;
        GC.SuppressFinalize(this);
    }

    private int find_part()
    {
        // 同じ Offset の空 Part を含めて、Position より後ろに終端がある最初の Part を選ぶ
        var low = 0;
        var high = m_files.Length;
        while (low < high)
        {
            var middle = low + (high - low) / 2;
            if (m_offsets[middle + 1] <= m_position) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    private void require_open() => ObjectDisposedException.ThrowIf(m_isDisposed, this);
    private static bool is_hash(string a_hash) => a_hash.Length == 64 && a_hash.All(char.IsAsciiHexDigit);
}
