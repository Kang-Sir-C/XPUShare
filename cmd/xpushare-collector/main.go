package main

import (
	"flag"
	"net/http"

	// prometheus

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"

	// xpushare
	"xpushare/pkg/collector"
	"xpushare/pkg/gpu"
	"xpushare/pkg/logger"
)

var (
	// the parameter from command line
	// web
	listenPort  = flag.String("web.listen-port", "9004", "An port to listen on for web interface and telemetry.")
	metricsPath = flag.String("web.telemetry-path", "/xpushare-collector", "A path which to expose metrics.")

	// logger
	level = flag.Int64("level", 2, "The level order of log.")
)

const (
	// the file storing the log of xpushare collector
	logPath = "xpushare-collector.log"
)

func main() {
	flag.Parse()

	ksl := logger.New(*level, logPath)

	cfg := gpu.LoadConfigFromEnv()
	provider, err := gpu.NewProvider(cfg)
	if err != nil {
		ksl.Fatalf("Failed to create GPU provider: %v", err)
	}
	ksl.Infof("GPU provider: %s (XPU_PROVIDER=%s)", provider.Name(), cfg.Provider)
	if err := provider.Init(); err != nil {
		ksl.Warnf("Failed to initialize GPU provider %q: %v", provider.Name(), err)
		ksl.Warnf("If this is a GPU node, ensure vendor SDK/CLI is available inside this container.")
		ksl.Warnf("For Iluvatar, ensure 'libixml.so' is in the library path (e.g., /usr/lib, /usr/local/lib).")
		ksl.Warnf("For NVIDIA, ensure 'nvidia-smi' is available and driver libs are mounted.")
		ksl.Warnf("For MooreThreads, ensure 'mthreads-gmi' is available in PATH.")
		ksl.Warnf("For MetaX, ensure 'mx-smi' is available in PATH.")
		ksl.Warnf("For BirenTech, ensure 'brsmi' is available in PATH.")
		select {}
	}
	defer func() {
		if err := provider.Shutdown(); err != nil {
			ksl.Errorf("GPU provider shutdown error: %v", err)
		}
	}()

	/* prometheus exporter */
	collector := collector.NewCollector(ksl, provider)

	registry := prometheus.NewRegistry()
	registry.MustRegister(collector)
	// expose the registered metrics via HTTP
	http.Handle(*metricsPath, promhttp.HandlerFor(registry, promhttp.HandlerOpts{}))
	// Compatibility: many Prometheus setups default to scraping "/metrics".
	if *metricsPath != "/metrics" {
		http.Handle("/metrics", promhttp.HandlerFor(registry, promhttp.HandlerOpts{}))
	}

	ksl.Infof("Starting Server at http://localhost:%s%s", *listenPort, *metricsPath)
	ksl.Fatal(http.ListenAndServe(":"+*listenPort, nil))
}
