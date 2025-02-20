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
# 强制系统级安装Python包
COPY requirements.txt .
RUN pip3 install --no-cache-dir --break-system-packages -r requirements.txt

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
COPY . .

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
cd /usr/src/app/tools/accountlogin &&sudo python3 loginlocal.py &\n\
cd /usr/src/app/build && exec "$@"' > /usr/src/app/docker-entrypoint.sh && \
    chmod +x /usr/src/app/docker-entrypoint.sh

# 暴露端口
EXPOSE 5555

# 设置入口点和命令
ENTRYPOINT ["/usr/src/app/docker-entrypoint.sh"]
CMD ["./aiapi"]
