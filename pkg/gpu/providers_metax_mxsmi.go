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

// MetaX (沐曦) device discovery via `mx-smi`.
//
// MetaX runtimes use `MACA_VISIBLE_DEVICES` while many frameworks set
// `CUDA_VISIBLE_DEVICES`. XPUShare can inject both env vars (comma-separated)
// to keep them consistent.
type metaxMxsmiProvider struct {
	nodeName string
}

func newMetaxMxsmiProvider() Provider { return &metaxMxsmiProvider{} }

func (p *metaxMxsmiProvider) Name() string { return "metax-mxsmi" }
func (p *metaxMxsmiProvider) Init() error {
	p.nodeName = strings.TrimSpace(os.Getenv("NODE_NAME"))
	return nil
}
func (p *metaxMxsmiProvider) Shutdown() error { return nil }

// We intentionally parse the default `mx-smi` table output (no args),
// which typically includes per-device memory in the form:
//   <used>MiB(<total>MiB)
//
// If your `mx-smi` output differs, prefer XPU_PROVIDER=generic-cmd.
var metaxMxsmiLineRe = regexp.MustCompile(`^\s*(\d+)\s+(.+?)\s+\|.*?(\d+)\s*(?:MiB|MB)\s*\(\s*(\d+)\s*(?:MiB|MB)\s*\)\s*$`)

func (p *metaxMxsmiProvider) Devices() ([]Device, error) {
	out, err := exec.Command("mx-smi").CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("mx-smi failed: %w; output=%s", err, string(out))
	}

	var devices []Device
	sc := bufio.NewScanner(bytes.NewReader(out))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		m := metaxMxsmiLineRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}

		index, err := strconv.Atoi(m[1])
		if err != nil {
			return nil, fmt.Errorf("mx-smi parse index %q: %w", m[1], err)
		}
		model := strings.ReplaceAll(strings.TrimSpace(m[2]), " ", "-")

		totalMiB, err := strconv.ParseUint(m[4], 10, 64)
		if err != nil {
			return nil, fmt.Errorf("mx-smi parse totalMiB %q: %w", m[4], err)
		}

		uuid := fmt.Sprintf("metax-%d", index)
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
		return nil, fmt.Errorf("mx-smi output scan error: %w", err)
	}

	if len(devices) == 0 {
		return nil, fmt.Errorf("mx-smi returned no parseable device lines; consider using XPU_PROVIDER=generic-cmd with a custom parser")
	}
	return devices, nil
}

