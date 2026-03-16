//go:build linux

package gpu

import (
	"fmt"
	"strings"

	"gitee.com/deep-spark/go-ixml/pkg/ixml"
)

type iluvatarProvider struct{}

func newIluvatarProvider() Provider { return &iluvatarProvider{} }

func (p *iluvatarProvider) Name() string { return "iluvatar" }

func (p *iluvatarProvider) Init() error {
	if ret := ixml.Init(); ret != ixml.SUCCESS {
		return fmt.Errorf("ixml.Init failed: %s", ixml.ErrorString(ret))
	}
	return nil
}

func (p *iluvatarProvider) Shutdown() error {
	ret := ixml.Shutdown()
	if ret != ixml.SUCCESS {
		return fmt.Errorf("ixml.Shutdown failed: %s", ixml.ErrorString(ret))
	}
	return nil
}

func (p *iluvatarProvider) Devices() ([]Device, error) {
	n, ret := ixml.DeviceGetCount()
	if ret != ixml.SUCCESS {
		return nil, fmt.Errorf("ixml.DeviceGetCount failed: %s", ixml.ErrorString(ret))
	}

	devices := make([]Device, 0, n)
	for i := uint(0); i < n; i++ {
		var dev ixml.Device
		ret := ixml.DeviceGetHandleByIndex(i, &dev)
		if ret != ixml.SUCCESS {
			return nil, fmt.Errorf("ixml.DeviceGetHandleByIndex(%d) failed: %s", i, ixml.ErrorString(ret))
		}

		uuid, ret := dev.GetUUID()
		if ret != ixml.SUCCESS {
			return nil, fmt.Errorf("ixml.Device.GetUUID(%d) failed: %s", i, ixml.ErrorString(ret))
		}

		mem, ret := dev.GetMemoryInfo()
		if ret != ixml.SUCCESS {
			return nil, fmt.Errorf("ixml.Device.GetMemoryInfo(%d) failed: %s", i, ixml.ErrorString(ret))
		}

		name, ret := dev.GetName()
		if ret != ixml.SUCCESS {
			return nil, fmt.Errorf("ixml.Device.GetName(%d) failed: %s", i, ixml.ErrorString(ret))
		}

		// go-ixml reports MB, align to bytes like nvml collector.
		memoryBytes := uint64(mem.Total) * 1024 * 1024
		model := strings.ReplaceAll(name, " ", "-")

		devices = append(devices, Device{
			UUID:        uuid,
			Model:       model,
			MemoryBytes: memoryBytes,
			Index:       int(i),
		})
	}
	return devices, nil
}

