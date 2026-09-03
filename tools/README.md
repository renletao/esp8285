# ESP8285 配网码生成器

## Windows 使用

双击 `dist/ESP8285-WiFi-QR-Tool-windows-x64.exe` 即可。程序完全离线运行，
会在 Windows 默认浏览器中打开生成界面，不需要安装 Python、Go 或其他依赖。

浏览器下载的 PNG/SVG 文件默认保存在 Windows 的“下载”目录。

## 重新构建

需要 Go 1.22 或更高版本。在 `tools` 目录执行：

```powershell
$env:GOOS = "windows"
$env:GOARCH = "amd64"
go build -trimpath -ldflags "-s -w -H=windowsgui" -o dist/ESP8285-WiFi-QR-Tool-windows-x64.exe .
```

网页通过 `go:embed` 打包在 EXE 内，修改 `wifi_qr_tool.html` 后重新执行上述命令即可。
