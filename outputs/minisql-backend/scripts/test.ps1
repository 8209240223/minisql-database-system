param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
# 脚本参数：与 build.ps1 保持一致，只接受 Debug 或 Release。
& "$PSScriptRoot/build.ps1" -Configuration $Configuration
# 先跑构建脚本，保证测试跑的是最新产物，而不是上一次的旧二进制。
. "$PSScriptRoot/environment.ps1"
# 点源引入环境脚本，拿到 ctestExe 与项目根目录。
Push-Location $projectRoot
# 切到项目根目录，让 CTest 预设里的相对路径能正确解析。
try {
# 用 try/finally 保证退出时恢复原目录。
    & $ctestExe --preset "windows-$($Configuration.ToLowerInvariant())"
    # 按配置对应的预设运行全部测试。
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    # 测试有失败就抛错，让 CI/调用方拿到非零退出码。
} finally { Pop-Location }
# 恢复原目录。
