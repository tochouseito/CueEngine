using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace Cue.VisualStudioBridge
{
    /// <summary>一要求だけを所有し、Engine / Runtime の型を共有しない</summary>
    internal sealed class BridgeRequest
    {
        public string Command = "none";
        public string Workspace = "";
        public string Source = "";
        public string ExpectedExecutable = "";
        public ulong RequestId;
        public int IdeProcessId;
        public int TargetProcessId;
        public int SourceLine = 1;

        /// <summary>絶対 Path の通常 File を検証し、全親の Reparse Point を拒否する</summary>
        public static string validate_file(string value)
        {
            if (String.IsNullOrWhiteSpace(value) || !Path.IsPathRooted(value) || Path.GetPathRoot(value).Length < 3)
                throw new ArgumentException("An absolute file path is required.");
            string full = Path.GetFullPath(value);
            if (!File.Exists(full) || (File.GetAttributes(full) & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
                throw new ArgumentException("The requested file is missing or is a link.");
            for (DirectoryInfo parent = Directory.GetParent(full); parent != null; parent = parent.Parent)
                if ((parent.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new ArgumentException("A parent directory is a link.");
            return full;
        }

        /// <summary>ゲーム Root の通常 Directory と CMake 入口を検証し、Link を辿らない</summary>
        public static string validate_workspace(string value)
        {
            if (String.IsNullOrWhiteSpace(value) || !Path.IsPathRooted(value) || Path.GetPathRoot(value).Length < 3)
                throw new ArgumentException("An absolute workspace path is required.");
            string full = Path.GetFullPath(value);
            if (!Directory.Exists(full)) throw new ArgumentException("The requested workspace is missing.");
            for (var current = new DirectoryInfo(full); current != null; current = current.Parent)
                if ((current.Attributes & FileAttributes.ReparsePoint) != 0)
                    throw new ArgumentException("The workspace or a parent directory is a link.");
            validate_file(Path.Combine(full, "CMakeLists.txt"));
            return full;
        }

        /// <summary>既知の個別引数だけを受け取り、Shell の文字列として解釈しない</summary>
        public static BridgeRequest parse(string[] args)
        {
            var values = new Dictionary<string, string>(StringComparer.Ordinal);
            var known = new HashSet<string>(new[] { "--command", "--workspace", "--request-id", "--source", "--line", "--process-id", "--expected-executable", "--ide-process-id" }, StringComparer.Ordinal);
            if (args.Length == 0 || args.Length > 16 || args.Length % 2 != 0)
                throw new ArgumentException("Invalid bridge arguments.");
            for (int index = 0; index < args.Length; index += 2)
            {
                if (!known.Contains(args[index]) || values.ContainsKey(args[index]) || String.IsNullOrWhiteSpace(args[index + 1]))
                    throw new ArgumentException("Unknown, duplicate or empty bridge argument.");
                values.Add(args[index], args[index + 1]);
            }
            var request = new BridgeRequest();
            if (!values.TryGetValue("--command", out request.Command) ||
                !values.TryGetValue("--workspace", out request.Workspace) ||
                !values.TryGetValue("--request-id", out string id) ||
                !UInt64.TryParse(id, NumberStyles.None, CultureInfo.InvariantCulture, out request.RequestId) || request.RequestId == 0)
                throw new ArgumentException("Missing command, workspace or request identifier.");
            if (request.Command != "open-folder" && request.Command != "open-source" && request.Command != "attach-debugger")
                throw new ArgumentException("Unsupported bridge command.");
            request.Workspace = validate_workspace(request.Workspace);
            if (values.TryGetValue("--ide-process-id", out string ide) &&
                (!Int32.TryParse(ide, NumberStyles.None, CultureInfo.InvariantCulture, out request.IdeProcessId) || request.IdeProcessId <= 0))
                throw new ArgumentException("Invalid IDE process identifier.");
            if (request.Command == "open-source")
            {
                if (!values.TryGetValue("--source", out request.Source) || !values.TryGetValue("--line", out string line) ||
                    !Int32.TryParse(line, NumberStyles.None, CultureInfo.InvariantCulture, out request.SourceLine) || request.SourceLine < 1 || request.SourceLine > 10000000)
                    throw new ArgumentException("Invalid source location.");
                request.Source = validate_file(request.Source);
            }
            else if (values.ContainsKey("--source") || values.ContainsKey("--line"))
                throw new ArgumentException("Source arguments do not belong to this command.");
            if (request.Command == "attach-debugger")
            {
                if (!values.TryGetValue("--process-id", out string pid) ||
                    !Int32.TryParse(pid, NumberStyles.None, CultureInfo.InvariantCulture, out request.TargetProcessId) || request.TargetProcessId <= 0 ||
                    !values.TryGetValue("--expected-executable", out request.ExpectedExecutable))
                    throw new ArgumentException("Invalid target process.");
                request.ExpectedExecutable = validate_file(request.ExpectedExecutable);
                if (!String.Equals(Path.GetFileName(request.ExpectedExecutable), "CuePlayHost.exe", StringComparison.OrdinalIgnoreCase))
                    throw new ArgumentException("Only the Play Host can be attached.");
            }
            else if (values.ContainsKey("--process-id") || values.ContainsKey("--expected-executable"))
                throw new ArgumentException("Target arguments do not belong to this command.");
            return request;
        }
    }

    internal static class Program
    {
        /// <summary>COM 操作を STA に閉じ込め、成功・失敗とも一つの構造応答として返す</summary>
        [STAThread]
        private static int Main(string[] args)
        {
            Console.OutputEncoding = new UTF8Encoding(false);
            BridgeRequest request = null;
            int ide = 0;
            try
            {
                request = BridgeRequest.parse(args);
                using (var automation = new VisualStudioAutomation())
                    ide = automation.execute(request);
                write_result(request, true, ide, "Completed");
                return 0;
            }
            catch (Exception error)
            {
                write_result(request, false, ide, error.GetType().Name + ": " + error.Message);
                return 1;
            }
        }
        /// <summary>改行・NUL を除去し、診断から追加 Protocol 行を作れないようにする</summary>
        private static void write_result(BridgeRequest request, bool success, int ide, string message)
        {
            string clean = new string(message.Take(2048).Select(c => Char.IsControl(c) ? ' ' : c).ToArray());
            Console.Write("formatVersion=1\nrequestId=" + (request == null ? 0UL : request.RequestId).ToString(CultureInfo.InvariantCulture) +
                "\noperation=" + (request == null ? "none" : request.Command) +
                "\nsuccess=" + (success ? "true" : "false") +
                "\nideProcessId=" + ide.ToString(CultureInfo.InvariantCulture) +
                "\ntargetProcessId=" + (request == null ? 0 : request.TargetProcessId).ToString(CultureInfo.InvariantCulture) +
                "\nmessage=" + clean + "\n");
        }
    }
}
