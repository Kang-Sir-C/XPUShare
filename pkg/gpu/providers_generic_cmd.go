package gpu

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
)

// generic-cmd provider:
// - Reads a command from XPU_DEVICE_LIST_CMD.
// - Executes it and parses each output line as:
//     uuid,model,memory_bytes,index
//
// This is the "interface" for bringing up new domestic GPUs without changing
// XPUShare code first; later you can replace it with a native provider.
type genericCmdProvider struct {
	cmd string
}

func newGenericCmdProviderFromEnv() Provider {
	return &genericCmdProvider{
		cmd: os.Getenv("XPU_DEVICE_LIST_CMD"),
	}
}

func (p *genericCmdProvider) Name() string { return "generic-cmd" }
func (p *genericCmdProvider) Init() error {
	if strings.TrimSpace(p.cmd) == "" {
		return fmt.Errorf("XPU_DEVICE_LIST_CMD is required when XPU_PROVIDER=generic-cmd")
	}
	return nil
}
func (p *genericCmdProvider) Shutdown() error { return nil }

func (p *genericCmdProvider) Devices() ([]Device, error) {
	// Use `sh -lc` so users can provide pipelines.
	out, err := exec.Command("sh", "-lc", p.cmd).CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("XPU_DEVICE_LIST_CMD failed: %w; output=%s", err, string(out))
	}

	var devices []Device
	sc := bufio.NewScanner(bytes.NewReader(out))
	lineNo := 0
	for sc.Scan() {
		lineNo++
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.Split(line, ",")
		if len(parts) != 4 {
			return nil, fmt.Errorf("XPU_DEVICE_LIST_CMD invalid line %d: want 4 comma-separated fields, got %q", lineNo, line)
		}
		uuid := strings.TrimSpace(parts[0])
		model := strings.ReplaceAll(strings.TrimSpace(parts[1]), " ", "-")
		memBytes, err := strconv.ParseUint(strings.TrimSpace(parts[2]), 10, 64)
		if err != nil {
			return nil, fmt.Errorf("XPU_DEVICE_LIST_CMD invalid memory_bytes at line %d: %w", lineNo, err)
		}
		index, err := strconv.Atoi(strings.TrimSpace(parts[3]))
		if err != nil {
			return nil, fmt.Errorf("XPU_DEVICE_LIST_CMD invalid index at line %d: %w", lineNo, err)
		}
		if uuid == "" {
			return nil, fmt.Errorf("XPU_DEVICE_LIST_CMD empty uuid at line %d", lineNo)
		}
		devices = append(devices, Device{
			UUID:        uuid,
			Model:       model,
			MemoryBytes: memBytes,
			Index:       index,
		})
	}
	if err := sc.Err(); err != nil {
		return nil, fmt.Errorf("XPU_DEVICE_LIST_CMD output scan error: %w", err)
	}
	return devices, nil
}

