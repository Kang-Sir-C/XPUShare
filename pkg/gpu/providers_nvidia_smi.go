package gpu

import (
	"bufio"
	"bytes"
	"fmt"
	"os/exec"
	"strconv"
	"strings"
)

type nvidiaSmiProvider struct{}

func newNvidiaSmiProvider() Provider { return &nvidiaSmiProvider{} }

func (p *nvidiaSmiProvider) Name() string { return "nvidia-smi" }
func (p *nvidiaSmiProvider) Init() error  { return nil }
func (p *nvidiaSmiProvider) Shutdown() error {
	return nil
}

func (p *nvidiaSmiProvider) Devices() ([]Device, error) {
	// Using `nounits` so memory.total is an integer in MiB.
	cmd := exec.Command("nvidia-smi", "--query-gpu=index,uuid,name,memory.total", "--format=csv,noheader,nounits")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("nvidia-smi failed: %w; output=%s", err, string(out))
	}

	var devices []Device
	sc := bufio.NewScanner(bytes.NewReader(out))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}

		// Example: 0, GPU-xxxx..., NVIDIA A100-SXM4-40GB, 40536
		parts := strings.Split(line, ",")
		if len(parts) < 4 {
			return nil, fmt.Errorf("unexpected nvidia-smi line: %q", line)
		}
		indexStr := strings.TrimSpace(parts[0])
		index, err := strconv.Atoi(indexStr)
		if err != nil {
			return nil, fmt.Errorf("unexpected nvidia-smi index %q: %w", indexStr, err)
		}
		uuid := strings.TrimSpace(parts[1])
		model := strings.ReplaceAll(strings.TrimSpace(parts[2]), " ", "-")
		memMiBStr := strings.TrimSpace(parts[3])
		memMiB, err := strconv.ParseUint(memMiBStr, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("unexpected nvidia-smi memory.total %q: %w", memMiBStr, err)
		}
		devices = append(devices, Device{
			UUID:        uuid,
			Model:       model,
			MemoryBytes: memMiB * 1024 * 1024,
			Index:       index,
		})
	}
	if err := sc.Err(); err != nil {
		return nil, fmt.Errorf("nvidia-smi output scan error: %w", err)
	}
	return devices, nil
}
