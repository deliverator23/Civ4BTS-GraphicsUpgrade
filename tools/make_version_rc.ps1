# Writes build\version.rc from src\version.rc.in: the file version is the system's 32-bit d3d9.dll's, the product
# version is GRAPHICSUPGRADE_VERSION from src\common.h.
$v = (Get-Item "$env:SystemRoot\SysWOW64\d3d9.dll").VersionInfo
$num = '{0},{1},{2},{3}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart
$str = '{0}.{1}.{2}.{3}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart
$m = [regex]::Match((Get-Content 'src\common.h' -Raw), '#define GRAPHICSUPGRADE_VERSION "([^"]+)"')
if (-not $m.Success) { Write-Error 'GRAPHICSUPGRADE_VERSION not found in src\common.h'; exit 1 }
(Get-Content 'src\version.rc.in') `
    -replace '@VERSION_NUM@', $num `
    -replace '@VERSION_STR@', $str `
    -replace '@GRAPHICSUPGRADE_VERSION@', $m.Groups[1].Value |
    Set-Content 'build\version.rc'
