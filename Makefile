CONTAINER_PREFIX?=
CONTAINER_NAME?=xpushare-collector
CONTAINER_VERSION?=latest
NERDCTL_NS?=k8s.io
NERDCTL=sudo nerdctl --namespace $(NERDCTL_NS)
ifneq ($(CONTAINER_PREFIX),)
CONTAINER_IMAGE=$(CONTAINER_PREFIX)/$(CONTAINER_NAME):$(CONTAINER_VERSION)
else
CONTAINER_IMAGE=$(CONTAINER_NAME):$(CONTAINER_VERSION)
endif


TARGET=xpushare-scheduler xpushare-collector xpushare-aggregator xpushare-config xpushare-query-ip
GO=go
GO_MODULE=GO111MODULE=on
GO_BUILD_FLAGS=-buildvcs=false
BIN_DIR=bin/
ALPINE_COMPILE_FLAGS=CGO_ENABLED=1 GOOS=linux GOARCH=arm64
NVML_COMPILE_FLAGS=CGO_LDFLAGS_ALLOW='-Wl,--unresolved-symbols=ignore-in-object-files' GOOS=linux GOARCH=arm64
PACKAGE_PREFIX=xpushare/cmd/

.PHONY: all clean $(TARGET)

all: $(TARGET)

xpushare-collector:
	$(GO_MODULE) $(NVML_COMPILE_FLAGS) $(GO) build $(GO_BUILD_FLAGS) -o $(BIN_DIR)$@ $(PACKAGE_PREFIX)$@

xpushare-scheduler:
	$(GO_MODULE) $(ALPINE_COMPILE_FLAGS) $(GO) build $(GO_BUILD_FLAGS) -o $(BIN_DIR)$@ $(PACKAGE_PREFIX)$@

xpushare-aggregator:
	$(GO_MODULE) $(ALPINE_COMPILE_FLAGS) $(GO) build $(GO_BUILD_FLAGS) -o $(BIN_DIR)$@ $(PACKAGE_PREFIX)$@
	
xpushare-config:
	$(GO_MODULE) $(ALPINE_COMPILE_FLAGS) $(GO) build $(GO_BUILD_FLAGS) -o $(BIN_DIR)$@ $(PACKAGE_PREFIX)$@

xpushare-query-ip:
	$(GO_MODULE) $(ALPINE_COMPILE_FLAGS) $(GO) build $(GO_BUILD_FLAGS) -o $(BIN_DIR)$@ $(PACKAGE_PREFIX)$@

xhook-scheduler:
	$(NERDCTL) build -t $(CONTAINER_IMAGE) -f ./docker/xhook-scheduler/Dockerfile . --no-cache

xhook-init:
	$(NERDCTL) build -t $(CONTAINER_IMAGE) -f ./docker/xhook-init/Dockerfile . --no-cache

build-image:
	$(NERDCTL) build -t $(CONTAINER_IMAGE) -f ./docker/$(CONTAINER_NAME)/Dockerfile . --no-cache

push-image:
	$(NERDCTL) push $(CONTAINER_IMAGE)

deploy-component:
	kubectl apply -f build/

delete-component:
	kubectl delete -f build/

save-image:
	$(NERDCTL) save -o $(CONTAINER_NAME)_$(CONTAINER_VERSION).tar $(CONTAINER_IMAGE)

load-image:
	$(NERDCTL) load -i $(CONTAINER_NAME)_$(CONTAINER_VERSION).tar

list-images:
	$(NERDCTL) images | grep -E 'xpushare|xhook'

sync:
	git submodule update --remote

clean:
	rm $(BIN_DIR)* 2>/dev/null; exit 0
