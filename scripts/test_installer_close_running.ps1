param(
    [string]$Iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
# Regression for closing a running Pulse before an upgrade. Pulse 1.0.48 and
# older did not answer the update handshake, so every upgrade from them
# stopped with "Pulse is busy" even with the window closed to the tray.
#
# The closing code is taken verbatim from installer/PulseSetup.iss and run in
# a harness Setup that returns False from InitializeSetup, so nothing is
# installed. It acts on stand-in pulse.exe processes in build/; an installed
# Pulse lives in another directory and is never touched.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$source = Get-Content -LiteralPath (Join-Path $repo 'installer/PulseSetup.iss') -Raw -Encoding UTF8
$out = Join-Path $repo 'build/installer-close-test'
$out = [IO.Path]::GetFullPath($out)
if (!$out.StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'build')) + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Harness output must stay inside build.' }
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path "$out\app", "$out\other" | Out-Null

$failures = 0
function Check([bool]$ok, [string]$label) {
    if ($ok) { Write-Output "[PASS] $label" } else { Write-Output "[FAIL] $label"; $script:failures++ }
}

# --- Control flow on the script text ----------------------------------------
$prepare = [regex]::Match($source, '(?s)function PrepareToInstall\(.*?\r?\nend;').Value
if (!$prepare) { throw 'PrepareToInstall not found; update the harness.' }
Check (([regex]::Matches($prepare, 'Result := PulseCloseError;')).Count -eq 2) 'both close attempts report the specific failure'
Check ($source -match '(?s)function ClosePulseForUpdate:.*ClosePulseInstances\(Dirs, 10\).*MB_RETRYCANCEL') 'busy Pulse is waited for and setup offers Retry'
Check ($source -match 'CloseApplications=no') 'Restart Manager still cannot bypass the handshake'

# --- Stand-in pulse.exe -------------------------------------------------------
# legacy: ignores the handshake like 1.0.48 (reply 0), logs sign-out messages.
# modern: answers busy (2) until the given seconds pass, then accepts (1).
$fake = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
static class FakePulse {
    delegate IntPtr WndProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASS { public uint style; public WndProc proc; public int cls, wnd; public IntPtr inst, icon, cursor, bg; public string menu, name; }
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr h; public uint m; public IntPtr w, l; public uint t; public int x, y; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassW(ref WNDCLASS c);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] static extern void PostQuitMessage(int code);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern uint RegisterWindowMessageW(string s);
    [DllImport("user32.dll")] static extern int GetMessageW(out MSG m, IntPtr h, uint a, uint b);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG m);
    static string mode, log;
    static uint update;
    static DateTime busyUntil;
    static WndProc keep;
    static void Note(string s) { File.AppendAllText(log, s + "\r\n"); }
    static IntPtr Proc(IntPtr h, uint m, IntPtr w, IntPtr l) {
        if (m == update && mode == "modern") {
            if (DateTime.UtcNow < busyUntil) { Note("busy"); return (IntPtr)2; }
            Note("accepted"); DestroyWindow(h); return (IntPtr)1;
        }
        if (m == update && mode == "savefailed") { Note("savefailed"); return (IntPtr)3; }
        if (m == 0x11) { Note("queryendsession " + l); return (IntPtr)1; }
        if (m == 0x16) { Note("endsession " + w); return IntPtr.Zero; }
        if (m == 0x02) { PostQuitMessage(0); return IntPtr.Zero; }
        return DefWindowProcW(h, m, w, l);
    }
    static void Main(string[] a) {
        mode = a[0]; log = a[1];
        busyUntil = DateTime.UtcNow.AddSeconds(a.Length > 2 ? double.Parse(a[2]) : 0);
        update = RegisterWindowMessageW("Pulse.PrepareUpdateShutdown.v1");
        IntPtr inst = Marshal.GetHINSTANCE(typeof(FakePulse).Module);
        keep = Proc;
        var c = new WNDCLASS { proc = keep, inst = inst, name = "PulseMainWindow" };
        RegisterClassW(ref c);
        // No WS_VISIBLE: like a window closed to the tray.
        CreateWindowExW(0, "PulseMainWindow", "Pulse", 0, 0, 0, 200, 200, IntPtr.Zero, IntPtr.Zero, inst, IntPtr.Zero);
        Note("ready");
        MSG msg;
        while (GetMessageW(out msg, IntPtr.Zero, 0, 0) > 0) DispatchMessageW(ref msg);
        Note("exit");
    }
}
'@
$exe = "$out\app\pulse.exe"
Add-Type -TypeDefinition $fake -OutputAssembly $exe -OutputType WindowsApplication
Copy-Item -LiteralPath $exe -Destination "$out\other\pulse.exe"

