using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Cue.VisualStudioBridge
{
    /// <summary>Open Folder の Root を VS Shell から取得し、Solution の表示名で推測しない</summary>
    internal static class FolderWorkspace
    {
        private const int k_isInOpenFolderMode = -8044;
        private static readonly Guid s_solutionService = new Guid("7F7CD0DB-91EF-49DC-9FA9-02D128515DD4");

        /// <summary>DTE は呼出元の借用。QueryService が返す参照だけをこの呼出内で解放する</summary>
        internal static bool matches(object dte, string workspace)
        {
            var provider = dte as IOleServiceProvider;
            if (provider == null) return false;
            Guid service = s_solutionService;
            Guid contract = s_solutionService;
            IntPtr pointer = IntPtr.Zero;
            object shell = null;
            try
            {
                int result = provider.QueryService(ref service, ref contract, out pointer);
                if (result < 0 || pointer == IntPtr.Zero) return false;
                shell = Marshal.GetObjectForIUnknown(pointer);
                var solution = (IVsSolutionWorkspace)shell;
                object mode;
                if (solution.GetProperty(k_isInOpenFolderMode, out mode) < 0 || !(mode is bool) || !(bool)mode)
                    return false;
                string directory, solutionFile, userOptions;
                if (solution.GetSolutionInfo(out directory, out solutionFile, out userOptions) < 0 ||
                    String.IsNullOrWhiteSpace(directory)) return false;
                return String.Equals(Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar),
                    Path.GetFullPath(workspace).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase);
            }
            finally
            {
                if (shell != null && Marshal.IsComObject(shell)) Marshal.ReleaseComObject(shell);
                if (pointer != IntPtr.Zero) Marshal.Release(pointer);
            }
        }
    }

    [ComImport, Guid("6D5140C1-7436-11CE-8034-00AA006009FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IOleServiceProvider
    {
        [PreserveSig]
        int QueryService(ref Guid service, ref Guid contract, out IntPtr value);
    }

    // IVsSolution の先頭 20 Slot を公式順で宣言する
    // 未使用 Slot は呼ばない。Interop Assembly を配布せず Shell の既存 COM Proxy を利用する
    [ComImport, Guid("7F7CD0DB-91EF-49DC-9FA9-02D128515DD4"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IVsSolutionWorkspace
    {
        [PreserveSig] int GetProjectEnum();
        [PreserveSig] int CreateProject();
        [PreserveSig] int GenerateUniqueProjectName();
        [PreserveSig] int GetProjectOfGuid();
        [PreserveSig] int GetGuidOfProject();
        [PreserveSig]
        int GetSolutionInfo([MarshalAs(UnmanagedType.BStr)] out string directory,
            [MarshalAs(UnmanagedType.BStr)] out string solution, [MarshalAs(UnmanagedType.BStr)] out string userOptions);
        [PreserveSig] int AdviseSolutionEvents();
        [PreserveSig] int UnadviseSolutionEvents();
        [PreserveSig] int SaveSolutionElement();
        [PreserveSig] int CloseSolutionElement();
        [PreserveSig] int GetProjectOfProjref();
        [PreserveSig] int GetProjrefOfProject();
        [PreserveSig] int GetProjectInfoOfProjref();
        [PreserveSig] int AddVirtualProject();
        [PreserveSig] int GetItemOfProjref();
        [PreserveSig] int GetProjrefOfItem();
        [PreserveSig] int GetItemInfoOfProjref();
        [PreserveSig] int GetProjectOfUniqueName();
        [PreserveSig] int GetUniqueNameOfProject();
        [PreserveSig]
        int GetProperty(int property, [MarshalAs(UnmanagedType.Struct)] out object value);
    }
}
