package main

import (
	"fmt"
	"syscall"
	"unsafe"
)

var (
	user32           = syscall.NewLazyDLL("user32.dll")
	gdi32            = syscall.NewLazyDLL("gdi32.dll")
	getDesktopWindow = user32.NewProc("GetDesktopWindow")
	getDC            = user32.NewProc("GetDC")
	releaseDC        = user32.NewProc("ReleaseDC")
	getSystemMetrics = user32.NewProc("GetSystemMetrics")
)

func main() {
	w, _, _ := getSystemMetrics.Call(0) // SM_CXSCREEN
	h, _, _ := getSystemMetrics.Call(1) // SM_CYSCREEN
	fmt.Printf("Screen dimensions: %dx%d\n", w, h)
}