# --- Harness Setup ---------------------------------------------------------
$block = [regex]::Match($source, '(?s)\{ --- Closing running Pulse -.*?\{ --- end of closing running Pulse --- \}').Value
if (!$block) { throw 'Closing block markers not found; update the harness.' }
if ($block -match "ExpandConstant\('\{app\}|Reg(Query|Write|Delete)\w*\(") { throw 'Closing block must not use {app} or the registry.' }
$harness = @'
[Setup]
AppName=Pulse close-running regression harness
AppVersion=1
DefaultDirName={tmp}\PulseCloseHarnessNeverInstalled
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputBaseFilename=installer-close-harness
Compression=none
[CustomMessages]
PulseUpdateBusy=busy
PulseUpdateCloseFailed=failed
PulseUpdateWaitBusy=retry
[Code]
var
  Report: String;
  PulseCloseError: String;
function GetTickCount: DWORD;
  external 'GetTickCount@kernel32.dll stdcall';
procedure Check(Condition: Boolean; const LabelText: String);
begin
  if Condition then Report := Report + '[PASS] ' + LabelText + #13#10
  else Report := Report + '[FAIL] ' + LabelText + #13#10;
end;
function PulseInstallDirectories: String;
begin
  Result := ExpandConstant('{param:dir}');
end;
'@ + "`r`n" + $block + "`r`n" + [regex]::Match($source, '(?s)function ClosePulseForUpdate: Boolean;.*?\r?\nend;').Value + "`r`n" + @'
function InitializeSetup: Boolean;
var
  Started: DWORD;
  State: Integer;
