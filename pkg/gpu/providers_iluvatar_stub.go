//go:build !linux

package gpu

import "fmt"

type iluvatarProvider struct{}

func newIluvatarProvider() Provider { return &iluvatarProvider{} }

func (p *iluvatarProvider) Name() string { return "iluvatar" }
func (p *iluvatarProvider) Init() error {
	return fmt.Errorf("iluvatar provider requires linux build environment (go-ixml depends on linux/cgo)")
}
func (p *iluvatarProvider) Shutdown() error { return nil }
func (p *iluvatarProvider) Devices() ([]Device, error) {
	return nil, fmt.Errorf("iluvatar provider requires linux build environment (go-ixml depends on linux/cgo)")
}

