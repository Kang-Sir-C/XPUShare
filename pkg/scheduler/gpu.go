package scheduler

import (
	"context"
	"strconv"
	"time"

	"github.com/prometheus/common/model"
)

const (
	// query metrics
	GPU_CAPACITY    = "gpu_capacity"
	GPU_REQUIREMENT = "gpu_requirement"
)

type GPU struct {
	uuid   string
	memory int64
	index  int
}

func (kss *XPUShareScheduler) queryGPUCapicity(nodeName string) []model.LabelSet {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	result, warnings, err := kss.promeAPI.Series(ctx, []string{
		"{__name__=~\"" + GPU_CAPACITY + "\",node=\"" + nodeName + "\"}",
	}, time.Now().Add(-time.Minute*2), time.Now())

	if err != nil {
		kss.ksl.Warnf("Error querying Prometheus: %v\n", err)
		return nil
	}
	if len(warnings) > 0 {
		kss.ksl.Warnf("Warnings: %v\n", warnings)
	}
	return result
}

func (kss *XPUShareScheduler) getGPUByNode(nodeName string) {
	results := kss.queryGPUCapicity(nodeName)

	gpuInfos := map[string][]GPU{}
	for _, res := range results {
		uuid := string(res["uuid"])
		model := string(res["model"])
		memory, _ := strconv.ParseInt(string(res["memory"]), 10, 64)
		index, _ := strconv.Atoi(string(res["index"]))
		gpuInfos[model] = append(gpuInfos[model], GPU{
			uuid:   uuid,
			memory: memory,
			index:  index,
		})
	}
	kss.gpuInfos[nodeName] = gpuInfos

	// 🔴 DEBUG DUMP 3: 打印 GPU 发现结果
	totalGPUs := 0
    for _, list := range gpuInfos {
        totalGPUs += len(list)
    }

    kss.ksl.Errorf("[DEBUG GPU] [PROME] Node: %s, Found Models: %d, Total GPUs: %d. Data: %+v", 
        nodeName, len(gpuInfos), totalGPUs, gpuInfos)

}
