FROM selenium/standalone-chrome:latest
USER root

# 设置时区和语言环境
ENV TZ=Asia/Shanghai \
    LANG=C.UTF-8 \
    SELENIUM_PORT=4444

# 设置时区
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

# 安装依赖，合并RUN命令减少层级
RUN apt-get update && apt-get install -y \
    curl \
    git \
    gcc \
    g++ \
    cmake \
    make \
    openssl \
    libssl-dev \
    libjsoncpp-dev \
    xvfb \
    uuid-dev \
    zlib1g-dev \
    wget \
    jq \
    python3-full \
    python3-pip \
    python3-venv \
    unzip \
    postgresql \
    postgresql-server-dev-all \
    libpq-dev \
    libmariadb-dev \
    software-properties-common \
    && rm -rf /var/lib/apt/lists/* 

# 以root用户安装Python包
COPY requirements.txt .
RUN pip3 install --no-cache-dir --break-system-packages -r requirements.txt

# 创建并设置必要的目录
RUN mkdir -p /home/seluser/.wdm/drivers && \
    mkdir -p /home/seluser/.local/lib && \
    chown -R seluser:seluser /home/seluser/.wdm && \
    chown -R seluser:seluser /home/seluser/.local && \
    chmod -R 755 /home/seluser/.wdm && \
    chmod -R 755 /home/seluser/.local

# 切换到seluser
USER seluser

# 安装Drogon（优化构建步骤）
WORKDIR /usr/src
RUN git clone https://github.com/drogonframework/drogon && \
    cd drogon && \
    git submodule update --init && \
    mkdir build && \
    cd build && \
    cmake .. -DBUILD_POSTGRESQL=ON -DBUILD_MYSQL=ON && \
    make -j $(nproc) && \
    make install && \
    ldconfig && \
    cd / && \
    rm -rf /usr/src/drogon

# 设置工作目录和复制项目文件
WORKDIR /usr/src/app/
COPY --chown=seluser:seluser . .

# 创建必要的目录并设置权限
RUN mkdir -p build/uploads/tmp uploads/tmp && \
    chmod -R 777 build/uploads/tmp uploads/tmp

# 构建项目
WORKDIR /usr/src/app/build
RUN cmake .. && make -j $(nproc)

# 创建并设置启动脚本
RUN echo '#!/bin/bash\n\
if [ ! -z "$CONFIG_JSON" ]; then\n\
    echo "$CONFIG_JSON" > /usr/src/app/config.json\n\
fi\n\
\n\
if [ ! -z "$CUSTOM_CONFIG" ]; then\n\
    echo "$CUSTOM_CONFIG" | jq -s ".[0] * $(<config.json)" > /usr/src/app/config.json\n\
fi\n\
\n\
# 清理可能存在的Chrome用户数据目录\n\
rm -rf /tmp/.com.google.Chrome* /tmp/.org.chromium.Chromium* /tmp/chrome-* \n\
\n\
cd /usr/src/app/tools/accountlogin && \
CHROME_USER_DATA_DIR=/tmp/chrome-data-$(date +%s) python3 loginlocal.py &\n\
cd /usr/src/app/build && exec "$@"' > /usr/src/app/docker-entrypoint.sh && \
    chmod +x /usr/src/app/docker-entrypoint.sh

# 暴露端口
EXPOSE 5555

# 设置入口点和命令
ENTRYPOINT ["/usr/src/app/docker-entrypoint.sh"]
CMD ["./aiapi"]
