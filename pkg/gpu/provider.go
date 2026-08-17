package gpu

import "fmt"

type Device struct {
	UUID        string
	Model       string
	MemoryBytes uint64
	Index       int
}

type Provider interface {
	Name() string
	Init() error
	Shutdown() error
	Devices() ([]Device, error)
}

func NewProvider(cfg Config) (Provider, error) {
	switch cfg.Provider {
	case "iluvatar":
		return newIluvatarProvider(), nil
	case "nvidia", "nvidia-smi":
		return newNvidiaSmiProvider(), nil
	case "mthreads", "mthreads-gmi":
		return newMthreadsGmiProvider(), nil
	case "metax", "metax-mxsmi":
		return newMetaxMxsmiProvider(), nil
	case "biren", "biren-brsmi":
		return newBirenBrsmiProvider(), nil
	case "hygon", "hygon-rocmsmi":
		return newHygonRocmsmiProvider(), nil
	case "ascend", "ascend-npusmi":
		return newAscendNpusmiProvider(), nil
	case "generic-cmd":
		return newGenericCmdProviderFromEnv(), nil
	default:
		return nil, fmt.Errorf("unknown XPU_PROVIDER %q (supported: iluvatar, nvidia|nvidia-smi, mthreads|mthreads-gmi, metax|metax-mxsmi, biren|biren-brsmi, hygon|hygon-rocmsmi, ascend|ascend-npusmi, generic-cmd)", cfg.Provider)
	}
}
