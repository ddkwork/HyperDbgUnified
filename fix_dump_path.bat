@reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v MinidumpDir /t REG_SZ /d D:\Minidump /f >nul
@reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v DumpFile /t REG_SZ /d D:\MEMORY.DMP /f >nul
@reg add HKLM\SYSTEM\CurrentControlSet\Control\CrashControl /v CrashDumpEnabled /t REG_DWORD /d 7 /f >nul
