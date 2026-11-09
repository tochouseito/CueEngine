using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Threading;
using Microsoft.CSharp.RuntimeBinder;

namespace Cue.VisualStudioBridge
{
    /// <summary>一回の IDE 操作を所有する。既存の別 Workspace は閉じず、IDE 自身は終了させない</summary>
    internal sealed class VisualStudioAutomation : IDisposable
    {
        private readonly Stopwatch m_timer = Stopwatch.StartNew();
        private readonly ComMessageFilter m_filter;
        private object m_dte;

        internal VisualStudioAutomation() { m_filter = new ComMessageFilter(m_timer); }

        /// <summary>正確な Workspace / PID を選択してから Source 表示または Native 接続を実行する</summary>
        internal int execute(BridgeRequest request)
        {
            int idePid;
            m_dte = find_running(request.Workspace, request.IdeProcessId, 0, out idePid);
            if (m_dte == null)
            {
                int launched = NativeMethods.launch_ide(find_devenv(), request.Workspace);
                do
                {
                    Thread.Sleep(100);
                    m_dte = find_running(request.Workspace, launched, launched, out idePid);
                } while (m_dte == null && m_timer.ElapsedMilliseconds < 100000);
                if (m_dte == null) throw new TimeoutException("Visual Studio did not publish the requested workspace.");
            }
            dynamic dte = m_dte;
            dte.UserControl = true;
            dte.MainWindow.Visible = true;
            if (request.Command == "open-source")
            {
                dynamic window = dte.ItemOperations.OpenFile(request.Source, "{00000000-0000-0000-0000-000000000000}");
                window.Activate();
                dynamic selection = window.Document.Selection;
                selection.GotoLine(request.SourceLine, false);
            }
            else if (request.Command == "attach-debugger")
            {
                using (Process target = Process.GetProcessById(request.TargetProcessId))
                {
                    if (target.HasExited || !same_path(target.MainModule.FileName, request.ExpectedExecutable))
                        throw new InvalidOperationException("The target Play Host no longer matches the request.");
                    // Process Handle を保持し、操作中に PID を再利用させない
                    IntPtr held = target.Handle;
                    if (!is_attached(dte, request.TargetProcessId))
                    {
                        bool found = false;
                        foreach (dynamic process in dte.Debugger.LocalProcesses)
                        {
                            if (Convert.ToInt32(process.ProcessID, CultureInfo.InvariantCulture) != request.TargetProcessId) continue;
                            try { process.Attach2("Native"); }
                            catch (RuntimeBinderException) { process.Attach(); }
                            catch (COMException error) when (error.ErrorCode == unchecked((int)0x80020003) || error.ErrorCode == unchecked((int)0x80020006))
                            { process.Attach(); }
                            found = true;
                            break;
                        }
                        if (!found) throw new InvalidOperationException("The requested Play Host was not found in Visual Studio.");
                    }
                    while (!is_attached(dte, request.TargetProcessId))
                    {
                        if (target.HasExited || m_timer.ElapsedMilliseconds >= 100000)
                            throw new TimeoutException("Native debugger attachment was not confirmed.");
                        Thread.Sleep(100);
                    }
                    GC.KeepAlive(held);
                }
            }
            return idePid;
        }

        /// <summary>接続要求の返却値だけでなく、IDE の対象一覧にも指定 PID が存在することを確認する</summary>
        private static bool is_attached(dynamic dte, int pid)
        {
            foreach (dynamic process in dte.Debugger.DebuggedProcesses)
                if (Convert.ToInt32(process.ProcessID, CultureInfo.InvariantCulture) == pid) return true;
            return false;
        }

