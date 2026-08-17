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

// Huawei Ascend NPU device discovery via `npu-smi info`.
//
// `npu-smi info` prints a table with columns like:
//   NPU  Chip  Health  Power  Temp  Hugepages  Chip  Bus-Id  AICore  Memory(MB)
//   0     0     OK     68W    45    0/0        910B  ...     0       0/32768
//
// This provider parses that table to extract device index, model, and total
// memory. A per-node synthetic UUID is emitted: NODE_NAME + "-ascend-<index>".
//
// For container binding, Ascend runtimes use:
//   - ASCEND_RT_VISIBLE_DEVICES
type ascendNpusmiProvider struct {
	nodeName string
}

func newAscendNpusmiProvider() Provider { return &ascendNpusmiProvider{} }

func (p *ascendNpusmiProvider) Name() string { return "ascend-npusmi" }
func (p *ascendNpusmiProvider) Init() error {
	p.nodeName = strings.TrimSpace(os.Getenv("NODE_NAME"))
	return nil
}
func (p *ascendNpusmiProvider) Shutdown() error { return nil }

// Match a table row that contains NPU index, chip model, and memory like:
//   0     0     OK     68W    45    0/0        910B  0000:C1:00.0  0       0 / 32768
// We capture: (npu_id)  ...  (chip_model)  ...  used / (total) at the end.
var ascendLineRe = regexp.MustCompile(
	`^\s*(\d+)\s+\d+\s+\w+\s+\S+\s+\S+\s+\S+\s+(\S+)\s+\S+\s+\S+\s+\S+\s*/\s*(\d+)`,
)

func (p *ascendNpusmiProvider) Devices() ([]Device, error) {
	out, err := exec.Command("npu-smi", "info").CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("npu-smi info failed: %w; output=%s", err, string(out))
	}

	var devices []Device
	seen := make(map[int]bool)
	sc := bufio.NewScanner(bytes.NewReader(out))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		m := ascendLineRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}

		index, err := strconv.Atoi(m[1])
		if err != nil {
			continue
		}
		// npu-smi may list multiple chips per NPU; take the first occurrence.
		if seen[index] {
			continue
		}
		seen[index] = true

		chipModel := strings.TrimSpace(m[2])
		model := "Ascend" + chipModel

		totalMB, err := strconv.ParseUint(strings.TrimSpace(m[3]), 10, 64)
		if err != nil {
			continue
		}

		uuid := fmt.Sprintf("ascend-%d", index)
		if p.nodeName != "" {
			uuid = fmt.Sprintf("%s-%s", p.nodeName, uuid)
		}

		devices = append(devices, Device{
			UUID:        uuid,
			Model:       model,
			MemoryBytes: totalMB * 1024 * 1024,
			Index:       index,
		})
	}
	if err := sc.Err(); err != nil {
		return nil, fmt.Errorf("npu-smi output scan error: %w", err)
	}

	if len(devices) == 0 {
		return nil, fmt.Errorf("npu-smi returned no parseable device lines; consider using XPU_PROVIDER=generic-cmd with a custom parser")
	}
	return devices, nil
}
