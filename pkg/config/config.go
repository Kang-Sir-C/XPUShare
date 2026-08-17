package config

import (
	"os"
	"strconv"

	"github.com/sirupsen/logrus"

	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/cache"

	corev1 "k8s.io/api/core/v1"
	corev1informer "k8s.io/client-go/informers/core/v1"
	corev1lister "k8s.io/client-go/listers/core/v1"

	promeV1 "github.com/prometheus/client_golang/api/prometheus/v1"
)

const (
	schedulerGPUConfigPath         = "/xpushare/scheduler/config/"
	schedulerGPUPodManagerPortPath = "/xpushare/scheduler/podmanagerport/"
)

var (
	domain = "sharedgpu/"

	// the upper limit percentage of time over the past sample period that one or more kernels of the pod are executed on the GPU
	XPUShareResourceGPULimit = domain + "gpu_limit"
	// the minimum request percentage of time over the past sample period that one or more kernels of the pod are executed on the GPU
	XPUShareResourceGPURequest = domain + "gpu_request"
	// the gpu memory request (in Byte)
	XPUShareResourceGPUMemory = domain + "gpu_mem"

	// the binding gpu uuid (or uuid list for multi-gpu)
	XPUShareResourceGPUUUID = domain + "gpu_uuid"
	// the pod-manager port used by the hook library
	XPUShareResourcePodManagerPort = domain + "gpu_manager_port"

	// scheduler name
	schedulerName = "xpushare-scheduler"

	nodeName string
)

type Config struct {
	ksl       *logrus.Logger
	promeAPI  promeV1.API
	clientset kubernetes.Interface
	podLister corev1lister.PodLister
}

func NewConfig(ksl *logrus.Logger, promeAPI promeV1.API, clientset kubernetes.Interface, podInformer corev1informer.PodInformer, stopCh <-chan struct{}) *Config {

	config := &Config{
		ksl:       ksl,
		promeAPI:  promeAPI,
		clientset: clientset,
		podLister: podInformer.Lister(),
	}

	// get the node name of this pod
	nodeName = os.Getenv("NODE_NAME")
	ksl.Printf("Node: %v", nodeName)

	// create the configuration directories
	os.MkdirAll(schedulerGPUConfigPath, os.ModePerm)
	os.MkdirAll(schedulerGPUPodManagerPortPath, os.ModePerm)

	pInformer := podInformer.Informer()
	pInformer.AddEventHandler(
		cache.FilteringResourceEventHandler{
			FilterFunc: config.filterPod,
			Handler: cache.ResourceEventHandlerFuncs{
				AddFunc: func(obj interface{}) {
					gpuConfig, podMangerPortConfig := config.convertData(config.queryDecision())
					config.writeFile(gpuConfig, podMangerPortConfig)
					pod := obj.(*corev1.Pod)
					ksl.Infof("add: %v/%v", pod.Namespace, pod.Name)
				},
				UpdateFunc: func(old, new interface{}) {
					gpuConfig, podMangerPortConfig := config.convertData(config.queryDecision())
					config.writeFile(gpuConfig, podMangerPortConfig)
					pod := new.(*corev1.Pod)
					ksl.Infof("update: %v/%v", pod.Namespace, pod.Name)
				},
				DeleteFunc: func(obj interface{}) {
					gpuConfig, podMangerPortConfig := config.convertData(config.queryDecision())
					config.writeFile(gpuConfig, podMangerPortConfig)
					switch t := obj.(type) {
					case *corev1.Pod:
						ksl.Infof("delete: %v/%v", t.Namespace, t.Name)
					case cache.DeletedFinalStateUnknown:
						if pod, ok := t.Obj.(*corev1.Pod); ok {
							ksl.Infof("delete: %v/%v", pod.Namespace, pod.Name)
						}
					default:
						ksl.Infof("delete: unknown object")
					}
				},
			},
		})

	go pInformer.Run(stopCh)

	if !cache.WaitForCacheSync(stopCh, pInformer.HasSynced) {
		ksl.Fatalf("Failed to WaitForCacheSync")
	}

	return config
}

func (c *Config) filterPod(obj interface{}) bool {
	switch t := obj.(type) {
	case *corev1.Pod:
		return c.checkSharedPod(obj.(*corev1.Pod))
	case cache.DeletedFinalStateUnknown:
		if pod, ok := t.Obj.(*corev1.Pod); ok {
			return c.checkSharedPod(pod)
		}
		return false
	default:
		return false
	}
}

func (c *Config) checkSharedPod(pod *corev1.Pod) bool {

	// only need to process the pod scheduled
	if pod.Spec.NodeName == "" {
		return false
	}
	if pod.Spec.NodeName != nodeName {
		return false
	}

	limit, ok := pod.Labels[XPUShareResourceGPULimit]

	if !ok {
		return false
	}

	floatLimit, err := strconv.ParseFloat(limit, 64)
	if err != nil {
		c.ksl.Errorf("limit converts error")
		return false
	}
	if floatLimit <= 1.0 {
		c.ksl.Infof("Get the gpu limit(<= 1.0) of pod: %v/%v, need to process", pod.Namespace, pod.Name)
		return true
	}

	return false
}
