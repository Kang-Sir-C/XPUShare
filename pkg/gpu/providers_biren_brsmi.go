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

// BirenTech device discovery via `brsmi`.
//
// The public Biren container ecosystem (BIRENSUPA) typically exposes the device
// node as /dev/biren and ships a `brsmi`-like management tool. This provider
// parses the default table output and emits a per-node synthetic UUID:
//   NODE_NAME + "-biren-<index>"
//
// If your `brsmi` output differs or requires flags, prefer XPU_PROVIDER=generic-cmd.
type birenBrsmiProvider struct {
	nodeName string
}

func newBirenBrsmiProvider() Provider { return &birenBrsmiProvider{} }

func (p *birenBrsmiProvider) Name() string { return "biren-brsmi" }
func (p *birenBrsmiProvider) Init() error {
	p.nodeName = strings.TrimSpace(os.Getenv("NODE_NAME"))
	return nil
}
func (p *birenBrsmiProvider) Shutdown() error { return nil }

// Try to match a common SMI-style row that includes memory like:
//   ... 3419MiB(16384MiB)
//   ... 3419MB(16384MB)
var birenBrsmiLineRe = regexp.MustCompile(`^\s*(\d+)\s+(.+?)\s+\|.*\|\s*\d+%\s+(\d+)\s*(?:MiB|MB)\s*\(\s*(\d+)\s*(?:MiB|MB)\s*\)\s*$`)

func (p *birenBrsmiProvider) Devices() ([]Device, error) {
	bin := "brsmi"
	if _, err := exec.LookPath(bin); err != nil {
		// Common mount path used by device plugins/toolkits.
		if _, err2 := os.Stat("/usr/bin/brsmi"); err2 == nil {
			bin = "/usr/bin/brsmi"
		}
	}

	out, err := exec.Command(bin).CombinedOutput()
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
		m := birenBrsmiLineRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}

		index, err := strconv.Atoi(m[1])
		if err != nil {
			return nil, fmt.Errorf("%s parse index %q: %w", bin, m[1], err)
		}
		model := strings.ReplaceAll(strings.TrimSpace(m[2]), " ", "-")

		totalMiB, err := strconv.ParseUint(m[4], 10, 64)
		if err != nil {
			return nil, fmt.Errorf("%s parse totalMiB %q: %w", bin, m[4], err)
		}

		uuid := fmt.Sprintf("biren-%d", index)
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

