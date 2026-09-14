param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
# 脚本参数：只接受 Debug 或 Release，默认 Debug。
# ValidateSet 会在参数非法时直接报错，避免拼错配置名却继续往下跑。
. "$PSScriptRoot/environment.ps1"
# 点源引入环境脚本：它负责定位 CMake、CTest 与项目根目录。
Push-Location $projectRoot
# 切到项目根目录，保证后续相对路径都从这里解析。
try {
# 用 try/finally 保证无论成功失败都会恢复原来的工作目录。
    & $cmakeExe --preset windows
    # 用 windows 预设做一次 CMake 配置（生成构建系统）。
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    # 原生程序不抛异常，只能看退出码；非 0 就主动抛错停止脚本。
    & $cmakeExe --build --preset "windows-$($Configuration.ToLowerInvariant())" --parallel
    # 按配置对应的预设并行构建（预设名里用小写的 debug/release）。
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
    # 构建失败同样抛错，避免脚本假装成功。
} finally { Pop-Location }
# 无论成功失败都切回原来的目录。
