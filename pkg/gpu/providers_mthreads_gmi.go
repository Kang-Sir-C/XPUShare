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

// MooreThreads device discovery via `mthreads-gmi`.
//
// This provider parses the default human-readable table output and emits a
// per-node synthetic UUID (NODE_NAME + "-mthreads-<index>") to keep scheduler
// state unique across nodes even if a stable hardware UUID is not available.
//
// For container binding, MooreThreads runtimes may use:
// - container runtime selector: MTHREADS_VISIBLE_DEVICES
// - user runtime selector: MUSA_VISIBLE_DEVICES
//
// Use XPU_VISIBLE_DEVICES_ENV to override and (optionally) set both via comma.
type mthreadsGmiProvider struct {
	nodeName string
}

func newMthreadsGmiProvider() Provider { return &mthreadsGmiProvider{} }

func (p *mthreadsGmiProvider) Name() string { return "mthreads-gmi" }
func (p *mthreadsGmiProvider) Init() error {
	p.nodeName = strings.TrimSpace(os.Getenv("NODE_NAME"))
	return nil
}
func (p *mthreadsGmiProvider) Shutdown() error { return nil }

// Example line (from `mthreads-gmi`):
// 0    MTT S80        |00000000:01:00.0    |0%    3419MiB(16384MiB)
var mthreadsGmiLineRe = regexp.MustCompile(`^\s*(\d+)\s+(.+?)\s+\|.*\|\s*\d+%\s+(\d+)MiB\((\d+)MiB\)\s*$`)

func (p *mthreadsGmiProvider) Devices() ([]Device, error) {
	out, err := exec.Command("mthreads-gmi").CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("mthreads-gmi failed: %w; output=%s", err, string(out))
	}

	var devices []Device
	sc := bufio.NewScanner(bytes.NewReader(out))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		m := mthreadsGmiLineRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}

		index, err := strconv.Atoi(m[1])
		if err != nil {
			return nil, fmt.Errorf("mthreads-gmi parse index %q: %w", m[1], err)
		}
		model := strings.ReplaceAll(strings.TrimSpace(m[2]), " ", "-")

		totalMiB, err := strconv.ParseUint(m[4], 10, 64)
		if err != nil {
			return nil, fmt.Errorf("mthreads-gmi parse totalMiB %q: %w", m[4], err)
		}

		uuid := fmt.Sprintf("mthreads-%d", index)
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
		return nil, fmt.Errorf("mthreads-gmi output scan error: %w", err)
	}

	if len(devices) == 0 {
		return nil, fmt.Errorf("mthreads-gmi returned no parseable device lines; consider using XPU_PROVIDER=generic-cmd with a custom parser")
	}
	return devices, nil
}