begin
  if ExpandConstant('{param:case}') = 'pure' then
  begin
    Check(PulsePathInDirectories('C:\Program Files\Pulse\pulse.exe', 'D:\X|c:\program files\pulse'), 'path in second directory, any case');
    Check(PulsePathInDirectories('C:\Program Files\Pulse\pulse.exe', 'C:\Program Files\Pulse\'), 'trailing backslash on directory');
    Check(not PulsePathInDirectories('C:\Program Files\Pulse2\pulse.exe', 'C:\Program Files\Pulse'), 'sibling directory with same prefix is outside');
    Check(not PulsePathInDirectories('D:\dev\pulse\build\pulse.exe', 'C:\Program Files\Pulse'), 'development build is outside');
    Check(PulsePathInDirectories('', 'C:\Program Files\Pulse'), 'unreadable path still blocks');
    Check(ParsePulseProcessIds('"pulse.exe","120","Console","1","9 K"'#13#10'"Pulse.exe","7","Console","1","9 K"'#13#10) = '120|7|', 'process ids from tasklist rows');
    Check(ParsePulseProcessIds('INFO: No tasks are running which match the specified criteria.'#13#10) = '', 'no process rows');
    Check(ParsePulseProcessIds('"pulse_shell.exe","5","Console","1","9 K"') = '', 'other images ignored');
  end else
  begin
    Started := GetTickCount;
    if ExpandConstant('{param:case}') = 'autoupdate' then
    begin
      if ClosePulseForUpdate then State := PulseCloseDone else State := PulseCloseFailed;
    end else
      State := ClosePulseInstances(ExpandConstant('{param:dir}'), StrToInt(ExpandConstant('{param:busy}')));
    Report := Report + 'state=' + IntToStr(State) + #13#10 + 'ms=' + IntToStr(GetTickCount - Started) + #13#10;
  end;
  SaveStringToFile(ExpandConstant('{param:report}'), Report, False);
  { Returning False prevents installation, file deployment, and uninstall registration. }
  Result := False;
end;
'@
$iss = Join-Path $out 'harness.iss'
Set-Content -LiteralPath $iss -Value $harness -Encoding utf8
& $Iscc "/Q" "/O$out" $iss
if ($LASTEXITCODE -ne 0) { throw 'Pascal harness compilation failed.' }
$setup = Join-Path $out 'installer-close-harness.exe'

function Run-Harness([string]$case, [int]$busy, [bool]$update = $false) {
    $report = "$out\report-$case.txt"
    $argv = '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$out\setup-$case.log", "/case=$case", "/dir=$out\app", "/busy=$busy", "/report=$report"
    if ($update) { $argv += '/PULSEUPDATE=1' }
    $p = Start-Process -FilePath $setup -ArgumentList $argv -WindowStyle Hidden -Wait -PassThru
    if (!(Test-Path -LiteralPath $report)) { throw "Harness $case produced no report (exit $($p.ExitCode))." }
    $text = Get-Content -LiteralPath $report -Raw
    $state = if ($text -match 'state=(\d+)') { [int]$Matches[1] } else { -1 }
    $ms = if ($text -match 'ms=(\d+)') { [int]$Matches[1] } else { -1 }
    return [pscustomobject]@{ Text = $text; State = $state; Ms = $ms }
}
function Start-Fake([string]$dir, [string]$mode, [string]$log, [double]$busy = 0) {
    $p = Start-Process -FilePath "$out\$dir\pulse.exe" -ArgumentList $mode, $log, $busy -WindowStyle Hidden -PassThru
    for ($i = 0; $i -lt 50 -and !((Test-Path $log) -and ((Get-Content $log -Raw) -match 'ready')); $i++) { Start-Sleep -Milliseconds 100 }
    return $p
}
function Log-Of([string]$log) { if (Test-Path $log) { Get-Content $log -Raw } else { '' } }

# --- Pure helpers --------------------------------------------------------------
$pure = Run-Harness 'pure' 0
Write-Output $pure.Text.TrimEnd()
if ($pure.Text -match '\[FAIL\]') { $failures++ }
Check (($pure.Text -split '\[PASS\]').Count -eq 9) 'all helper assertions completed'

try {
    # --- Older Pulse without the handshake, plus a copy elsewhere ----------------
    $legacy = Start-Fake 'app' 'legacy' "$out\legacy.log"
    $other = Start-Fake 'other' 'legacy' "$out\other.log"
    $r = Run-Harness 'legacy' 2
    $log = Log-Of "$out\legacy.log"
    Check ($r.State -eq 0) "older Pulse closed without the handshake (state $($r.State), $($r.Ms) ms)"
    Check ($log -match 'queryendsession 1' -and $log -match 'endsession 1') 'it got the sign-out messages first, so it saved its session'
    Check ($legacy.WaitForExit(3000)) 'its process is gone'
    Check (!$other.HasExited) 'Pulse running from another directory is left alone'
    Check ((Log-Of "$out\other.log") -notmatch 'session') 'the other copy got no messages'

    # --- Busy for a few seconds, then accepts --------------------------------------
    $busy = Start-Fake 'app' 'modern' "$out\busy.log" 3
    $r = Run-Harness 'busy' 10
    $log = Log-Of "$out\busy.log"
    Check ($r.State -eq 0 -and $r.Ms -ge 2000) "busy Pulse is waited for, then closes (state $($r.State), $($r.Ms) ms)"
    Check ($log -match 'busy' -and $log -match 'accepted' -and $log -match 'exit' -and $log -notmatch 'session') 'it closed itself through the handshake'
    Check ($busy.WaitForExit(3000)) 'its process is gone'

    # --- Unattended mode waits beyond the old ten-second Retry boundary ----------
    $autoupdate = Start-Fake 'app' 'modern' "$out\autoupdate.log" 12
    $r = Run-Harness 'autoupdate' 0 $true
    Check ($r.State -eq 0 -and $r.Ms -ge 10000) "unattended update automatically waits beyond Retry boundary (state $($r.State), $($r.Ms) ms)"
    Check ($autoupdate.WaitForExit(3000) -and (Log-Of "$out\autoupdate.log") -match 'accepted') 'unattended update closes through accepted handshake'

    $legacyUpdate = Start-Fake 'app' 'legacy' "$out\legacy-update.log"
    $r = Run-Harness 'legacy-update' 0 $true
    Check ($r.State -eq 1 -and !$legacyUpdate.HasExited -and (Log-Of "$out\legacy-update.log") -notmatch 'session') 'unattended update refuses to force-close an unsupported legacy process'
    $legacyUpdate.Kill(); $legacyUpdate.WaitForExit()

    $savefailed = Start-Fake 'app' 'savefailed' "$out\savefailed.log"
    $r = Run-Harness 'savefailed' 0 $true
    Check ($r.State -eq 1 -and !$savefailed.HasExited -and (Log-Of "$out\savefailed.log") -notmatch 'session') 'session-save failure aborts without killing Pulse'
    $savefailed.Kill(); $savefailed.WaitForExit()

    # --- Still busy when the wait runs out ---------------------------------------
    $stuck = Start-Fake 'app' 'modern' "$out\stuck.log" 600
    $r = Run-Harness 'stuck' 2
    Check ($r.State -eq 2) "work in progress is never interrupted (state $($r.State), $($r.Ms) ms)"
    Check (!$stuck.HasExited -and (Log-Of "$out\stuck.log") -notmatch 'session') 'the busy Pulse keeps running'
} finally {
    foreach ($p in @($legacy, $other, $busy, $stuck, $autoupdate, $legacyUpdate, $savefailed)) { if ($p -and !$p.HasExited) { $p.Kill(); $p.WaitForExit() } }
}

if ($failures) { throw "Close-running regression failed ($failures)." }
Write-Output 'Close-running regression: PASS'
