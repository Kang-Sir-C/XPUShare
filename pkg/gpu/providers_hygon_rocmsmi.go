package gpu

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
)

// Hygon DCU device discovery via `hy-smi` or `rocm-smi`.
//
// Hygon DCUs are ROCm-compatible accelerators. This provider tries `hy-smi`
// first (Hygon's vendor tool) and falls back to `rocm-smi` which ships with
// the standard ROCm stack.
//
// The provider parses the default human-readable table output and emits a
// per-node synthetic UUID (NODE_NAME + "-hygon-<index>").
//
// For container binding, Hygon/ROCm runtimes typically use:
//   - HIP_VISIBLE_DEVICES
type hygonRocmsmiProvider struct {
	nodeName string
}

func newHygonRocmsmiProvider() Provider { return &hygonRocmsmiProvider{} }

func (p *hygonRocmsmiProvider) Name() string { return "hygon-rocmsmi" }
func (p *hygonRocmsmiProvider) Init() error {
	p.nodeName = strings.TrimSpace(os.Getenv("NODE_NAME"))
	return nil
}
func (p *hygonRocmsmiProvider) Shutdown() error { return nil }

// rocm-smi / hy-smi typical table row (varies by version):
//   0        K100_AI        0000:01:00.0   3419MiB / 16384MiB   0%
// We also handle lines like:
//   GPU[0]  : K100_AI  ...  3419MiB(16384MiB)
var hygonLineRe = regexp.MustCompile(
	`(?:GPU\[)?(\d+)\]?\s+[:\s]*(.+?)\s+(?:.*?\s)?(\d+)\s*(?:MiB|MB)\s*[/(]\s*(\d+)\s*(?:MiB|MB)`,
)

func (p *hygonRocmsmiProvider) Devices() ([]Device, error) {
	bin, args := p.resolveCommand()

	cmd := exec.Command(bin, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("%s failed: %w; output=%s", bin, err, string(out))
	}

	var devices []Device
	sc := bufio.NewScanner(bytes.NewReader(out))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		m := hygonLineRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}

		index, err := strconv.Atoi(m[1])
		if err != nil {
			continue
		}
		model := strings.ReplaceAll(strings.TrimSpace(m[2]), " ", "-")

		totalMiB, err := strconv.ParseUint(m[4], 10, 64)
		if err != nil {
			continue
		}

		uuid := fmt.Sprintf("hygon-%d", index)
		if p.nodeName != "" {
			uuid = fmt.Sprintf("%s-%s", p.nodeName, uuid)
		}

		devices = append(devices, Device{
			UUID:        uuid,
			Model:       model,
			MemoryBytes: totalMiB * 1024 * 1024,
			Index:       index,
		})
	}
	if err := sc.Err(); err != nil {
		return nil, fmt.Errorf("%s output scan error: %w", bin, err)
	}

	if len(devices) == 0 {
		return nil, fmt.Errorf("%s returned no parseable device lines; consider using XPU_PROVIDER=generic-cmd with a custom parser", bin)
	}
	return devices, nil
}

// resolveCommand picks hy-smi if available, otherwise rocm-smi.
func (p *hygonRocmsmiProvider) resolveCommand() (string, []string) {
	if path, err := exec.LookPath("hy-smi"); err == nil {
		return path, nil
	}
	return "rocm-smi", []string{"--showid", "--showproductname", "--showmeminfo", "vram"}
}
