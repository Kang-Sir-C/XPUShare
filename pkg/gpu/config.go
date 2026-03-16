package gpu

import (
	"os"
	"strings"
)

// Config is shared by all XPUShare components.
//
// It intentionally keeps to environment variables to avoid adding extra config
// plumbing across the scheduler framework and sidecars.
type Config struct {
	// Provider selects the GPU provider implementation used by the collector.
	// Known values: "iluvatar", "nvidia-smi", "mthreads-gmi", "metax-mxsmi", "biren-brsmi", "hygon-rocmsmi", "ascend-npusmi", "generic-cmd".
	Provider string

	// VisibleDevicesEnv overrides the env var injected into containers to
	// select visible devices (e.g., NVIDIA_VISIBLE_DEVICES, IX_VISIBLE_DEVICES).
	VisibleDevicesEnv string

	// VisibleDevicesValue selects what value to put into the visible-devices env:
	// - "uuid": use device UUIDs (stable across nodes)
	// - "index": use device indices (0..N-1 on the node)
	//
	// Different domestic runtimes may only accept indices. NVIDIA generally
	// accepts UUIDs or indices (via NVIDIA_VISIBLE_DEVICES).
	VisibleDevicesValue string

	// HookLibraryName is the basename under /xpushare/library that will be used
	// as LD_PRELOAD for fractional GPU pods.
	HookLibraryName string

	// EnforcementLayer selects which API layer enforces quotas/tokens in the
	// injected interposition library.
	// Known values: "driver", "runtime", "profile", "none".
	EnforcementLayer string
}

func LoadConfigFromEnv() Config {
	hook := os.Getenv("XPU_HOOK_LIBRARY")
	if hook == "" {
		hook = "libxhook.so.1"
	}
	cfg := Config{
		Provider:        getenvDefault("XPU_PROVIDER", "iluvatar"),
		VisibleDevicesEnv: os.Getenv("XPU_VISIBLE_DEVICES_ENV"),
		VisibleDevicesValue: os.Getenv("XPU_VISIBLE_DEVICES_VALUE"),
		HookLibraryName: hook,
		EnforcementLayer: os.Getenv("XPU_ENFORCEMENT_LAYER"),
	}
	return cfg
}

func (c Config) HookEnabled() bool {
	v := strings.TrimSpace(strings.ToLower(c.HookLibraryName))
	return v != "" && v != "none" && v != "disable" && v != "disabled"
}

func (c Config) VisibleDevicesEnvVars() []string {
	// Allow comma-separated env var names so we can satisfy runtimes that have
	// multiple layers (e.g., container runtime + user runtime).
	// Example: "MTHREADS_VISIBLE_DEVICES,MUSA_VISIBLE_DEVICES"
	if strings.TrimSpace(c.VisibleDevicesEnv) != "" {
		parts := strings.Split(c.VisibleDevicesEnv, ",")
		out := make([]string, 0, len(parts))
		for _, p := range parts {
			name := strings.TrimSpace(p)
			if name == "" {
				continue
			}
			out = append(out, name)
		}
		if len(out) > 0 {
			return out
		}
	}
	switch c.Provider {
	case "nvidia", "nvidia-smi":
		return []string{"NVIDIA_VISIBLE_DEVICES"}
	case "mthreads", "mthreads-gmi":
		// MooreThreads exposes both a container-runtime selector (often
		// MTHREADS_VISIBLE_DEVICES) and a user-runtime selector (often
		// MUSA_VISIBLE_DEVICES). Keep the default conservative.
		return []string{"MUSA_VISIBLE_DEVICES"}
	case "metax", "metax-mxsmi":
		// MetaX uses MACA_VISIBLE_DEVICES while many frameworks set
		// CUDA_VISIBLE_DEVICES (see vllm-metax patch notes). Inject both by
		// default to avoid mismatch.
		return []string{"CUDA_VISIBLE_DEVICES", "MACA_VISIBLE_DEVICES"}
	case "biren", "biren-brsmi":
		// Biren commonly targets CUDA-like frameworks; default to the standard
		// CUDA selector. Override via XPU_VISIBLE_DEVICES_ENV if your runtime
		// expects a vendor-specific name.
		return []string{"CUDA_VISIBLE_DEVICES"}
	case "hygon", "hygon-rocmsmi":
		return []string{"HIP_VISIBLE_DEVICES"}
	case "ascend", "ascend-npusmi":
		return []string{"ASCEND_RT_VISIBLE_DEVICES"}
	case "generic-cmd":
		// Require an explicit override for unknown runtimes.
		return []string{"VISIBLE_DEVICES"}
	case "iluvatar":
		fallthrough
	default:
		return []string{"IX_VISIBLE_DEVICES"}
	}
}

func (c Config) VisibleDevicesValueMode() string {
	if c.VisibleDevicesValue != "" {
		switch c.VisibleDevicesValue {
		case "uuid", "index":
			return c.VisibleDevicesValue
		default:
			// fall back to provider defaults if misconfigured
		}
	}

	// Provider defaults:
	// - Iluvatar deployments in this repo historically used per-node indices.
	// - MooreThreads uses per-node indices for MUSA_VISIBLE_DEVICES.
	// - NVIDIA prefers UUIDs for uniqueness, but indices also work.
	switch c.Provider {
	case "iluvatar", "mthreads", "mthreads-gmi", "metax", "metax-mxsmi", "biren", "biren-brsmi", "hygon", "hygon-rocmsmi", "ascend", "ascend-npusmi":
		return "index"
	default:
		return "uuid"
	}
}

func (c Config) EnforcementLayerMode() string {
	if strings.TrimSpace(c.EnforcementLayer) != "" {
		v := strings.TrimSpace(strings.ToLower(c.EnforcementLayer))
		switch v {
		case "driver", "runtime", "profile", "none", "off", "disable", "disabled":
			// normalize off/disable variants to "none"
			if v == "off" || v == "disable" || v == "disabled" {
				return "none"
			}
			return v
		}
	}

	// Provider defaults:
	// - Iluvatar showed runtime bypass; default to runtime enforcement.
	// - Others default to driver enforcement for compatibility.
	switch c.Provider {
	case "iluvatar":
		return "runtime"
	case "hygon", "hygon-rocmsmi":
		return "runtime" // HIP has no clean driver/runtime split
	case "ascend", "ascend-npusmi":
		return "runtime" // Only memory isolation via runtime interception
	default:
		return "driver"
	}
}

func getenvDefault(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
