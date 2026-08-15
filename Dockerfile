FROM ubuntu:22.04

# 设置时区和语言环境
ENV TZ=Asia/Shanghai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone
ENV LANG=C.UTF-8


# 安装基本工具和编译环境
RUN apt-get update && apt-get install -y \
    git \
    gcc \
    g++ \
    cmake \
    make \
    openssl \
    libssl-dev \
    libjsoncpp-dev \
    libpq-dev \
    postgresql-server-dev-all \
    libsqlite3-dev \
    sqlite3 \
    libmysqlclient-dev \
    default-libmysqlclient-dev \
    xvfb \
    uuid-dev \
    zlib1g-dev \
    wget \
    jq \
    python3-pip \
    docker-compose \
    libspdlog-dev \
    && rm -rf /var/lib/apt/lists/*

# 安装Python依赖
COPY requirements.txt .
RUN pip install -r requirements.txt

# 安装 Drogon
WORKDIR /usr/drogon
RUN git clone https://github.com/drogonframework/drogon .
RUN git submodule update --init
RUN mkdir build
WORKDIR /usr/drogon/build
RUN cmake ..
RUN make -j $(nproc)
RUN make install
RUN ldconfig

# 设置工作目录
WORKDIR /usr/aiapi

# 复制项目文件
COPY . .

# 创建启动脚本
RUN cat <<'EOF' > /usr/aiapi/docker-entrypoint.sh
#!/bin/bash
CONFIG_PATH="/usr/aiapi/src/config.json"
if [ ! -z "$CONFIG_JSON" ]; then
    echo "$CONFIG_JSON" > "$CONFIG_PATH"
fi
if [ ! -z "$CUSTOM_CONFIG" ]; then
    TMP_CONFIG=$(mktemp)
    jq -s ".[0] * .[1]" <(echo "$CUSTOM_CONFIG") "$CONFIG_PATH" > "$TMP_CONFIG" && mv "$TMP_CONFIG" "$CONFIG_PATH"
fi
cd /usr/aiapi/src/build && exec "$@"
EOF

RUN chmod +x /usr/aiapi/docker-entrypoint.sh

# 创建构建目录
RUN mkdir -p src/build
RUN mkdir -p src/build/logs
WORKDIR /usr/aiapi/src/build

# 构建项目
# Ubuntu 22.04 提供 MariaDB 兼容的 MySQL 客户端库；显式告诉 Drogon
# 头文件和库的位置，避免 FindMySQL.cmake 搜索失败。
RUN cmake .. \
    -DMYSQL_INCLUDE_DIRS=/usr/include/mariadb \
    -DMYSQL_LIBRARIES=/usr/lib/x86_64-linux-gnu/libmariadb.so
RUN make -j $(nproc)

# 复制默认配置文件（config.sqlite.example.json 已删除，改用 config.example.json）
# 运行时若通过 volumes 挂载 ./data/config.json 到 /usr/aiapi/src/config.json，
# 挂载会覆盖此默认文件；此处仅保证无挂载时容器仍有可用配置。
RUN cp /usr/aiapi/config.example.json /usr/aiapi/src/config.json

# 暴露端口
EXPOSE 5555 5556

# 设置入口点
ENTRYPOINT ["/usr/aiapi/docker-entrypoint.sh"]

# 运行应用
CMD ["./aiapi"]