        /// <summary>Workspace の絶対 Path と実行中 VS の PID を照合し、同名 Project の誤選択を防ぐ</summary>
        private object find_running(string workspace, int preferredPid, int requiredPid, out int selectedPid)
        {
            selectedPid = 0;
            IRunningObjectTable table = null;
            IEnumMoniker enumerator = null;
            object selected = null;
            bool isAmbiguous = false;
            try
            {
                Marshal.ThrowExceptionForHR(NativeMethods.GetRunningObjectTable(0, out table));
                table.EnumRunning(out enumerator);
                var monikers = new IMoniker[1];
                while (enumerator.Next(1, monikers, IntPtr.Zero) == 0)
                {
                    IBindCtx context = null;
                    object candidate = null;
                    try
                    {
                        Marshal.ThrowExceptionForHR(NativeMethods.CreateBindCtx(0, out context));
                        monikers[0].GetDisplayName(context, null, out string display);
                        int colon = display.LastIndexOf(':');
                        if (!display.StartsWith("!VisualStudio.DTE.", StringComparison.OrdinalIgnoreCase) &&
                            !display.StartsWith("VisualStudio.DTE.", StringComparison.OrdinalIgnoreCase)) continue;
                        if (colon < 0 || !Int32.TryParse(display.Substring(colon + 1), out int pid) || pid <= 0 ||
                            (requiredPid != 0 && pid != requiredPid)) continue;
                        table.GetObject(monikers[0], out candidate);
                        if (!FolderWorkspace.matches(candidate, workspace)) continue;
                        if (selected != null && selectedPid != pid)
                        {
                            if (selectedPid == preferredPid) continue;
                            if (pid != preferredPid) { isAmbiguous = true; continue; }
                            Marshal.ReleaseComObject(selected);
                        }
                        else if (selected != null)
                        {
                            continue;
                        }
                        selected = candidate;
                        selectedPid = pid;
                        candidate = null;
                    }
                    catch (UnauthorizedAccessException) { }
                    catch (COMException error) when (error.ErrorCode != unchecked((int)0x80010001) && error.ErrorCode != unchecked((int)0x8001010A))
                    { /* 終了途中の別 VS / ROT Entry は候補として公開しない */ }
                    finally
                    {
                        if (candidate != null && Marshal.IsComObject(candidate)) Marshal.ReleaseComObject(candidate);
                        if (context != null) Marshal.ReleaseComObject(context);
                        if (monikers[0] != null) { Marshal.ReleaseComObject(monikers[0]); monikers[0] = null; }
                    }
                }
                if (isAmbiguous && selectedPid != preferredPid)
                    throw new InvalidOperationException("Multiple Visual Studio instances have this workspace open.");
                return selected;
            }
            catch
            {
                if (selected != null) Marshal.ReleaseComObject(selected);
                throw;
            }
            finally
            {
                if (enumerator != null) Marshal.ReleaseComObject(enumerator);
                if (table != null) Marshal.ReleaseComObject(table);
            }
        }

        /// <summary>VS Installer の登録情報から IDE を選ぶ。Shell やユーザー入力を Command にしない</summary>
        private static string find_devenv()
        {
            string installer = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Microsoft Visual Studio", "Installer", "vswhere.exe");
            if (File.Exists(installer))
            {
                using (Process query = Process.Start(new ProcessStartInfo(installer,
                    "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property productPath")
                    { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true }))
                {
                    if (!query.WaitForExit(5000)) { query.Kill(); throw new TimeoutException("Visual Studio discovery timed out."); }
                    string result = query.StandardOutput.ReadToEnd().Trim();
                    if (query.ExitCode == 0 && File.Exists(result) && String.Equals(Path.GetFileName(result), "devenv.exe", StringComparison.OrdinalIgnoreCase))
                        return result;
                }
            }
            foreach (string root in new[] { Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86) })
            {
                string folder = Path.Combine(root, "Microsoft Visual Studio");
                if (!Directory.Exists(folder)) continue;
                var versions = new List<string>(Directory.GetDirectories(folder));
                versions.Sort((a, b) => StringComparer.OrdinalIgnoreCase.Compare(b, a));
                foreach (string version in versions)
                    foreach (string edition in Directory.GetDirectories(version))
                    {
                        string candidate = Path.Combine(edition, "Common7", "IDE", "devenv.exe");
                        if (File.Exists(candidate)) return candidate;
                    }
            }
            throw new FileNotFoundException("A Visual Studio installation with C++ tools was not found.");
        }

        /// <summary>文字列の Caption ではなく FileSystem の絶対 Path を比較する</summary>
        private static bool same_path(string first, string second)
        {
            return String.Equals(Path.GetFullPath(first).TrimEnd(Path.DirectorySeparatorChar),
                Path.GetFullPath(second).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase);
        }
        /// <summary>COM 借用だけを解放する。Visual Studio.Quit は呼ばない</summary>
        public void Dispose()
        {
            if (m_dte != null && Marshal.IsComObject(m_dte)) Marshal.ReleaseComObject(m_dte);
            m_dte = null;
            m_filter.Dispose();
        }
    }
}
