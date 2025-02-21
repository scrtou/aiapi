FROM ubuntu:22.04

# 设置时区和语言环境
ENV TZ=Asia/Shanghai \
    LANG=C.UTF-8 \
    SELENIUM_PORT=4444

# 设置时区
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

# 安装依赖，合并 RUN 命令减少层级
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

# 添加谷歌浏览器官方源
RUN wget -q -O - https://dl-ssl.google.com/linux/linux_signing_key.pub | apt-key add -
RUN sh -c 'echo "deb [arch=amd64] http://dl.google.com/linux/chrome/deb/ stable main" >> /etc/apt/sources.list.d/google-chrome.list'

# 安装谷歌浏览器
RUN apt-get update && apt-get install -y google-chrome-stable

# 安装 ChromeDriver
# 安装 ChromeDriver（最终稳定版）
RUN set -euxo pipefail \
    && CHROME_FULL_VERSION=$(google-chrome --version | awk '{print $3}') \
    && CHROMEDRIVER_VERSION=$(curl -fsS "https://chromedriver.storage.googleapis.com/LATEST_RELEASE_$CHROME_FULL_VERSION") \
    && echo "正在安装 ChromeDriver 版本: $CHROMEDRIVER_VERSION" \
    && wget -nv --tries=3 "https://chromedriver.storage.googleapis.com/$CHROMEDRIVER_VERSION/chromedriver_linux64.zip" \
    && unzip -t chromedriver_linux64.zip \
    && unzip chromedriver_linux64.zip \
    && mv chromedriver /usr/local/bin/ \
    && chmod +x /usr/local/bin/chromedriver \
    && rm chromedriver_linux64.zip \
    && chromedriver --version

# 以 root 用户安装 Python 包
COPY requirements.txt .
RUN pip3 install --no-cache-dir --break-system-packages -r requirements.txt

# 安装 Drogon（以 root 用户安装）
WORKDIR /usr/src
RUN git clone https://github.com/drogonframework/drogon && \
    cd drogon && \
    git submodule update --init && \
    mkdir build && \
    cd build && \
    cmake.. -DBUILD_POSTGRESQL=ON -DBUILD_MYSQL=ON && \
    make -j $(nproc) && \
    make install && \
    ldconfig && \
    cd / && \
    rm -rf /usr/src/drogon

# 设置工作目录和复制项目文件
WORKDIR /usr/src/app/
COPY . .

# 创建必要的目录并设置权限
RUN mkdir -p /usr/src/app/uploads/tmp && \
    mkdir -p /usr/src/app/build/uploads/tmp && \
    chmod -R 777 /usr/src/app/uploads && \
    chmod -R 777 /usr/src/app/build/uploads && \


# 构建项目
WORKDIR /usr/src/app/build
RUN cmake.. && make -j $(nproc)

# 创建并设置启动脚本
RUN echo '#!/bin/bash\n\
if [! -z "$CONFIG_JSON" ]; then\n\
    echo "$CONFIG_JSON" > /usr/src/app/config.json\n\
fi\n\
\n\
if [! -z "$CUSTOM_CONFIG" ]; then\n\
    echo "$CUSTOM_CONFIG" | jq -s ".[0] * $(<config.json)" > /usr/src/app/config.json\n\
fi\n\
\n\
# 确保目录存在并有正确的权限\n\
mkdir -p /usr/src/app/uploads/tmp\n\
mkdir -p /usr/src/app/build/uploads/tmp\n\
chmod -R 777 /usr/src/app/uploads\n\
chmod -R 777 /usr/src/app/build/uploads\n\
\n\
# 清理并创建新的Chrome用户数据目录\n\
rm -rf /home/seluser/chrome-data/*\n\
mkdir -p /home/seluser/chrome-data\n\
\n\
# 启动ChromeDriver（添加这一行确保驱动在容器启动时运行）\n\
chromedriver --port=9515 --whitelisted-ips=\n\
\n\
cd /usr/src/app/tools/accountlogin && \
CHROME_USER_DATA_DIR=/home/seluser/chrome-data python3 loginlocal.py &\n\
cd /usr/src/app/build && exec "$@"' > /usr/src/app/docker-entrypoint.sh && \
    chmod +x /usr/src/app/docker-entrypoint.sh

# 暴露端口
EXPOSE 5555

# 设置入口点和命令
ENTRYPOINT ["/usr/src/app/docker-entrypoint.sh"]
CMD ["./aiapi"]