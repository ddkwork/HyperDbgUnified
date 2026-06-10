#requires -RunAsAdministrator

$regPath = 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl'
$helperPath = Join-Path $PSScriptRoot 'fix_dump_path.bat'
$sep = '=' * 45

Write-Host '正在修改蓝屏转储文件路径到 D:\ ...' -ForegroundColor Yellow
Write-Host ''

# ==========================================
# 第一步：立即修改注册表
# ==========================================
Write-Host '[1/5] 设置完整转储路径（D:\MEMORY.DMP）...' -NoNewline
Set-ItemProperty -Path $regPath -Name 'DumpFile' -Value 'D:\MEMORY.DMP'
Write-Host '  OK' -ForegroundColor Green

Write-Host '[2/5] 设置小内存转储路径（D:\Minidump）...' -NoNewline
Set-ItemProperty -Path $regPath -Name 'MinidumpDir' -Value 'D:\Minidump'
Write-Host '  OK' -ForegroundColor Green

Write-Host '[3/5] 设置转储类型（自动内存转储）...' -NoNewline
Set-ItemProperty -Path $regPath -Name 'CrashDumpEnabled' -Value 7
Write-Host '  OK' -ForegroundColor Green

Write-Host '[4/5] 允许覆盖旧文件...' -NoNewline
Set-ItemProperty -Path $regPath -Name 'OverwriteExistingDumpFile' -Value 1
Write-Host '  OK' -ForegroundColor Green

# ==========================================
# 第二步：生成开机修复脚本
# ==========================================
Write-Host '[5/5] 创建开机自修复任务（登录后10秒自动修复）...'

@"
@reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v MinidumpDir /t REG_SZ /d D:\Minidump /f >nul
@reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v DumpFile /t REG_SZ /d D:\MEMORY.DMP /f >nul
@reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v CrashDumpEnabled /t REG_DWORD /d 7 /f >nul
"@ | Out-File -FilePath $helperPath -Encoding ASCII

# ==========================================
# 第三步：创建开机自修复计划任务
# ==========================================
$taskName = 'FixDumpPath'
$result = & schtasks /create /tn $taskName /tr "`"$helperPath`"" /sc onlogon /delay 0000:10 /ru SYSTEM /rl highest /f 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host '     计划任务创建成功' -ForegroundColor Green
} else {
    Write-Host '     计划任务创建失败:' -ForegroundColor Red
    Write-Host "     $result" -ForegroundColor Red
}

Write-Host ''
Write-Host $sep -ForegroundColor Cyan
Write-Host '  修改完成！' -ForegroundColor Green
Write-Host '  小内存转储路径:  D:\Minidump' -ForegroundColor Cyan
Write-Host '  完整/核心转储:    D:\MEMORY.DMP' -ForegroundColor Cyan
Write-Host '  转储类型:         自动内存转储' -ForegroundColor Cyan
Write-Host '  自修复任务:       登录后10秒自动修复' -ForegroundColor Cyan
Write-Host '  希沃还原:         未修改，保持正常使用' -ForegroundColor Cyan
Write-Host $sep -ForegroundColor Cyan
Write-Host ''

$reboot = Read-Host '必须重启计算机才能生效，是否立即重启？(Y/N)'
if ($reboot -eq 'Y' -or $reboot -eq 'y') {
    Restart-Computer -Force
}
