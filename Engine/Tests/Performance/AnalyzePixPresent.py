"""PIX Timing Capture を読み取り専用で開き、対象 Process の Present Stack を集計する。"""

import argparse
import collections
import ctypes
from ctypes import wintypes
import json
import os
import pathlib
import sqlite3
import struct


class SymbolInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wintypes.ULONG), ("TypeIndex", wintypes.ULONG),
                ("Reserved", ctypes.c_ulonglong * 2), ("Index", wintypes.ULONG), ("Size", wintypes.ULONG),
                ("ModBase", ctypes.c_ulonglong), ("Flags", wintypes.ULONG), ("Value", ctypes.c_ulonglong),
                ("Address", ctypes.c_ulonglong), ("Register", wintypes.ULONG), ("Scope", wintypes.ULONG),
                ("Tag", wintypes.ULONG), ("NameLen", wintypes.ULONG), ("MaxNameLen", wintypes.ULONG),
                ("Name", ctypes.c_char * 1)]


def local_path(path):
    if path.startswith("\\Device\\HarddiskVolume3\\"):
        return "C:/" + path[len("\\Device\\HarddiskVolume3\\"):].replace("\\", "/")
    if path.lower().startswith("\\systemroot\\"):
        return "C:/Windows/" + path[len("\\SystemRoot\\"):].replace("\\", "/")
    return path


