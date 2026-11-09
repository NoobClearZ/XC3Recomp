# Point the project at the player's own game files (modelled on LASRecomp's start-screen game-file setup).
# The player picks a folder; exefs (the folder with main and main.npdm) and romfs (the game's data root: XC3's has
# bf3.arh / bf3.ard and no bdat/ folder; bdat/ is XCDE's) are found in it or a few levels below (some layouts nest them). Game packages (.xci / .nsp / .nca / .nsz)
# are refused without being read. The version (games/<id>/profile.json: XC3 2.2.0) and the romfs contents are checked by tools/extract/check_dump.py,
# then targets\exefs and targets\romfs are linked to the folders found (junctions: no 14 GB copy).
# [XC3] The add-on content (DLC, optional): a folder with bf3_dlc01..04.arh / .ard (and movie\, sound\), linked as
# targets\dlcs. Found next to exefs / romfs when the game's folder is chosen, or chosen on its own with -Dlc.
#   powershell -ExecutionPolicy Bypass -File tools\setup_game_files.ps1 [folder]      exit 0 = ready
#   powershell -ExecutionPolicy Bypass -File tools\setup_game_files.ps1 -Dlc [folder] exit 0 = DLC linked
param([string]$Folder, [switch]$Dlc)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$PickHint = "Choose the folder that has the game's exefs and romfs folders in it."
$DlcHint = 'Choose the folder with the DLC files (bf3_dlc01.arh, bf3_dlc01.ard, ...).'
$py = "$root\.venv\Scripts\python.exe"

function Find-Dlc([string]$dir, [int]$depth) {
    if ($depth -gt 4) { return $null }
    if (Get-ChildItem -LiteralPath $dir -Filter 'bf3_dlc*.arh' -File -ErrorAction SilentlyContinue | Select-Object -First 1) { return $dir }
    foreach ($d in Get-ChildItem -LiteralPath $dir -Directory -ErrorAction SilentlyContinue) {
        $r = Find-Dlc $d.FullName ($depth + 1)
        if ($r) { return $r }
    }
    return $null
}

# targets\<name> -> $target (a junction; an existing real folder is never deleted). $false if it could not be linked.
function Set-Link([string]$name, [string]$target) {
    New-Item -ItemType Directory -Force "$root\targets" | Out-Null
    $t = "$root\targets\$name"
    if (Test-Path -LiteralPath $t) {
        $item = Get-Item -LiteralPath $t -Force
        if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            Write-Host "targets\$name is a real folder (game files set up earlier). Move or rename it, then run this again."
            return $false
        }
        $item.Delete()  # (removes the link only)
    }
    New-Item -ItemType Junction -Path $t -Target $target | Out-Null
    return $true
}

if ($Dlc) {
    if (-not $Folder) {
        Add-Type -AssemblyName System.Windows.Forms
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = $DlcHint
        if ($dlg.ShowDialog() -ne 'OK') { Write-Host 'Cancelled.'; exit 2 }
        $Folder = $dlg.SelectedPath
    }
    if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { Write-Host "No such folder: $Folder"; exit 1 }
    $d = Find-Dlc (Resolve-Path -LiteralPath $Folder).Path 0
    if (-not $d) { Write-Host 'No DLC files found in that folder.'; Write-Host $DlcHint; exit 1 }
    Write-Host "dlcs: $d"
    & $py -X utf8 "$root\tools\extract\check_dump.py" --dlcs $d
    if ($LASTEXITCODE -ne 0) { exit 1 }
    if (-not (Set-Link 'dlcs' $d)) { exit 1 }
    Write-Host 'DLC ready: targets\dlcs points to it.'
    exit 0
}

function Find-Game([string]$dir, [int]$depth, [hashtable]$found) {
    if ($depth -gt 4 -or ($found.exefs -and $found.romfs)) { return }
    if (-not $found.exefs -and (Test-Path "$dir\main" -PathType Leaf) -and (Test-Path "$dir\main.npdm" -PathType Leaf)) { $found.exefs = $dir }
    if (-not $found.romfs -and ((Test-Path "$dir\bf3.arh" -PathType Leaf) -or (Test-Path "$dir\bdat" -PathType Container))) { $found.romfs = $dir }
    foreach ($f in Get-ChildItem -LiteralPath $dir -File -ErrorAction SilentlyContinue) {
        if ($f.Extension -match '^\.(xci|nsp|nca|nsz|xcz)$') { $found.packages = $true }
    }
    foreach ($d in Get-ChildItem -LiteralPath $dir -Directory -ErrorAction SilentlyContinue) {
        Find-Game $d.FullName ($depth + 1) $found
    }
}

if (-not $Folder) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "$PickHint (Xenoblade Chronicles 3, version 2.2.0)"
    if ($dlg.ShowDialog() -ne 'OK') { Write-Host 'Cancelled.'; exit 2 }
    $Folder = $dlg.SelectedPath
}
if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { Write-Host "No such folder: $Folder"; exit 1 }

$found = @{ exefs = $null; romfs = $null; packages = $false }
Find-Game (Resolve-Path -LiteralPath $Folder).Path 0 $found
if (-not $found.exefs -or -not $found.romfs) {
    if ($found.packages) {
        Write-Host 'Those are game packages, not game files.'
        Write-Host 'XC3Recomp needs the game''s own files (the exefs and romfs folders), not .xci / .nsp / .nca files.'
    } else {
        Write-Host 'No game files found in that folder.'
        Write-Host $PickHint
    }
    exit 1
}
Write-Host "exefs: $($found.exefs)"
Write-Host "romfs: $($found.romfs)"

# version (build-ids) and romfs contents
& $py -X utf8 "$root\tools\extract\check_dump.py" --exefs $found.exefs --romfs $found.romfs
if ($LASTEXITCODE -ne 0) {
    if ($LASTEXITCODE -eq 4) { Write-Host 'XC3Recomp needs version 2.2.0: the base game with the 2.2.0 update applied.' }
    elseif ($LASTEXITCODE -eq 5) { Write-Host 'Some romfs files are missing or different, usually from a copy or extraction that stopped partway, or a romfs without the 2.2.0 update. Get the game files again, then choose the folder again.' }
    exit 1
}

# targets\exefs, targets\romfs -> the folders found
foreach ($k in 'exefs', 'romfs') { if (-not (Set-Link $k $found[$k])) { exit 1 } }
Write-Host 'Game files ready: targets\ points to them.'
# [XC3] DLC in the same folder: linked too (optional; an unusable one is reported, the game files stay ready)
$d = Find-Dlc (Resolve-Path -LiteralPath $Folder).Path 0
if ($d) {
    & $py -X utf8 "$root\tools\extract\check_dump.py" --dlcs $d
    if ($LASTEXITCODE -eq 0 -and (Set-Link 'dlcs' $d)) { Write-Host "DLC ready: targets\dlcs -> $d" }
}
exit 0
