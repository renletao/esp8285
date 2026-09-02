// 把内嵌的配网码生成器网页写到临时目录并用默认浏览器打开。
package main

import (
	"embed"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

//go:embed wifi_qr_tool.html
var assets embed.FS

func main() {
	html, err := assets.ReadFile("wifi_qr_tool.html")
	if err != nil {
		fail(err)
	}
	dir := filepath.Join(os.TempDir(), "esp8285-qr")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fail(err)
	}
	page := filepath.Join(dir, "wifi_qr_tool.html")
	if err := os.WriteFile(page, html, 0o644); err != nil {
		fail(err)
	}
	if err := open(page); err != nil {
		fail(fmt.Errorf("打开浏览器失败: %w\n请手动打开这个文件: %s", err, page))
	}
}

func open(path string) error {
	switch runtime.GOOS {
	case "windows":
		if err := exec.Command("rundll32", "url.dll,FileProtocolHandler", path).Start(); err == nil {
			return nil
		}
		return exec.Command("cmd", "/c", "start", "", path).Start()
	case "darwin":
		return exec.Command("open", path).Start()
	default:
		return exec.Command("xdg-open", path).Start()
	}
}

func fail(err error) {
	msg := fmt.Sprintf("配网码生成器启动失败\n\n%v\n", err)
	fmt.Fprint(os.Stderr, msg)
	if note, e := os.UserHomeDir(); e == nil {
		os.WriteFile(filepath.Join(note, "配网码生成器-错误.txt"), []byte(msg), 0o644)
	}
	os.Exit(1)
}
