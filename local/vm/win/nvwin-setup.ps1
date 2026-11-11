# First logon of the Windows benchmark guest (autounattend.xml ->
# FirstLogonCommands, elevated), adapted from another project of the author.
# Runs from the unattend stick next to agent.ps1, authorized_keys and
# user.txt. Everything win.sh needs to drive the guest: OpenSSH with the key,
# the job runner in the desktop session (games and the frame-time capture
# need it), a session that never locks or sleeps, no Windows Update restarts.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force 'C:\nvwin\jobs' | Out-Null
Start-Transcript -Path 'C:\nvwin\setup.log' -Append | Out-Null

$user, $password = Get-Content "$src\user.txt"

# The job runner, started elevated in the desktop session at every logon.
Copy-Item "$src\agent.ps1" 'C:\nvwin\agent.ps1' -Force
Set-ItemProperty 'C:\nvwin\agent.ps1' IsReadOnly $false
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\nvwin\agent.ps1'
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries
Register-ScheduledTask -TaskName 'nvwin-agent' -Action $action -Trigger $trigger `
  -Principal $principal -Settings $settings -Force | Out-Null

# OpenSSH server (a Feature on Demand: needs QEMU's user network).
Add-WindowsCapability -Online -Name 'OpenSSH.Server~~~~0.0.1.0' | Out-Null
Set-Service sshd -StartupType Automatic
Start-Service sshd
New-ItemProperty -Path 'HKLM:\SOFTWARE\OpenSSH' -Name DefaultShell -Force -PropertyType String `
  -Value 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' | Out-Null
# QEMU's user network comes up as Public; port 22 is opened for Private only.
Get-NetConnectionProfile | Set-NetConnectionProfile -NetworkCategory Private
Set-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -Profile Any
$keys = 'C:\ProgramData\ssh\administrators_authorized_keys'
Copy-Item "$src\authorized_keys" $keys -Force
icacls $keys /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F' | Out-Null

# A desktop session that is always there, never locks, never sleeps.
$winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
Set-ItemProperty $winlogon AutoAdminLogon '1'
Set-ItemProperty $winlogon DefaultUserName $user
Set-ItemProperty $winlogon DefaultPassword $password
Remove-ItemProperty $winlogon AutoLogonCount -ErrorAction SilentlyContinue
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c   # High performance
powercfg /change standby-timeout-ac 0
powercfg /change monitor-timeout-ac 0
powercfg /change hibernate-timeout-ac 0
New-Item -Force 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' | Out-Null
Set-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization' NoLockScreen 1

# No updates or restarts in the middle of a benchmark.
New-Item -Force 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' | Out-Null
Set-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' NoAutoUpdate 1
Set-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU' NoAutoRebootWithLoggedOnUsers 1
# Windows Update must not swap the NVIDIA driver installed by hand.
New-Item -Force 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' | Out-Null
Set-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate' ExcludeWUDriversInQualityUpdate 1
# Game Bar / Game DVR record in the background: off, as on most benchmark rigs.
New-Item -Force 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\GameDVR' | Out-Null
Set-ItemProperty 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\GameDVR' AllowGameDVR 0

Set-Content 'C:\nvwin\setup.done' (Get-Date -Format o)
Stop-Transcript | Out-Null
shutdown /r /t 10 /c 'nvwin: setup done'
