using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;
using System.Threading;

namespace Cue.VisualStudioBridge
{
    internal static class NativeMethods
    {
        [DllImport("ole32.dll")]
        internal static extern int CreateBindCtx(uint reserved, out IBindCtx context);
        [DllImport("ole32.dll")]
        internal static extern int GetRunningObjectTable(uint reserved, out IRunningObjectTable table);
        [DllImport("ole32.dll")]
        internal static extern int CoRegisterMessageFilter(IMessageFilter filter, out IMessageFilter oldFilter);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            public uint cb;
            public string reserved, desktop, title;
            public uint x, y, xSize, ySize, xCountChars, yCountChars, fillAttribute, flags;
            public short showWindow, reservedSize;
            public IntPtr reservedBytes, input, output, error;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInfo
        {
            public IntPtr process, thread;
            public uint processId, threadId;
        }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processSecurity,
            IntPtr threadSecurity, bool inheritHandles, uint flags, IntPtr environment, string directory,
            ref StartupInfo startup, out ProcessInfo process);
        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);

        /// <summary>自分の Bridge Job から離して IDE を起動し、取消や完了で IDE を終了させない</summary>
        internal static int launch_ide(string executable, string solution)
        {
            var startup = new StartupInfo();
            startup.cb = (uint)Marshal.SizeOf(startup);
            ProcessInfo process;
            var line = new StringBuilder(quote(executable) + " " + quote(solution));
            if (!CreateProcessW(executable, line, IntPtr.Zero, IntPtr.Zero, false, 0x01000000, IntPtr.Zero,
                System.IO.Path.GetDirectoryName(solution), ref startup, out process))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Visual Studio could not be launched independently.");
            CloseHandle(process.thread);
            CloseHandle(process.process);
            return checked((int)process.processId);
        }

        /// <summary>Windows CRT が引数を復元する Quote 規則を適用する</summary>
        private static string quote(string value)
        {
            var line = new StringBuilder("\"");
            int slashes = 0;
            foreach (char character in value)
            {
                if (character == '\\') { ++slashes; continue; }
                line.Append('\\', character == '"' ? slashes * 2 + 1 : slashes);
                line.Append(character);
                slashes = 0;
            }
            line.Append('\\', slashes * 2);
            return line.Append('"').ToString();
        }
    }

    [ComImport, Guid("00000016-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMessageFilter
    {
        [PreserveSig] int HandleInComingCall(int callType, IntPtr caller, int tickCount, IntPtr interfaceInfo);
        [PreserveSig] int RetryRejectedCall(IntPtr callee, int tickCount, int rejectType);
        [PreserveSig] int MessagePending(IntPtr callee, int tickCount, int pendingType);
    }

    /// <summary>VS が一時的に Busy の場合だけ有限時間再試行する。登録を呼出 STA の範囲に限定する</summary>
    internal sealed class ComMessageFilter : IMessageFilter, IDisposable
    {
        private readonly Stopwatch m_timer;
        private readonly IMessageFilter m_previous;
        internal ComMessageFilter(Stopwatch timer)
        {
            m_timer = timer;
            int result = NativeMethods.CoRegisterMessageFilter(this, out m_previous);
            Marshal.ThrowExceptionForHR(result);
        }
        public int HandleInComingCall(int callType, IntPtr caller, int tickCount, IntPtr interfaceInfo) { return 0; }
        public int RetryRejectedCall(IntPtr callee, int tickCount, int rejectType)
        {
            return (rejectType == 1 || rejectType == 2) && m_timer.ElapsedMilliseconds < 100000 ? 100 : -1;
        }
        public int MessagePending(IntPtr callee, int tickCount, int pendingType) { return 2; }
        public void Dispose()
        {
            IMessageFilter ignored;
            NativeMethods.CoRegisterMessageFilter(m_previous, out ignored);
        }
    }
}
