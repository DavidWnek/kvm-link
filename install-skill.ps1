# Installs the kvm-link skill for Claude Code: copies skill\kvm-link, kvm.py and grab.ps1 to
# %USERPROFILE%\.claude\skills\kvm-link, and notes where this project is so Claude can find
# the firmware and wiring notes. Run it again after changing any of them.
#
#   .\install-skill.ps1
#   .\install-skill.ps1 -Target D:\somewhere\skills\kvm-link

param([string]$Target = (Join-Path $env:USERPROFILE '.claude\skills\kvm-link'))

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

New-Item -ItemType Directory -Force (Join-Path $Target 'scripts') | Out-Null
Copy-Item (Join-Path $root 'skill\kvm-link\SKILL.md') $Target -Force
Copy-Item (Join-Path $root 'kvm.py'), (Join-Path $root 'grab.ps1') (Join-Path $Target 'scripts') -Force
Set-Content (Join-Path $Target 'project-path.txt') $root -Encoding ascii

"Installed the kvm-link skill in $Target"
Get-ChildItem $Target -Recurse -File | ForEach-Object { '  ' + $_.FullName.Substring($Target.Length + 1) }
