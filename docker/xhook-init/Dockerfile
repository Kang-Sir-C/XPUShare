# 1. 使用最基础的镜像
FROM swr.cn-north-4.myhuaweicloud.com/ddn-k8s/docker.io/library/alpine:latest-linuxarm64

# 2. 直接从宿主机 COPY 编译好的库文件
COPY xhook/lib/libxhook.so.1 /libxhook.so.1

# 3. 启动命令：复制到共享卷
CMD ["cp", "/libxhook.so.1", "/xpushare/library/libxhook.so.1"]
