package config

import (
	"fmt"
	"io/ioutil"
	"os"
	"strconv"
	"strings"

	"github.com/prometheus/common/model"
	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/labels"
)

const (
	// query metrics
	GPU_REQUIREMENT = "gpu_requirement"
)

// queryDecision returns the "current" gpu_requirement labelsets.
//
// NOTE: do NOT use Prometheus Series(range) here.
// When a pod is recreated quickly with different labels (e.g. gpu_request/gpu_limit),
// Series(range) can return both old & new series and the config file order becomes nondeterministic.
// That will break E4 fairness (expected 1:2 -> observed ~1:1).
//
// We derive config from the local informer cache (Kubernetes source of truth),
// which is immediate and does not suffer scrape lag / stale series issues.
func (c *Config) queryDecision() []model.LabelSet {
	pods, err := c.podLister.List(labels.Everything())
	if err != nil {
		c.ksl.Warnf("Error listing pods from cache: %v", err)
		return nil
	}

	result := make([]model.LabelSet, 0, len(pods))
	for _, pod := range pods {
		if pod == nil {
			continue
		}
		if pod.Spec.NodeName != nodeName {
			continue
		}
		if pod.Spec.SchedulerName != schedulerName {
			continue
		}
		if pod.Status.Phase != corev1.PodRunning {
			continue
		}

		limit, ok := pod.Labels[XPUShareResourceGPULimit]
		if !ok || limit == "" {
			continue
		}
		request := pod.Labels[XPUShareResourceGPURequest]
		if request == "" {
			request = "0.0"
		}

		memory := pod.Labels[XPUShareResourceGPUMemory]
		if memory == "" && pod.Annotations != nil {
			memory = pod.Annotations[XPUShareResourceGPUMemory]
		}
		if memory == "" {
			memory = "0"
		}

		if pod.Annotations == nil {
			continue
		}
		uuid := pod.Annotations[XPUShareResourceGPUUUID]
		port := pod.Annotations[XPUShareResourcePodManagerPort]
		if uuid == "" || port == "" {
			continue
		}

		result = append(result, model.LabelSet{
			model.LabelName("uuid"):               model.LabelValue(uuid),
			model.LabelName("exported_namespace"): model.LabelValue(pod.Namespace),
			model.LabelName("exported_pod"):       model.LabelValue(pod.Name),
			model.LabelName("request"):            model.LabelValue(request),
			model.LabelName("limit"):              model.LabelValue(limit),
			model.LabelName("memory"):             model.LabelValue(memory),
			model.LabelName("port"):               model.LabelValue(port),
		})
	}

	c.ksl.Infof("[DEBUG] Pod cache query returned %d results (node=%s)", len(result), nodeName)
	return result
}

// gpuConfig
// -> key: uuid ; value: all pod request
// podMangerPortConfig
// ->  key: uuid ; value: all pod manager port
func (c *Config) convertData(result []model.LabelSet) (map[string][]string, map[string][]string) {

	gpuConfig, podManagerPortConfig := map[string][]string{}, map[string][]string{}
	for _, res := range result {
		uuid := strings.ReplaceAll(string(res["uuid"]), ",", "")

		namespace := res["exported_namespace"]
		name := res["exported_pod"]

		request, err := strconv.ParseFloat(string(res["request"]), 64)
		if err != nil || request > 1.0 {
			continue
		}
		gpuData := fmt.Sprintf("%v/%v %v %v %v\n", namespace, name, res["request"], res["limit"], res["memory"])
		portData := fmt.Sprintf("%v/%v %v\n", namespace, name, res["port"])

		gpuConfig[uuid] = append(gpuConfig[uuid], gpuData)
		podManagerPortConfig[uuid] = append(podManagerPortConfig[uuid], portData)

	}

	return gpuConfig, podManagerPortConfig
}

// gpuConfigFile is named by UUID of GPU
// first line means that there are n pod sharing this GPU
// following n lines means that the gpu request of the pods
func (c *Config) writeFile(gpuConfig, podManagerPortConfig map[string][]string) {

	for uuid, gpuRequest := range gpuConfig {
		gpuConfigFile, err := os.Create(schedulerGPUConfigPath + uuid)
		if err != nil {
			c.ksl.Errorf("Error when create config file on path: %s, %v", schedulerGPUConfigPath+uuid, err)
		}

		gpuConfigFile.WriteString(fmt.Sprintf("%d\n", len(gpuRequest)))
		for _, req := range gpuRequest {
			gpuConfigFile.WriteString(req)
		}
		gpuConfigFile.Sync()
		gpuConfigFile.Close()
	}

	for uuid, managerPort := range podManagerPortConfig {
		podmanagerPortFile, err := os.Create(schedulerGPUPodManagerPortPath + uuid)
		if err != nil {
			c.ksl.Errorf("Error when create config file on path: %s, %v", schedulerGPUPodManagerPortPath+uuid, err)
		}

		podmanagerPortFile.WriteString(fmt.Sprintf("%d\n", len(managerPort)))
		for _, port := range managerPort {
			podmanagerPortFile.WriteString(port)
		}
		podmanagerPortFile.Sync()
		podmanagerPortFile.Close()
	}

	// TODO: gpuConfig & podManagerConfig == nil
	if len(gpuConfig) == 0 || len(podManagerPortConfig) == 0 {
		c.ksl.Debug("Currently, no pod need gpu, set the file to 0")
		c.cleanFile()
	}
}

func (c *Config) readFileName() []os.FileInfo {
	fileList, err := ioutil.ReadDir(schedulerGPUConfigPath)
	if err != nil {
		c.ksl.Fatalf("Error when read the config file: %v", err)
	}
	return fileList
}

func (c *Config) cleanFile() {
	fileList := c.readFileName()

	for _, uuid := range fileList {
		gpuConfigFile, err := os.Create(schedulerGPUConfigPath + uuid.Name())
		if err != nil {
			c.ksl.Errorf("Error when create config file on path: %s, %v", schedulerGPUConfigPath+uuid.Name(), err)
		}

		gpuConfigFile.WriteString("0\n")
		gpuConfigFile.Sync()
		gpuConfigFile.Close()
	}
	for _, uuid := range fileList {
		podmanagerPortFile, err := os.Create(schedulerGPUPodManagerPortPath + uuid.Name())
		if err != nil {
			c.ksl.Errorf("Error when create config file on path: %s, %v", schedulerGPUPodManagerPortPath+uuid.Name(), err)
		}

		podmanagerPortFile.WriteString("0\n")
		podmanagerPortFile.Sync()
		podmanagerPortFile.Close()
	}
}
