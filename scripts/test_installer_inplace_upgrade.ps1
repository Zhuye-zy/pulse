param(
    [string]$Iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
# Regression for #72: an upgrade into the same directory must not run the
# previous uninstaller (it deletes pulse.exe and unpins the Start/taskbar
# shortcuts). Nothing here installs, uninstalls or touches the registry: the
# directory test runs in a harness Setup that returns False from
# InitializeSetup, and the control flow is checked on the script text.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$source = Get-Content -LiteralPath (Join-Path $repo 'installer/PulseSetup.iss') -Raw
$out = Join-Path $repo 'build/installer-inplace-test'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$failures = 0
function Check([bool]$ok, [string]$label) {
    if ($ok) { Write-Output "[PASS] $label" } else { Write-Output "[FAIL] $label"; $script:failures++ }
}

# --- Control flow on the script text ----------------------------------------
$prepare = [regex]::Match($source, '(?s)function PrepareToInstall\(.*?\r?\nend;').Value
$step = [regex]::Match($source, '(?s)procedure CurStepChanged\(.*?\r?\nend;').Value
if (!$prepare -or !$step) { throw 'Installer procedure boundaries changed; update the harness.' }
$inPlace = $prepare.IndexOf('if InPlaceUpgrade then')
$uninstall = $prepare.IndexOf('UninstallPreviousVersion')
Check ($prepare -match 'InPlaceUpgrade := UpgradesInPlace;') 'PrepareToInstall decides in-place upgrades'
Check ($inPlace -ge 0 -and $uninstall -gt $inPlace) 'in-place branch returns before the previous uninstaller runs'
Check ($prepare.IndexOf('ClosePulseForUpdate') -lt $inPlace -and $prepare.IndexOf('StopPulseHosts') -lt $inPlace) 'Pulse and its hosts are stopped before files are overwritten'
Check ($step -match '(?s)if InPlaceUpgrade and PulseIndexServiceExists then.*--uninstall') 'in-place upgrade without the index task removes the old service'
Check ($source -match "(?s)function UpgradesInPlace:.*SamePulseDirectory\(Previous, ExpandConstant\('\{app\}'\)\).*pulse\.exe") 'in-place requires the registered directory and its executable'

# --- SamePulseDirectory in a harness Setup ----------------------------------
$fn = [regex]::Match($source, '(?s)function SamePulseDirectory\(.*?\r?\nend;').Value
if (!$fn) { throw 'SamePulseDirectory not found; update the harness.' }
if ($fn -match '\b(Reg\w+|Exec|ShellExec|DeleteFile|DelTree|ExpandConstant|FileExists)\s*\(') {
    throw 'Unexpected external operation in SamePulseDirectory.'
}
$report = (Join-Path $out 'result.txt').Replace("'", "''")
$prefix = @'
[Setup]
AppName=Pulse in-place upgrade regression harness
AppVersion=1
DefaultDirName={tmp}\PulseInPlaceHarnessNeverInstalled
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputBaseFilename=installer-inplace-harness
Compression=none
[Code]
var
  Report: String;
procedure Check(Condition: Boolean; const LabelText: String);
begin
  if Condition then Report := Report + '[PASS] ' + LabelText + #13#10
  else Report := Report + '[FAIL] ' + LabelText + #13#10;
end;
'@
$tests = @'
function InitializeSetup: Boolean;
begin
  Check(SamePulseDirectory('C:\Program Files\Pulse\', 'c:\program files\pulse'), 'same directory ignores case and trailing backslash');
  Check(SamePulseDirectory(' C:\Program Files\Pulse ', 'C:\Program Files\Pulse'), 'same directory ignores surrounding blanks');
  Check(SamePulseDirectory('D:\', 'D:\'), 'drive root compares equal');
  Check(not SamePulseDirectory('C:\Program Files\Pulse', 'D:\Apps\Pulse'), 'moved install is not in place');
  Check(not SamePulseDirectory('C:\Pulse', 'C:\Pulse2'), 'directory prefix is not the same directory');
  Check(not SamePulseDirectory('', 'C:\Program Files\Pulse'), 'missing registration is not in place');
  Check(not SamePulseDirectory('C:\Program Files\Pulse', ''), 'empty target is not in place');
  { Returning False prevents installation, file deployment, and uninstall registration. }
  Result := False;
'@
$harness = $prefix + "`r`n" + $fn + "`r`n" + $tests + "`r`n  SaveStringToFile('$report', Report, False);`r`nend;`r`n"
$iss = Join-Path $out 'harness.iss'
Set-Content -LiteralPath $iss -Value $harness -Encoding utf8
& $Iscc "/Q" "/O$out" $iss
if ($LASTEXITCODE -ne 0) { throw 'Pascal harness compilation failed.' }
$exe = Join-Path $out 'installer-inplace-harness.exe'
if (Test-Path -LiteralPath (Join-Path $out 'result.txt')) { Remove-Item -LiteralPath (Join-Path $out 'result.txt') }
$process = Start-Process -FilePath $exe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -WindowStyle Hidden -Wait -PassThru
if (!(Test-Path -LiteralPath (Join-Path $out 'result.txt'))) { throw "Harness produced no report (exit $($process.ExitCode))." }
$result = (Get-Content -LiteralPath (Join-Path $out 'result.txt') -Raw).TrimEnd()
Write-Output $result
if ($result -match '\[FAIL\]') { $failures++ }
if (($result -split '\[PASS\]').Count -ne 8) { throw 'Unexpected number of completed assertions.' }
if ($failures) { throw "In-place upgrade regression failed ($failures)." }
Write-Output 'In-place upgrade regression: PASS'
