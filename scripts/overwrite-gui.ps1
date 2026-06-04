# Launch OverWrite GUI (Windows)
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Gui = Join-Path $Root "gui\overwrite_gui.py"

if (-not (Test-Path $Gui)) {
    Write-Error "GUI not found: $Gui"
    exit 1
}

$Py = $null
foreach ($c in @("python", "python3", "py")) {
    if (Get-Command $c -ErrorAction SilentlyContinue) {
        $Py = $c
        break
    }
}
if (-not $Py) {
    Write-Error "Python 3 not found. Install from https://www.python.org/ (enable tk/tcl)."
    exit 1
}

if ($Py -eq "py") {
    & py -3 $Gui @args
} else {
    & $Py $Gui @args
}
