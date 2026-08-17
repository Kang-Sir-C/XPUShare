package scheduler

import (
	"os"
	"reflect"
	"strconv"

	"xpushare/pkg/lib/queue"

	"github.com/fsnotify/fsnotify"
	"github.com/spf13/viper"
	"gopkg.in/yaml.v2"
)

type Config struct {
	// specify the whole physical cluster
	// key: celltype -> val: cellTypeSpec
	// TODO: Automatically construct it based on node info from Device Plugins
	CellTypes map[string]CellTypeSpec `yaml:"cellTypes"`
	Cells     []CellSpec              `yaml:"cells"`
}

type CellTypeSpec struct {
	ChildCellType     string `yaml:"childCellType"`
	ChildCellNumber   int32  `yaml:"childCellNumber"`
	ChildCellPriority int32  `yaml:"childCellPriority"`
	IsNodeLevel       bool   `yaml:"isNodeLevel"`
}

// Specify Cell instances -> node-level or above
type CellSpec struct {
	CellType     string     `yaml:"cellType"`
	CellID       string     `yaml:"cellId"`
	CellChildren []CellSpec `yaml:"cellChildren,omitempty"`
}
/*
func (kss *XPUShareScheduler) initRawConfig() *Config {
	var c Config

	configPath := kss.args.XPUShareConfig
	// convert raw data to yaml
	yamlBytes, err := os.ReadFile(configPath)
	if err != nil {
		kss.ksl.Errorf("Failed to read config file: %v, %v", configPath, err)
	}

	// 🔴 最终诊断 1: 打印读取到的 YAML 文件的原始内容
    kss.ksl.Errorf("[DEBUG RAW] Reading config file at %s, size: %d bytes.", configPath, len(yamlBytes))
    kss.ksl.Errorf("[DEBUG RAW] YAML Content:\n%s", string(yamlBytes))


	if err := yaml.Unmarshal(yamlBytes, &c); err != nil {
		kss.ksl.Errorf("Failed to unmarshal YAML %#v to object: %v", string(yamlBytes), err)
	}

	// 🔴 最终诊断 2: 打印 Unmarshal 后的结果
    if c.CellTypes != nil {
        kss.ksl.Errorf("[DEBUG RAW] Unmarshal Result: CellTypes loaded. Keys: %+v", reflect.ValueOf(c.CellTypes).MapKeys())
    }

	if c.CellTypes == nil || c.Cells == nil {
		kss.ksl.Warn("The cellTypes and cells in xpushare config file is nil")
	}

	kss.checkPhysicalCells(&c)
	return &c
}
	*/

func (kss *XPUShareScheduler) initRawConfig() *Config {
	var c Config
	configPath := kss.args.XPUShareConfig
	
	// 🔴 最终诊断 1: 读取文件并检查错误
	yamlBytes, err := os.ReadFile(configPath)
	if err != nil {
		// 确保将读取错误打印出来
		kss.ksl.Errorf("[FATAL CONFIG] Failed to read config file %v: %v", configPath, err)
        // 🚨 关键：检查这里是否因为读取失败而直接退出
        // 您的原始代码没有 os.Exit，但如果有其他逻辑导致程序退出，可能会中断日志流
	} else {
        
    }

	if err := yaml.Unmarshal(yamlBytes, &c); err != nil {
		kss.ksl.Errorf("[FATAL UNMARSHAL] Failed to unmarshal YAML %#v to object: %v", string(yamlBytes), err)
        // 如果 Unmarshal 失败，打印原始内容以检查格式
	} else {
        
    }

	if c.CellTypes == nil || c.Cells == nil {
		kss.ksl.Warn("The cellTypes and cells in xpushare config file is nil")
	}

    // 🔴 确保在 checkPhysicalCells 之前，所有键都已清理 (假设您已应用 TrimSpace 修复)
	kss.checkPhysicalCells(&c) 
	return &c
}

func (kss *XPUShareScheduler) checkPhysicalCells(c *Config) {
	// check if cell type if physical cells map into celltypes
	cellTypes := c.CellTypes
	cells := c.Cells

	for idx, cell := range cells {
		cts, ok := cellTypes[cell.CellType]
		if !ok {
			kss.ksl.Errorf("Cells contains unknown cellType: %v", cell.CellType)
		}
		if cts.ChildCellPriority > 100 || cts.ChildCellPriority < 0 {
			kss.ksl.Errorf("Cell Priority must be in  0~1000")
		}
		inferCellSpec(&cells[idx], cellTypes, cell.CellType, idx+1)
	}
}

// infer the child's configuration from the parent's configuration
func inferCellSpec(spec *CellSpec, cellTypes map[string]CellTypeSpec, cellType string, defaultID int) {
	idQueue := queue.NewQueue()
	q := queue.NewQueue()
	q.Enqueue(spec)
	first := true

	for q.Len() > 0 {
		n := q.Len()
		for i := 1; i <= n; i++ {
			current := q.Dequeue().(*CellSpec)
			if first {
				if current.CellID == "" {
					current.CellID = current.CellID + strconv.Itoa(defaultID)
				}
				first = false
			} else {
				previousID := idQueue.Dequeue().(string)
				if current.CellID == "" {
					current.CellID = previousID + "/" + strconv.Itoa(int(i))
				} else {
					current.CellID = previousID + "/" + current.CellID
				}
			}

			// if we can't find the cellType in celltypes,
			// it means current celltpe is a leaf cell type.
			ct, ok := cellTypes[current.CellType]
			if !ok {
				continue
			}
			if ct.ChildCellNumber > 0 && len(current.CellChildren) == 0 {
				current.CellChildren = make([]CellSpec, ct.ChildCellNumber)
			}
			for c := range current.CellChildren {
				if current.CellChildren[c].CellType == "" {
					current.CellChildren[c].CellType = ct.ChildCellType
				}

				idQueue.Enqueue(current.CellID)
				q.Enqueue(&current.CellChildren[c])
			}
		}
	}
}

func (kss *XPUShareScheduler) watchConfig(c *Config) {
	configPath := kss.args.XPUShareConfig
	v := viper.New()
	v.SetConfigFile(configPath)
	v.WatchConfig()
	kss.ksl.Info("Watching config file: ", configPath)

	v.OnConfigChange(func(e fsnotify.Event) {
		kss.ksl.Warnf("Watched config file is changed: %s", e.Name)
		if ok := reflect.DeepEqual(*c, kss.initRawConfig()); !ok {
			kss.ksl.Error("Config file content is changed, exiting ...")
			os.Exit(0)
		}
	})
}
