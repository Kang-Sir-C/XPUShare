# 1. 使用天数官方镜像作为运行底座
# FROM corex:4.3.6
FROM docker.io/library/corex:4.3.6
# 2. 安装依赖
RUN pip3 install inotify

# 3. COPY 宿主机编译好的二进制文件
COPY xhook/bin/xhook-schd /xhook-schd
COPY xhook/bin/xhook-pmgr /xhook-pmgr

# 4. COPY 启动脚本
COPY docker/xhook-scheduler/launcher-multigpus.sh /launcher-multigpus.sh
COPY docker/xhook-scheduler/launcher.py /launcher.py

# 5. 赋予权限
RUN chmod +x /xhook-schd /xhook-pmgr /launcher-multigpus.sh

# 6. 环境变量
ENV IX_VISIBLE_DEVICES all

# 7. 启动命令
CMD ["/launcher-multigpus.sh", "/xpushare/scheduler/config", "/xpushare/scheduler/podmanagerport"]