class Symbols:
    def __init__(self, pix_dir, cache, images):
        self.directory = os.add_dll_directory(str(pix_dir))
        self.dll = ctypes.WinDLL(str(pix_dir / "dbghelp.dll"), use_last_error=True)
        # Capture の仮想 Address Space を解析する独立 Session とし、現在 Process と混同しない
        self.process = wintypes.HANDLE(0x43554501)
        self.dll.SymSetOptions.argtypes = [wintypes.DWORD]
        # Symbol Server の対話を抑止し、取得できない Symbol は未解決として残す
        self.dll.SymSetOptions(0x2 | 0x4 | 0x80000)
        self.dll.SymInitialize.argtypes = [wintypes.HANDLE, ctypes.c_char_p, wintypes.BOOL]
        search = f"srv*{cache.resolve()}*https://msdl.microsoft.com/download/symbols".encode()
        if not self.dll.SymInitialize(self.process, search, False):
            raise ctypes.WinError(ctypes.get_last_error())
        self.dll.SymLoadModuleEx.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.c_char_p, ctypes.c_char_p,
                                           ctypes.c_ulonglong, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
        self.dll.SymLoadModuleEx.restype = ctypes.c_ulonglong
        self.dll.SymFromAddr.argtypes = [wintypes.HANDLE, ctypes.c_ulonglong,
                                        ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(SymbolInfo)]
        self.dll.SymCleanup.argtypes = [wintypes.HANDLE]
        self.images = images
        self.loaded = set()
        self.names = {}
        self.flags = {}

    def resolve(self, address):
        if address in self.names:
            return self.names[address]
        module = next((m for m in self.images if m[0] <= address < m[0] + m[1]), None)
        if not module:
            result = f"unknown!0x{address:x}"
        else:
            base, size, path = module
            name = pathlib.PureWindowsPath(path).name
            if base not in self.loaded:
                self.loaded.add(base)
                disk_path = local_path(path)
                print("Loading symbols:", name, flush=True)
                self.dll.SymLoadModuleEx(self.process, None, disk_path.encode(), name.encode(), base, size, None, 0)
            buffer = ctypes.create_string_buffer(ctypes.sizeof(SymbolInfo) + 2048)
            symbol = ctypes.cast(buffer, ctypes.POINTER(SymbolInfo))
            symbol.contents.SizeOfStruct = ctypes.sizeof(SymbolInfo)
            symbol.contents.MaxNameLen = 2048
            displacement = ctypes.c_ulonglong()
            if self.dll.SymFromAddr(self.process, address, ctypes.byref(displacement), symbol):
                self.flags[address] = symbol.contents.Flags
                text = ctypes.string_at(ctypes.addressof(buffer) + SymbolInfo.Name.offset,
                                        symbol.contents.NameLen).decode("utf-8", errors="replace")
                # Export の直前名は Driver 内部関数の名前を表さないため RVA だけを記す
                result = (f"{name}!0x{address-base:x}" if symbol.contents.Flags & 0x200
                          else f"{name}!{text}+0x{displacement.value:x}")
            else:
                result = f"{name}!0x{address-base:x}"
        self.names[address] = result
        return result

    def close(self):
        self.dll.SymCleanup(self.process)
        self.directory.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--pix", type=pathlib.Path, default=pathlib.Path("C:/Program Files/Microsoft PIX/2602.25"))
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    directory = os.add_dll_directory(str(args.pix))
    connection = sqlite3.connect(args.capture.resolve().as_uri() + "?mode=ro", uri=True)
    connection.enable_load_extension(True)
    connection.execute("select load_extension(?,?)", (str(args.pix / "PixStorage.dll"), "sqlite3_batchexpand_init"))
    facts = dict(connection.execute("select Id,Value from CaptureFacts"))
    pid, start, stop = facts[4], facts[2], facts[24]
    threads = list(connection.execute("select t.Id,t.ProcThreadId from Threads t join Processes p "
                                     "on p.Id=t.ProcessRowId where p.ProcessId=?", (pid,)))
    thread_ids = {row[1] for row in threads}
    render_row = connection.execute("select q.ThreadId,count(*) n from ApiQueueExecution q join Threads t "
                                    "on t.Id=q.ThreadId join Processes p on p.Id=t.ProcessRowId "
                                    "where p.ProcessId=? group by q.ThreadId order by n desc limit 1", (pid,)).fetchone()
    render_thread = dict(threads)[render_row[0]]
    stacks = {r[0]: struct.unpack("<" + "Q" * r[1], r[2]) for r in connection.execute("select * from Stacks")}
    stack_events = collections.defaultdict(list)
    for tid, blob, count in connection.execute("select OSThreadId,StackEventData,EventCount from StackEvents"):
        if tid not in {t & 0xffffffff for t in thread_ids}:
            continue
        if len(blob) != count * 16:
            raise ValueError("Unsupported StackEventData layout")
        for timestamp, stack_id in struct.iter_unpack("<QQ", blob):
            if start <= timestamp <= stop:
                stack_events[(tid, timestamp)].append(stack_id)
    samples = list(connection.execute("select Timestamp from CpuSample where ProcThreadId=? "
                                     "and Timestamp between ? and ?", (render_thread, start, stop)))
    sample_stacks = []
    for (timestamp,) in samples:
        addresses = []
        for stack_id in stack_events.get((render_thread & 0xffffffff, timestamp), []):
            addresses.extend(stacks[stack_id])
        sample_stacks.append((timestamp, tuple(dict.fromkeys(addresses))))
    images = [(base & 0xffffffffffffffff, size, path) for base, size, path in connection.execute(
        "select i.PELoadAddress,i.LoadSize,s.Value from Images i join Strings s on s.Id=i.FilePathId "
        "where i.OSProcessId in (0,?)", (pid,))]
    cache = args.output.parent / "Symbols"
    cache.mkdir(exist_ok=True)
    symbols = Symbols(args.pix, cache, images)
    # 自分の実行 File 内の Frame を先に解決し、Presentを含むSampleだけ他Moduleへ展開する
    own = [m for m in images if pathlib.PureWindowsPath(m[2]).name == "CueEditorTimingProbe.exe"]
    present_samples = []
    own_functions = collections.Counter()
    for timestamp, addresses in sample_stacks:
        names = [symbols.resolve(a) for a in addresses if any(m[0] <= a < m[0] + m[1] for m in own)]
        own_functions.update(names)
        if any("DX12SwapChain::present" in n for n in names):
            present_samples.append((timestamp, addresses))
    functions, leaves, modules = collections.Counter(), collections.Counter(), collections.Counter()
    inclusive_modules = collections.Counter()
    selected = []
    for timestamp, addresses in present_samples:
        names = [symbols.resolve(a) for a in addresses]
        functions.update(set(n.rsplit("+0x", 1)[0] for n in names))
        inclusive_modules.update(set(n.split("!")[0] for n in names))
        if names:
            leaves.update([names[0].rsplit("+0x", 1)[0]])
            modules.update([names[0].split("!")[0]])
        selected.append({"timestamp": timestamp, "stack": names})
    # ETW の Stack 取得や割り込みも Present 中の Sample に含まれるため、別に数える
    instrumentation_samples = sum(any("EtwpApcPoolThunk" in n or "EtwpTraceStackWalk" in n
                                      for n in sample["stack"]) for sample in selected)
    dpc_samples = sum(any("KiExecuteAllDpcs" in n or "KiRetireDpcList" in n
                         for n in sample["stack"]) for sample in selected)
    submissions = connection.execute("select count(*) from ApiQueueExecution where ThreadId=? "
                                     "and SubmitTimestamp between ? and ?",
                                     (render_row[0], start, stop)).fetchone()[0]
    # Switch Out の Stack と、同じThreadの次の Switch In までを対応させる
    switches = list(connection.execute("select Timestamp,FromProcThreadId,ToProcThreadId,FromThreadWaitReason "
                                      "from ContextSwitch where (FromProcThreadId=? or ToProcThreadId=?) "
                                      "and Timestamp between ? and ? order by Timestamp",
                                      (render_thread, render_thread, start, stop)))
    pending = None
    matched_out_stacks = 0
    present_waits = []
    for timestamp, from_thread, to_thread, reason in switches:
        if to_thread == render_thread and pending:
            present_waits.append({"out_ns": pending[0], "in_ns": timestamp,
                                  "inactive_ns": timestamp - pending[0], "wait_reason": pending[1],
                                  "stack": pending[2]})
            pending = None
        if from_thread == render_thread:
            addresses = []
            for stack_id in stack_events.get((render_thread & 0xffffffff, timestamp), []):
                addresses.extend(stacks[stack_id])
            if addresses:
                matched_out_stacks += 1
            own_names = [symbols.resolve(a) for a in addresses if any(m[0] <= a < m[0]+m[1] for m in own)]
            if any("DX12SwapChain::present" in n for n in own_names):
                pending = (timestamp, reason, [symbols.resolve(a) for a in addresses])
    stats = list(connection.execute("select max(LostEtwEventCount),max(LostEtwBufferCount) from CaptureStats"))[0]
    result = {"capture": str(args.capture), "pid": pid, "render_tid": render_thread & 0xffffffff,
              "first_reliable_ns": start, "stop_ns": stop, "render_samples": len(samples),
              "reliable_seconds": (stop-start) / 1e9, "render_queue_submissions": submissions,
              "samples_with_stack": sum(bool(a) for _, a in sample_stacks), "present_samples": len(selected),
              "present_instrumentation_stack_samples": instrumentation_samples,
              "present_dpc_stack_samples": dpc_samples,
              "etw_lost_events": stats[0], "etw_lost_buffers": stats[1],
              "present_context_switches": present_waits,
              "render_switch_outs": sum(r[1] == render_thread for r in switches),
              "switch_outs_with_matched_stack": matched_out_stacks,
              "present_inactive_ns": (sum(r["inactive_ns"] for r in present_waits) if matched_out_stacks else None),
              "present_inclusive_modules": inclusive_modules.most_common(),
              "present_leaf_modules": modules.most_common(), "present_leaf_functions": leaves.most_common(30),
              "present_inclusive_functions": functions.most_common(50),
              "render_own_functions": own_functions.most_common(35), "present_stacks": selected,
              "resolved_addresses": symbols.names, "symbol_flags": symbols.flags}
    result["method"] = ("Read-only PIX SQLite query, target Render thread only. CPU samples are not "
                        "wall-time measurements. Inclusive counts overlap. StackEvents 16-byte pairs "
                        "are empirically matched by exact thread/timestamp; this layout is version-specific. "
                        "No matched switch-out stack means inactive duration is unknown, not zero. "
                        "NVIDIA private symbols are unavailable; export-only symbols are shown as RVA. "
                        "Device-path mapping assumes this machine's C: volume.")
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    symbols.close()
    connection.close()
    directory.close()
    print(json.dumps({k:v for k,v in result.items() if k not in ("present_stacks", "resolved_addresses", "symbol_flags", "present_context_switches")},
                     ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
