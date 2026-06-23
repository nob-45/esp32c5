@echo off
REM ============================================
REM 一键保存脚本：提交并推送到 GitHub
REM 用法：
REM   git_save.bat            使用默认提交说明
REM   git_save.bat "你的说明"  使用自定义提交说明
REM ============================================

cd /d "%~dp0.."

REM 取提交说明，没传参就用时间戳
set "MSG=%~1"
if "%MSG%"=="" (
    for /f "tokens=1-3 delims=/: " %%a in ("%date% %time%") do set "MSG=auto save %date% %time%"
)

echo [1/3] 添加改动...
git add -A

echo [2/3] 提交：%MSG%
git commit -m "%MSG%"
if errorlevel 1 (
    echo 没有需要提交的改动，或提交失败。
)

echo [3/3] 推送到 GitHub...
git push origin main

echo.
echo 完成。