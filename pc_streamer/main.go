package main

import (
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"math"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// Win32 API definitions
var (
	user32               = syscall.NewLazyDLL("user32.dll")
	gdi32                = syscall.NewLazyDLL("gdi32.dll")
	kernel32             = syscall.NewLazyDLL("kernel32.dll")

	procGetDesktopWindow = user32.NewProc("GetDesktopWindow")
	procGetDC            = user32.NewProc("GetDC")
	procReleaseDC        = user32.NewProc("ReleaseDC")
	procGetSystemMetrics = user32.NewProc("GetSystemMetrics")
	procGetCursorPos     = user32.NewProc("GetCursorPos")
	procKeybdEvent       = user32.NewProc("keybd_event")
	procMouseEvent       = user32.NewProc("mouse_event")

	procCreateCompatibleDC     = gdi32.NewProc("CreateCompatibleDC")
	procCreateCompatibleBitmap = gdi32.NewProc("CreateCompatibleBitmap")
	procSelectObject           = gdi32.NewProc("SelectObject")
	procBitBlt                 = gdi32.NewProc("BitBlt")
	procDeleteObject           = gdi32.NewProc("DeleteObject")
	procDeleteDC               = gdi32.NewProc("DeleteDC")
	procGetDIBits              = gdi32.NewProc("GetDIBits")

	procCreateFileW   = kernel32.NewProc("CreateFileW")
	procCloseHandle   = kernel32.NewProc("CloseHandle")
	procSetCommState  = kernel32.NewProc("SetCommState")
	procSetCommTimeouts = kernel32.NewProc("SetCommTimeouts")
	procReadFile      = kernel32.NewProc("ReadFile")
	procWriteFile     = kernel32.NewProc("WriteFile")
)

const (
	SRCCOPY          = 0x00CC0020
	DIB_RGB_COLORS   = 0
	GENERIC_READ     = 0x80000000
	GENERIC_WRITE    = 0x40000000
	OPEN_EXISTING    = 3
	FILE_ATTRIBUTE_NORMAL = 0x80

	KEYEVENTF_KEYUP     = 0x0002
	KEYEVENTF_SCANCODE  = 0x0008
	MOUSEEVENTF_MOVE    = 0x0001

	VK_SPACE = 0x20
	VK_W     = 0x57
	VK_S     = 0x53
	VK_A     = 0x41
	VK_D     = 0x44
)

type BITMAPINFOHEADER struct {
	BiSize          uint32
	BiWidth         int32
	BiHeight        int32
	BiPlanes        uint16
	BiBitCount      uint16
	BiCompression   uint32
	BiSizeImage     uint32
	BiXPelsPerMeter int32
	BiYPelsPerMeter int32
	BiClrUsed       uint32
	BiClrImportant  uint32
}

type BITMAPINFO struct {
	BmiHeader BITMAPINFOHEADER
	BmiColors [1]uint32
}

type DCB struct {
	DCBlength uint32
	BaudRate  uint32
	Flags     uint32
	WReserved uint16
	XonLim    uint16
	XoffLim   uint16
	ByteSize  byte
	Parity    byte
	StopBits  byte
	XonChar   byte
	XoffChar  byte
	ErrorChar byte
	EofChar   byte
	EvtChar   byte
	WReserved1 uint16
}

type COMMTIMEOUTS struct {
	ReadIntervalTimeout         uint32
	ReadTotalTimeoutMultiplier  uint32
	ReadTotalTimeoutConstant    uint32
	WriteTotalTimeoutMultiplier uint32
	WriteTotalTimeoutConstant   uint32
}

type POINT struct {
	X int32
	Y int32
}

// Streamer Configuration
type Config struct {
	sync.RWMutex
	ComPort         string  `json:"com_port"`
	BaudRate        int     `json:"baud_rate"`
	TargetFPS       int     `json:"target_fps"`
	CaptureMode     string  `json:"capture_mode"`     // "fullscreen", "cursor", "center"
	ScaleMode       string  `json:"scale_mode"`       // "fit", "stretch", "crop"
	DitherAlgo      string  `json:"dither_algo"`      // "floyd", "atkinson", "bayer4", "bayer8", "threshold"
	Brightness      float64 `json:"brightness"`       // -100 to 100
	Contrast        float64 `json:"contrast"`         // 0.2 to 3.0
	Gamma           float64 `json:"gamma"`            // 0.2 to 2.5
	InvertColors    bool    `json:"invert_colors"`
	ThresholdCutoff int     `json:"threshold_cutoff"` // 0 to 255
	MouseLookSpeed  int     `json:"mouse_look_speed"` // 5 to 100 px per tick
	GamepadEnabled  bool    `json:"gamepad_enabled"`
}

var cfg = Config{
	ComPort:         "COM8",
	BaudRate:        115200,
	TargetFPS:       25,
	CaptureMode:     "fullscreen",
	ScaleMode:       "fit",
	DitherAlgo:      "atkinson",
	Brightness:      10,
	Contrast:        1.3,
	Gamma:           1.1,
	InvertColors:    false,
	ThresholdCutoff: 128,
	MouseLookSpeed:  35,
	GamepadEnabled:  true,
}

var (
	serialHandle uintptr = 0
	serialMutex  sync.Mutex
	isStreaming  = false
	lastFrame    [1024]byte
	frameMutex   sync.RWMutex
	buttonStates = make(map[string]bool)
	btnMutex     sync.RWMutex
)

// Bayer matrices
var bayer4x4 = [4][4]float64{
	{0.0 / 16.0, 8.0 / 16.0, 2.0 / 16.0, 10.0 / 16.0},
	{12.0 / 16.0, 4.0 / 16.0, 14.0 / 16.0, 6.0 / 16.0},
	{3.0 / 16.0, 11.0 / 16.0, 1.0 / 16.0, 9.0 / 16.0},
	{15.0 / 16.0, 7.0 / 16.0, 13.0 / 16.0, 5.0 / 16.0},
}

var bayer8x8 = [8][8]float64{
	{0.0 / 64.0, 32.0 / 64.0, 8.0 / 64.0, 40.0 / 64.0, 2.0 / 64.0, 34.0 / 64.0, 10.0 / 64.0, 42.0 / 64.0},
	{48.0 / 64.0, 16.0 / 64.0, 56.0 / 64.0, 24.0 / 64.0, 50.0 / 64.0, 18.0 / 64.0, 58.0 / 64.0, 26.0 / 64.0},
	{12.0 / 64.0, 44.0 / 64.0, 4.0 / 64.0, 36.0 / 64.0, 14.0 / 64.0, 46.0 / 64.0, 6.0 / 64.0, 38.0 / 64.0},
	{60.0 / 64.0, 28.0 / 64.0, 52.0 / 64.0, 20.0 / 64.0, 62.0 / 64.0, 30.0 / 64.0, 54.0 / 64.0, 22.0 / 64.0},
	{3.0 / 64.0, 35.0 / 64.0, 11.0 / 64.0, 43.0 / 64.0, 1.0 / 64.0, 33.0 / 64.0, 9.0 / 64.0, 41.0 / 64.0},
	{51.0 / 64.0, 19.0 / 64.0, 59.0 / 64.0, 27.0 / 64.0, 49.0 / 64.0, 17.0 / 64.0, 57.0 / 64.0, 25.0 / 64.0},
	{15.0 / 64.0, 47.0 / 64.0, 7.0 / 64.0, 39.0 / 64.0, 13.0 / 64.0, 45.0 / 64.0, 5.0 / 64.0, 37.0 / 64.0},
	{63.0 / 64.0, 31.0 / 64.0, 55.0 / 64.0, 23.0 / 64.0, 61.0 / 64.0, 29.0 / 64.0, 53.0 / 64.0, 21.0 / 64.0},
}

// Open COM port using Win32 API
func openSerialPort(portName string, baudRate int) (uintptr, error) {
	if !strings.HasPrefix(portName, `\\.\`) {
		portName = `\\.\` + portName
	}
	pName, err := syscall.UTF16PtrFromString(portName)
	if err != nil {
		return 0, err
	}

	h, _, errCall := procCreateFileW.Call(
		uintptr(unsafe.Pointer(pName)),
		GENERIC_READ|GENERIC_WRITE,
		0,
		0,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		0,
	)
	if h == uintptr(syscall.InvalidHandle) {
		return 0, fmt.Errorf("failed to open port %s: %v", portName, errCall)
	}

	var dcb DCB
	dcb.DCBlength = uint32(unsafe.Sizeof(dcb))
	dcb.BaudRate = uint32(baudRate)
	dcb.ByteSize = 8
	dcb.Parity = 0   // NOPARITY
	dcb.StopBits = 0 // ONESTOPBIT
	dcb.Flags = 1    // fBinary

	r, _, errCall := procSetCommState.Call(h, uintptr(unsafe.Pointer(&dcb)))
	if r == 0 {
		procCloseHandle.Call(h)
		return 0, fmt.Errorf("SetCommState failed: %v", errCall)
	}

	var timeouts COMMTIMEOUTS
	timeouts.ReadIntervalTimeout = 50
	timeouts.ReadTotalTimeoutConstant = 50
	timeouts.ReadTotalTimeoutMultiplier = 10
	timeouts.WriteTotalTimeoutConstant = 50
	timeouts.WriteTotalTimeoutMultiplier = 10

	procSetCommTimeouts.Call(h, uintptr(unsafe.Pointer(&timeouts)))
	return h, nil
}

func closeSerialPort() {
	serialMutex.Lock()
	defer serialMutex.Unlock()
	if serialHandle != 0 && serialHandle != uintptr(syscall.InvalidHandle) {
		procCloseHandle.Call(serialHandle)
		serialHandle = 0
	}
}

func writeSerial(data []byte) error {
	serialMutex.Lock()
	defer serialMutex.Unlock()
	if serialHandle == 0 || serialHandle == uintptr(syscall.InvalidHandle) {
		return fmt.Errorf("serial not connected")
	}

	var bytesWritten uint32
	r, _, errCall := procWriteFile.Call(
		serialHandle,
		uintptr(unsafe.Pointer(&data[0])),
		uintptr(len(data)),
		uintptr(unsafe.Pointer(&bytesWritten)),
		0,
	)
	if r == 0 {
		return fmt.Errorf("write error: %v", errCall)
	}
	return nil
}

// Screen capture using GDI
func captureScreen(mode, scaleMode string) (*image.RGBA, error) {
	screenW, _, _ := procGetSystemMetrics.Call(0) // SM_CXSCREEN
	screenH, _, _ := procGetSystemMetrics.Call(1) // SM_CYSCREEN

	if screenW == 0 || screenH == 0 {
		return nil, fmt.Errorf("failed to get screen metrics")
	}

	var srcX, srcY, srcW, srcH int32
	if mode == "cursor" {
		var pt POINT
		procGetCursorPos.Call(uintptr(unsafe.Pointer(&pt)))
		srcW = 320
		srcH = 160
		srcX = pt.X - srcW/2
		srcY = pt.Y - srcH/2
		if srcX < 0 {
			srcX = 0
		}
		if srcY < 0 {
			srcY = 0
		}
		if srcX+srcW > int32(screenW) {
			srcX = int32(screenW) - srcW
		}
		if srcY+srcH > int32(screenH) {
			srcY = int32(screenH) - srcH
		}
	} else if mode == "center" {
		srcW = int32(screenW) / 2
		srcH = int32(screenH) / 2
		srcX = int32(screenW)/4
		srcY = int32(screenH)/4
	} else {
		srcX = 0
		srcY = 0
		srcW = int32(screenW)
		srcH = int32(screenH)
	}

	hDesktop, _, _ := procGetDesktopWindow.Call()
	hDC, _, _ := procGetDC.Call(hDesktop)
	defer procReleaseDC.Call(hDesktop, hDC)

	hMemDC, _, _ := procCreateCompatibleDC.Call(hDC)
	defer procDeleteDC.Call(hMemDC)

	const targetW = 128
	const targetH = 64

	hBitmap, _, _ := procCreateCompatibleBitmap.Call(hDC, uintptr(srcW), uintptr(srcH))
	defer procDeleteObject.Call(hBitmap)

	procSelectObject.Call(hMemDC, hBitmap)
	procBitBlt.Call(hMemDC, 0, 0, uintptr(srcW), uintptr(srcH), hDC, uintptr(srcX), uintptr(srcY), SRCCOPY)

	var bi BITMAPINFO
	bi.BmiHeader.BiSize = uint32(unsafe.Sizeof(bi.BmiHeader))
	bi.BmiHeader.BiWidth = srcW
	bi.BmiHeader.BiHeight = -srcH // Top-down
	bi.BmiHeader.BiPlanes = 1
	bi.BmiHeader.BiBitCount = 32
	bi.BmiHeader.BiCompression = 0 // BI_RGB

	rawBuf := make([]byte, srcW*srcH*4)
	procGetDIBits.Call(
		hMemDC,
		hBitmap,
		0,
		uintptr(srcH),
		uintptr(unsafe.Pointer(&rawBuf[0])),
		uintptr(unsafe.Pointer(&bi)),
		DIB_RGB_COLORS,
	)

	// Downsample to 128x64 with chosen scaling mode
	dst := image.NewRGBA(image.Rect(0, 0, targetW, targetH))

	if scaleMode == "stretch" {
		for y := 0; y < targetH; y++ {
			sy := (y * int(srcH)) / targetH
			for x := 0; x < targetW; x++ {
				sx := (x * int(srcW)) / targetW
				idx := (sy*int(srcW) + sx) * 4
				b := rawBuf[idx]
				g := rawBuf[idx+1]
				r := rawBuf[idx+2]
				dst.SetRGBA(x, y, color.RGBA{R: r, G: g, B: b, A: 255})
			}
		}
	} else if scaleMode == "crop" {
		// Crop center 2:1 ratio
		var cropW, cropH int
		if int(srcW) >= int(srcH)*2 {
			cropH = int(srcH)
			cropW = cropH * 2
		} else {
			cropW = int(srcW)
			cropH = cropW / 2
		}
		startX := (int(srcW) - cropW) / 2
		startY := (int(srcH) - cropH) / 2

		for y := 0; y < targetH; y++ {
			sy := startY + (y*cropH)/targetH
			for x := 0; x < targetW; x++ {
				sx := startX + (x*cropW)/targetW
				idx := (sy*int(srcW) + sx) * 4
				b := rawBuf[idx]
				g := rawBuf[idx+1]
				r := rawBuf[idx+2]
				dst.SetRGBA(x, y, color.RGBA{R: r, G: g, B: b, A: 255})
			}
		}
	} else { // "fit" (maintain aspect ratio with letterboxing)
		screenAspect := float64(srcW) / float64(srcH)
		targetAspect := float64(targetW) / float64(targetH) // 2.0

		var renderW, renderH, offsetX, offsetY int
		if screenAspect >= targetAspect {
			renderW = targetW
			renderH = int(float64(targetW) / screenAspect)
			offsetX = 0
			offsetY = (targetH - renderH) / 2
		} else {
			renderH = targetH
			renderW = int(float64(targetH) * screenAspect)
			offsetX = (targetW - renderW) / 2
			offsetY = 0
		}

		for y := 0; y < targetH; y++ {
			for x := 0; x < targetW; x++ {
				if x >= offsetX && x < offsetX+renderW && y >= offsetY && y < offsetY+renderH {
					sx := ((x - offsetX) * int(srcW)) / renderW
					sy := ((y - offsetY) * int(srcH)) / renderH
					idx := (sy*int(srcW) + sx) * 4
					b := rawBuf[idx]
					g := rawBuf[idx+1]
					r := rawBuf[idx+2]
					dst.SetRGBA(x, y, color.RGBA{R: r, G: g, B: b, A: 255})
				} else {
					dst.SetRGBA(x, y, color.RGBA{R: 0, G: 0, B: 0, A: 255})
				}
			}
		}
	}

	return dst, nil
}

// Convert RGB 128x64 image into 1-bit SSD1306 Page-format bitmap (1024 bytes)
func processImageTo1Bit(img *image.RGBA, c *Config) [1024]byte {
	c.RLock()
	bright := c.Brightness
	contrast := c.Contrast
	gamma := c.Gamma
	invert := c.InvertColors
	algo := c.DitherAlgo
	cutoff := float64(c.ThresholdCutoff)
	c.RUnlock()

	// 1. Grayscale & Image Adjustments
	var gray [64][128]float64
	for y := 0; y < 64; y++ {
		for x := 0; x < 128; x++ {
			col := img.RGBAAt(x, y)
			// Standard perceptual luminance
			lum := 0.299*float64(col.R) + 0.587*float64(col.G) + 0.114*float64(col.B)

			// Brightness
			lum += bright

			// Contrast
			lum = (lum-128.0)*contrast + 128.0

			// Normalize 0..1 for gamma
			norm := lum / 255.0
			if norm < 0 {
				norm = 0
			}
			if norm > 1 {
				norm = 1
			}

			// Gamma
			if gamma > 0 && gamma != 1.0 {
				norm = math.Pow(norm, 1.0/gamma)
			}

			val := norm * 255.0
			if invert {
				val = 255.0 - val
			}
			gray[y][x] = val
		}
	}

	// 2. Dithering
	var binary [64][128]byte

	switch algo {
	case "atkinson":
		// Atkinson Dither (Classic Mac)
		// Fraction: 1/8 to (x+1, y), (x+2, y), (x-1, y+1), (x, y+1), (x+1, y+1), (x, y+2)
		for y := 0; y < 64; y++ {
			for x := 0; x < 128; x++ {
				oldPixel := gray[y][x]
				var newPixel float64
				if oldPixel > cutoff {
					newPixel = 255.0
					binary[y][x] = 1
				} else {
					newPixel = 0.0
					binary[y][x] = 0
				}
				errVal := (oldPixel - newPixel) / 8.0
				if x+1 < 128 {
					gray[y][x+1] += errVal
				}
				if x+2 < 128 {
					gray[y][x+2] += errVal
				}
				if y+1 < 64 {
					if x-1 >= 0 {
						gray[y+1][x-1] += errVal
					}
					gray[y+1][x] += errVal
					if x+1 < 128 {
						gray[y+1][x+1] += errVal
					}
				}
				if y+2 < 64 {
					gray[y+2][x] += errVal
				}
			}
		}

	case "floyd":
		// Floyd-Steinberg error diffusion
		for y := 0; y < 64; y++ {
			for x := 0; x < 128; x++ {
				oldPixel := gray[y][x]
				var newPixel float64
				if oldPixel > cutoff {
					newPixel = 255.0
					binary[y][x] = 1
				} else {
					newPixel = 0.0
					binary[y][x] = 0
				}
				errVal := oldPixel - newPixel
				if x+1 < 128 {
					gray[y][x+1] += errVal * (7.0 / 16.0)
				}
				if y+1 < 64 {
					if x-1 >= 0 {
						gray[y+1][x-1] += errVal * (3.0 / 16.0)
					}
					gray[y+1][x] += errVal * (5.0 / 16.0)
					if x+1 < 128 {
						gray[y+1][x+1] += errVal * (1.0 / 16.0)
					}
				}
			}
		}

	case "bayer4":
		for y := 0; y < 64; y++ {
			for x := 0; x < 128; x++ {
				thresh := bayer4x4[y%4][x%4] * 255.0
				if gray[y][x] > thresh {
					binary[y][x] = 1
				} else {
					binary[y][x] = 0
				}
			}
		}

	case "bayer8":
		for y := 0; y < 64; y++ {
			for x := 0; x < 128; x++ {
				thresh := bayer8x8[y%8][x%8] * 255.0
				if gray[y][x] > thresh {
					binary[y][x] = 1
				} else {
					binary[y][x] = 0
				}
			}
		}

	default: // "threshold"
		for y := 0; y < 64; y++ {
			for x := 0; x < 128; x++ {
				if gray[y][x] > cutoff {
					binary[y][x] = 1
				} else {
					binary[y][x] = 0
				}
			}
		}
	}

	// 3. Pack into SSD1306 8-page format (1024 bytes)
	// byte_index = (y / 8) * 128 + x
	// bit = y % 8
	var packet [1024]byte
	for y := 0; y < 64; y++ {
		page := y / 8
		bit := byte(y % 8)
		for x := 0; x < 128; x++ {
			if binary[y][x] == 1 {
				packet[page*128+x] |= (1 << bit)
			}
		}
	}

	return packet
}

// Emulate Keyboard and Mouse inputs for gaming
func handleButtonEvent(btn string, state string) {
	btnMutex.Lock()
	buttonStates[btn] = (state == "1")
	btnMutex.Unlock()

	cfg.RLock()
	enabled := cfg.GamepadEnabled
	speed := cfg.MouseLookSpeed
	cfg.RUnlock()

	if !enabled {
		return
	}

	switch btn {
	case "UP": // 'W' key for move forward
		if state == "1" {
			procKeybdEvent.Call(uintptr(VK_W), 0x11, 0, 0)
		} else {
			procKeybdEvent.Call(uintptr(VK_W), 0x11, KEYEVENTF_KEYUP, 0)
		}
	case "DOWN": // 'S' key for move backward
		if state == "1" {
			procKeybdEvent.Call(uintptr(VK_S), 0x1F, 0, 0)
		} else {
			procKeybdEvent.Call(uintptr(VK_S), 0x1F, KEYEVENTF_KEYUP, 0)
		}
	case "CENTER": // Spacebar for Jump
		if state == "1" {
			procKeybdEvent.Call(uintptr(VK_SPACE), 0x39, 0, 0)
		} else {
			procKeybdEvent.Call(uintptr(VK_SPACE), 0x39, KEYEVENTF_KEYUP, 0)
		}
	case "LEFT": // Mouse camera rotation left
		if state == "1" {
			procMouseEvent.Call(MOUSEEVENTF_MOVE, uintptr(-speed), 0, 0, 0)
		}
	case "RIGHT": // Mouse camera rotation right
		if state == "1" {
			procMouseEvent.Call(MOUSEEVENTF_MOVE, uintptr(speed), 0, 0, 0)
		}
	}
}

// Background continuous camera rotation while Left/Right buttons are held
func mouseRotationLoop() {
	ticker := time.NewTicker(25 * time.Millisecond)
	for range ticker.C {
		cfg.RLock()
		enabled := cfg.GamepadEnabled
		speed := cfg.MouseLookSpeed
		cfg.RUnlock()

		if !enabled {
			continue
		}

		btnMutex.RLock()
		leftHeld := buttonStates["LEFT"]
		rightHeld := buttonStates["RIGHT"]
		btnMutex.RUnlock()

		if leftHeld && !rightHeld {
			procMouseEvent.Call(MOUSEEVENTF_MOVE, uintptr(-speed), 0, 0, 0)
		} else if rightHeld && !leftHeld {
			procMouseEvent.Call(MOUSEEVENTF_MOVE, uintptr(speed), 0, 0, 0)
		}
	}
}

// Background serial reader
func serialReaderLoop() {
	buf := make([]byte, 128)
	var line strings.Builder

	for {
		serialMutex.Lock()
		h := serialHandle
		serialMutex.Unlock()

		if h == 0 || h == uintptr(syscall.InvalidHandle) {
			time.Sleep(500 * time.Millisecond)
			continue
		}

		var bytesRead uint32
		r, _, _ := procReadFile.Call(
			h,
			uintptr(unsafe.Pointer(&buf[0])),
			uintptr(len(buf)),
			uintptr(unsafe.Pointer(&bytesRead)),
			0,
		)

		if r != 0 && bytesRead > 0 {
			for i := 0; i < int(bytesRead); i++ {
				b := buf[i]
				if b == '\n' || b == '\r' {
					msg := strings.TrimSpace(line.String())
					line.Reset()
					if strings.HasPrefix(msg, "BTN:") {
						// Format: BTN:UP:1 or BTN:CENTER:0
						parts := strings.Split(msg, ":")
						if len(parts) >= 3 {
							btn := parts[1]
							st := parts[2]
							fmt.Printf("[Remote Button] %s -> %s\n", btn, st)
							handleButtonEvent(btn, st)
						}
					}
				} else {
					line.WriteByte(b)
				}
			}
		} else {
			time.Sleep(10 * time.Millisecond)
		}
	}
}

// Background video streaming loop
func streamerLoop() {
	for {
		cfg.RLock()
		fps := cfg.TargetFPS
		cMode := cfg.CaptureMode
		sMode := cfg.ScaleMode
		cfg.RUnlock()

		if fps <= 0 {
			fps = 25
		}
		interval := time.Duration(1000/fps) * time.Millisecond
		start := time.Now()

		img, err := captureScreen(cMode, sMode)
		if err == nil && img != nil {
			binData := processImageTo1Bit(img, &cfg)

			frameMutex.Lock()
			lastFrame = binData
			frameMutex.Unlock()

			// Build frame packet: [0xAA, 0x55, 0x01, ... 1024 bytes ..., 0x55, 0xAA]
			packet := make([]byte, 1029)
			packet[0] = 0xAA
			packet[1] = 0x55
			packet[2] = 0x01 // Frame command
			copy(packet[3:1027], binData[:])
			packet[1027] = 0x55
			packet[1028] = 0xAA

			_ = writeSerial(packet)
		}

		elapsed := time.Since(start)
		if elapsed < interval {
			time.Sleep(interval - elapsed)
		}
	}
}

// Built-in Web Control Dashboard
func startWebServer() {
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write([]byte(htmlDashboard))
	})

	http.HandleFunc("/api/config", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "POST" {
			var newCfg Config
			if err := json.NewDecoder(r.Body).Decode(&newCfg); err == nil {
				cfg.Lock()
				oldPort := cfg.ComPort
				cfg.ComPort = newCfg.ComPort
				cfg.TargetFPS = newCfg.TargetFPS
				cfg.CaptureMode = newCfg.CaptureMode
				cfg.ScaleMode = newCfg.ScaleMode
				cfg.DitherAlgo = newCfg.DitherAlgo
				cfg.Brightness = newCfg.Brightness
				cfg.Contrast = newCfg.Contrast
				cfg.Gamma = newCfg.Gamma
				cfg.InvertColors = newCfg.InvertColors
				cfg.ThresholdCutoff = newCfg.ThresholdCutoff
				cfg.MouseLookSpeed = newCfg.MouseLookSpeed
				cfg.GamepadEnabled = newCfg.GamepadEnabled
				cfg.Unlock()

				if oldPort != newCfg.ComPort && newCfg.ComPort != "" {
					closeSerialPort()
					h, err := openSerialPort(newCfg.ComPort, 115200)
					if err == nil {
						serialMutex.Lock()
						serialHandle = h
						serialMutex.Unlock()
						fmt.Printf("[Serial] Connected to %s\n", newCfg.ComPort)
					}
				}
			}
		}

		cfg.RLock()
		defer cfg.RUnlock()
		json.NewEncoder(w).Encode(cfg)
	})

	http.HandleFunc("/api/frame", func(w http.ResponseWriter, r *http.Request) {
		frameMutex.RLock()
		data := lastFrame
		frameMutex.RUnlock()

		w.Header().Set("Content-Type", "application/octet-stream")
		w.Write(data[:])
	})

	http.HandleFunc("/api/buttons", func(w http.ResponseWriter, r *http.Request) {
		btnMutex.RLock()
		defer btnMutex.RUnlock()
		json.NewEncoder(w).Encode(buttonStates)
	})

	http.HandleFunc("/api/connect", func(w http.ResponseWriter, r *http.Request) {
		port := r.URL.Query().Get("port")
		if port == "" {
			cfg.RLock()
			port = cfg.ComPort
			cfg.RUnlock()
		}

		closeSerialPort()
		h, err := openSerialPort(port, 115200)
		res := map[string]interface{}{}
		if err != nil {
			res["success"] = false
			res["error"] = err.Error()
		} else {
			serialMutex.Lock()
			serialHandle = h
			serialMutex.Unlock()
			res["success"] = true
			res["port"] = port
			cfg.Lock()
			cfg.ComPort = port
			cfg.Unlock()
			fmt.Printf("[Serial] Connected to %s\n", port)
		}
		json.NewEncoder(w).Encode(res)
	})

	fmt.Println("-----------------------------------------------------------------")
	fmt.Println("🚀 iPodify PC OLED Streamer & Gaming Controller Started!")
	fmt.Println("🌐 Web Control Dashboard: http://localhost:8085")
	fmt.Println("🎮 Gaming controls active:")
	fmt.Println("   - UP    -> 'W' key (Forward)")
	fmt.Println("   - DOWN  -> 'S' key (Backward)")
	fmt.Println("   - CENTER-> Spacebar (Jump)")
	fmt.Println("   - LEFT  -> Mouse Camera Turn Left")
	fmt.Println("   - RIGHT -> Mouse Camera Turn Right")
	fmt.Println("-----------------------------------------------------------------")

	_ = http.ListenAndServe(":8085", nil)
}

const htmlDashboard = `<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>iPodify OLED Streamer & Controls</title>
<style>
  :root { --bg: #0f172a; --card: #1e293b; --accent: #10b981; --accent-hover: #059669; --text: #f8fafc; --muted: #94a3b8; }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 24px; display: flex; justify-content: center; }
  .container { max-width: 900px; width: 100%; display: grid; grid-template-columns: 1fr 1fr; gap: 20px; }
  .card { background: var(--card); border-radius: 12px; padding: 20px; box-shadow: 0 4px 16px rgba(0,0,0,0.4); }
  .full { grid-column: 1 / -1; }
  h2 { font-size: 1.25rem; font-weight: 700; margin-bottom: 16px; color: var(--accent); display: flex; align-items: center; gap: 8px; }
  .screen-box { background: #000; border: 4px solid #334155; border-radius: 8px; padding: 12px; display: flex; flex-direction: column; align-items: center; justify-content: center; }
  canvas { image-rendering: pixelated; width: 100%; max-width: 512px; aspect-ratio: 2 / 1; background: #000; border-radius: 4px; box-shadow: 0 0 20px rgba(16,185,129,0.2); }
  .ctrl-group { margin-bottom: 14px; }
  label { display: flex; justify-content: space-between; font-size: 0.85rem; color: var(--muted); margin-bottom: 6px; }
  input[type=range] { width: 100%; accent-color: var(--accent); cursor: pointer; }
  select, input[type=text] { width: 100%; background: #0f172a; border: 1px solid #334155; color: #fff; padding: 8px 12px; border-radius: 6px; outline: none; }
  .btn { background: var(--accent); color: #fff; border: none; padding: 10px 16px; border-radius: 6px; cursor: pointer; font-weight: 600; width: 100%; transition: background 0.2s; }
  .btn:hover { background: var(--accent-hover); }
  .btn-row { display: flex; gap: 10px; }
  .keys-pad { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-top: 10px; }
  .key-btn { background: #334155; color: #fff; border: none; padding: 12px; border-radius: 6px; text-align: center; font-weight: bold; font-size: 0.85rem; transition: background 0.1s; }
  .key-btn.active { background: #10b981; color: #000; box-shadow: 0 0 12px #10b981; }
</style>
</head>
<body>
<div class="container">
  <div class="card full">
    <h2>📺 OLED Живой экран (128x64 Монохром)</h2>
    <div class="screen-box">
      <canvas id="oled" width="128" height="64"></canvas>
      <div style="font-size:0.75rem; color:var(--muted); margin-top:8px;">Стриминг экрана монитора на ESP32 OLED дисплей</div>
    </div>
  </div>

  <div class="card">
    <h2>⚙️ Настройки изображения и дизеринга</h2>
    
    <div class="ctrl-group">
      <label>Алгоритм дизеринга</label>
      <select id="ditherAlgo" onchange="updateCfg()">
        <option value="atkinson">Atkinson (Retro Mac / Sharp)</option>
        <option value="floyd">Floyd-Steinberg (Smooth gradient)</option>
        <option value="bayer4">Bayer 4x4 (Comic Halftone)</option>
        <option value="bayer8">Bayer 8x8 (90s LCD)</option>
        <option value="threshold">Threshold (High Contrast Cutoff)</option>
      </select>
    </div>

    <div class="ctrl-group">
      <label>Масштабирование</label>
      <select id="scaleMode" onchange="updateCfg()">
        <option value="fit">Fit (Сохранять пропорции)</option>
        <option value="stretch">Stretch (Растянуть 128x64)</option>
        <option value="crop">Crop (Обрезать по центру 2:1)</option>
      </select>
    </div>

    <div class="ctrl-group">
      <label>Область захвата</label>
      <select id="captureMode" onchange="updateCfg()">
        <option value="fullscreen">Весь монитор</option>
        <option value="cursor">Следить за курсором (FPS прицел)</option>
        <option value="center">Центр экрана</option>
      </select>
    </div>

    <div class="ctrl-group">
      <label>Яркость <span id="valBright">0</span></label>
      <input type="range" id="bright" min="-100" max="100" value="10" oninput="updateCfg()">
    </div>

    <div class="ctrl-group">
      <label>Контраст <span id="valContrast">1.3</span></label>
      <input type="range" id="contrast" min="0.2" max="3.0" step="0.1" value="1.3" oninput="updateCfg()">
    </div>

    <div class="ctrl-group">
      <label>Гамма <span id="valGamma">1.1</span></label>
      <input type="range" id="gamma" min="0.2" max="2.5" step="0.1" value="1.1" oninput="updateCfg()">
    </div>

    <div class="ctrl-group">
      <label style="cursor:pointer;"><input type="checkbox" id="invert" onchange="updateCfg()" style="margin-right:8px;"> Инвертировать цвета (Белый/Черный)</label>
    </div>

    <div class="ctrl-group">
      <label>FPS стриминга <span id="valFps">25</span></label>
      <input type="range" id="fps" min="5" max="60" value="25" oninput="updateCfg()">
    </div>
  </div>

  <div class="card">
    <h2>🎮 Игровое управление (5 кнопок iPod)</h2>
    <div style="font-size:0.85rem; color:var(--muted); margin-bottom:12px;">
      Физические кнопки iPod передают команды на ПК для управления в играх (DOOM, Minecraft, шутерах):
    </div>
    <div class="keys-pad">
      <div></div>
      <div id="btnUP" class="key-btn">▲ W<br><small>Вперед</small></div>
      <div></div>
      <div id="btnLEFT" class="key-btn">◀ Camera<br><small>Влево</small></div>
      <div id="btnCENTER" class="key-btn">● SPACE<br><small>Прыжок</small></div>
      <div id="btnRIGHT" class="key-btn">▶ Camera<br><small>Вправо</small></div>
      <div></div>
      <div id="btnDOWN" class="key-btn">▼ S<br><small>Назад</small></div>
      <div></div>
    </div>

    <div class="ctrl-group" style="margin-top:16px;">
      <label>Скорость поворота камеры (мышью) <span id="valSpeed">35</span></label>
      <input type="range" id="speed" min="5" max="100" value="35" oninput="updateCfg()">
    </div>

    <div class="ctrl-group">
      <label style="cursor:pointer;"><input type="checkbox" id="gamepad" checked onchange="updateCfg()" style="margin-right:8px;"> Включить эмуляцию кнопок в играх</label>
    </div>

    <h2 style="margin-top:20px;">🔌 Подключение к ESP32</h2>
    <div class="btn-row">
      <input type="text" id="comPort" value="COM8" placeholder="COM8">
      <button class="btn" style="width:140px;" onclick="connectPort()">Подключить</button>
    </div>
    <div id="connStatus" style="font-size:0.85rem; color:var(--muted); margin-top:8px;">Статус: Ожидание подключения</div>
  </div>
</div>

<script>
  const canvas = document.getElementById('oled');
  const ctx = canvas.getContext('2d');
  const imgData = ctx.createImageData(128, 64);

  async function fetchFrame() {
    try {
      const res = await fetch('/api/frame');
      const buf = await res.arrayBuffer();
      const bytes = new Uint8Array(buf);
      if (bytes.length === 1024) {
        for (let y = 0; y < 64; y++) {
          const page = Math.floor(y / 8);
          const bit = y % 8;
          for (let x = 0; x < 128; x++) {
            const isWhite = (bytes[page * 128 + x] & (1 << bit)) !== 0;
            const idx = (y * 128 + x) * 4;
            const val = isWhite ? 255 : 0;
            imgData.data[idx] = val;
            imgData.data[idx+1] = isWhite ? 240 : 0;
            imgData.data[idx+2] = isWhite ? 220 : 0;
            imgData.data[idx+3] = 255;
          }
        }
        ctx.putImageData(imgData, 0, 0);
      }
    } catch(e) {}
    requestAnimationFrame(fetchFrame);
  }
  requestAnimationFrame(fetchFrame);

  async function pollButtons() {
    try {
      const res = await fetch('/api/buttons');
      const btns = await res.json();
      ['UP', 'DOWN', 'LEFT', 'RIGHT', 'CENTER'].forEach(b => {
        const el = document.getElementById('btn' + b);
        if (el) {
          if (btns[b]) el.classList.add('active');
          else el.classList.remove('active');
        }
      });
    } catch(e) {}
    setTimeout(pollButtons, 80);
  }
  pollButtons();

  function updateCfg() {
    const data = {
      com_port: document.getElementById('comPort').value,
      dither_algo: document.getElementById('ditherAlgo').value,
      scale_mode: document.getElementById('scaleMode').value,
      capture_mode: document.getElementById('captureMode').value,
      brightness: parseFloat(document.getElementById('bright').value),
      contrast: parseFloat(document.getElementById('contrast').value),
      gamma: parseFloat(document.getElementById('gamma').value),
      invert_colors: document.getElementById('invert').checked,
      target_fps: parseInt(document.getElementById('fps').value),
      mouse_look_speed: parseInt(document.getElementById('speed').value),
      gamepad_enabled: document.getElementById('gamepad').checked
    };
    document.getElementById('valBright').innerText = data.brightness;
    document.getElementById('valContrast').innerText = data.contrast;
    document.getElementById('valGamma').innerText = data.gamma;
    document.getElementById('valFps').innerText = data.target_fps;
    document.getElementById('valSpeed').innerText = data.mouse_look_speed;

    fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data)
    });
  }

  async function connectPort() {
    const port = document.getElementById('comPort').value;
    document.getElementById('connStatus').innerText = 'Подключение к ' + port + '...';
    try {
      const res = await fetch('/api/connect?port=' + encodeURIComponent(port));
      const j = await res.json();
      if (j.success) {
        document.getElementById('connStatus').innerText = '✅ Успешно подключено к ' + port;
        document.getElementById('connStatus').style.color = '#10b981';
      } else {
        document.getElementById('connStatus').innerText = '❌ Ошибка: ' + j.error;
        document.getElementById('connStatus').style.color = '#ef4444';
      }
    } catch(e) {
      document.getElementById('connStatus').innerText = '❌ Ошибка сети: ' + e;
    }
  }

  // Load config on startup
  fetch('/api/config').then(r => r.json()).then(c => {
    document.getElementById('comPort').value = c.com_port || 'COM8';
    document.getElementById('ditherAlgo').value = c.dither_algo || 'atkinson';
    document.getElementById('scaleMode').value = c.scale_mode || 'fit';
    document.getElementById('captureMode').value = c.capture_mode || 'fullscreen';
    document.getElementById('bright').value = c.brightness || 0;
    document.getElementById('contrast').value = c.contrast || 1.3;
    document.getElementById('gamma').value = c.gamma || 1.1;
    document.getElementById('invert').checked = !!c.invert_colors;
    document.getElementById('fps').value = c.target_fps || 25;
    document.getElementById('speed').value = c.mouse_look_speed || 35;
    document.getElementById('gamepad').checked = c.gamepad_enabled !== false;
    document.getElementById('valBright').innerText = c.brightness || 0;
    document.getElementById('valContrast').innerText = c.contrast || 1.3;
    document.getElementById('valGamma').innerText = c.gamma || 1.1;
    document.getElementById('valFps').innerText = c.target_fps || 25;
    document.getElementById('valSpeed').innerText = c.mouse_look_speed || 35;
  });
</script>
</body>
</html>
`

func main() {
	// Auto-detect and attempt connect to COM8 or first available
	if h, err := openSerialPort(cfg.ComPort, cfg.BaudRate); err == nil {
		serialHandle = h
		fmt.Printf("[Serial] Successfully connected to %s (%d baud)\n", cfg.ComPort, cfg.BaudRate)
	} else {
		fmt.Printf("[Serial] Notice: %s not open yet (%v). You can select COM port in Web Dashboard.\n", cfg.ComPort, err)
	}

	go mouseRotationLoop()
	go serialReaderLoop()
	go streamerLoop()
	go startWebServer()

	// Wait for Ctrl+C
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	<-sig

	closeSerialPort()
	fmt.Println("\nExiting PC streamer.")
}
