package aggregator

import (
	"context"
	"fmt"

	v1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

const (
	// scheduler name
	schedulerName = "xpushare-scheduler"
)

var (
	domain = "sharedgpu/"

	// the name of a pod group that defines a coscheduling pod group.
	PodGroupName = domain + "group_name"
	// the minimum number of pods to be scheduled together in a pod group.
	PodGroupMinAvailable = domain + "min_available"

	// the upper limit percentage of time over the past sample period that one or more kernels of the pod are executed on the GPU
	XPUShareResourceGPULimit = domain + "gpu_limit"
	// the minimum request percentage of time over the past sample period that one or more kernels of the pod are executed on the GPU
	XPUShareResourceGPURequest = domain + "gpu_request"
	// the gpu memory request (in Byte)
	XPUShareResourceGPUMemory = domain + "gpu_mem"

	// the binding cell id
	XPUShareResourceCellID = domain + "cell_id"
	// the binding gpu uuid (or uuid list for multi-gpu)
	XPUShareResourceGPUUUID = domain + "gpu_uuid"
	// the pod-manager port used by the hook library
	XPUShareResourcePodManagerPort = domain + "gpu_manager_port"
)

type PodInfo struct {
	namespace    string
	name         string
	podId        string
	nodeName     string
	groupName    string
	minAvailable string
	limit        string
	request      string
	memory       string
	cellID       string
	uuid         string
	port         string
}

func (a *Aggregator) getPods() []*PodInfo {
	runningPods, err := a.clientset.CoreV1().Pods("").List(context.TODO(), metav1.ListOptions{FieldSelector: "spec.schedulerName=" + schedulerName + ",status.phase=Running"})
	if err != nil {
		a.ksl.Fatalf("Error when listing the pod information: %s", err.Error())
	}

	pods := runningPods.Items

	// n := len(pods)
	//podInfos := make([]*PodInfo, n)

	podInfos := []*PodInfo{}
	for _, pod := range pods {
		p := processPod(&pod)

		if p != nil {
			// podInfos[i] = p
			podInfos = append(podInfos, p)
		}
	}

	return podInfos
}

func processPod(pod *v1.Pod) *PodInfo {

	limit, ok := pod.Labels[XPUShareResourceGPULimit]

	if !ok {
		limit = "0.0"
		return nil
	}

	namespace := pod.ObjectMeta.Namespace
	name := pod.ObjectMeta.Name
	key := fmt.Sprintf("%v/%v", namespace, name)

	groupName, ok := pod.Labels[PodGroupName]
	if !ok {
		groupName = key
	}

	minAvailable, ok := pod.Labels[PodGroupMinAvailable]
	if !ok {
		minAvailable = "1"
	}

	request, ok := pod.Labels[XPUShareResourceGPURequest]
	if !ok {
		request = "0.0"
	}

	memory, ok := pod.Labels[XPUShareResourceGPUMemory]
	if !ok {
		memory, ok = pod.Annotations[XPUShareResourceGPUMemory]
		if !ok {
			memory = "0"
		}
	}

	uuid, port := getGPUUUIDPort(pod)

	cellID := pod.Annotations[XPUShareResourceCellID]

	return &PodInfo{
		namespace:    pod.ObjectMeta.Namespace,
		name:         pod.ObjectMeta.Name,
		podId:        string(pod.ObjectMeta.UID),
		nodeName:     pod.Spec.NodeName,
		groupName:    groupName,
		minAvailable: minAvailable,
		limit:        limit,
		request:      request,
		memory:       memory,
		cellID:       cellID,
		uuid:         uuid,
		port:         port,
	}
}

func getGPUUUIDPort(pod *v1.Pod) (string, string) {
	if pod.Annotations != nil {
		if uuid, ok := pod.Annotations[XPUShareResourceGPUUUID]; ok && uuid != "" {
			port := "0"
			if p, ok := pod.Annotations[XPUShareResourcePodManagerPort]; ok && p != "" {
				port = p
			}
			return uuid, port
		}
	}

	// Fallback: older manifests may only inject env vars.
	uuid := ""
	port := "0"
	containers := len(pod.Spec.Containers)
	findUUID := false
	findPort := false

	for i := 0; i < containers; i++ {
		n := len(pod.Spec.Containers[i].Env)
		for j := 0; j < n; j++ {
			switch pod.Spec.Containers[i].Env[j].Name {
			case "IX_VISIBLE_DEVICES", "NVIDIA_VISIBLE_DEVICES", "CUDA_VISIBLE_DEVICES", "MACA_VISIBLE_DEVICES", "MUSA_VISIBLE_DEVICES", "MTHREADS_VISIBLE_DEVICES", "VISIBLE_DEVICES":
				uuid = pod.Spec.Containers[i].Env[j].Value
				findUUID = true
			case "POD_MANAGER_PORT":
				port = pod.Spec.Containers[i].Env[j].Value
				findPort = true
			}
		}
		if findUUID && findPort {
			break
		}
	}
	return uuid, port
}
