FROM ubuntu:22.04

# 设置时区和语言环境
ENV TZ=Asia/Shanghai \
    LANG=C.UTF-8 \
    SELENIUM_PORT=4444

# 设置时区
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone
USER root

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
# 安装 Google Chrome
RUN wget -q -O - https://dl-ssl.google.com/linux/linux_signing_key.pub | apt-key add - \
    && sh -c 'echo "deb [arch=amd64] http://dl.google.com/linux/chrome/deb/ stable main" >> /etc/apt/sources.list.d/google-chrome.list' \
    && apt-get update && apt-get install -y google-chrome-stable

# 安装 ChromeDriver（适配 Chrome for Testing 存储库）
RUN set -eux \
    && CHROME_FULL_VERSION=$(google-chrome --version | awk '{print $3}') \
    && echo "Chrome 版本: $CHROME_FULL_VERSION" \
    && CHROMEDRIVER_URL="https://storage.googleapis.com/chrome-for-testing-public/$CHROME_FULL_VERSION/linux64/chromedriver-linux64.zip" \
    && echo "ChromeDriver 下载链接: $CHROMEDRIVER_URL" \
    && wget -nv --tries=3 "$CHROMEDRIVER_URL" \
    && unzip -t chromedriver-linux64.zip \
    && unzip chromedriver-linux64.zip \
    && mv chromedriver-linux64/chromedriver /usr/local/bin/ \
    && chmod +x /usr/local/bin/chromedriver \
    && rm -rf chromedriver-linux64.zip chromedriver-linux64 \
    && chromedriver --version

# 以 root 用户安装 Python 包
COPY requirements.txt .
RUN pip3 install --no-cache-dir -r requirements.txt

# 安装 Drogon（以 root 用户安装）
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
RUN mkdir -p /tmp/chrome-data && \
    chmod -R 777 /tmp/chrome-data && \
    mkdir -p /dev/shm && \
    chmod -R 777 /dev/shm

# 设置工作目录和复制项目文件
WORKDIR /usr/src/app/
COPY . .

# 创建必要的目录并设置权限
RUN mkdir -p /usr/src/app/uploads/tmp && \
    mkdir -p /usr/src/app/build/uploads/tmp && \
    chmod -R 777 /usr/src/app/uploads && \
    chmod -R 777 /usr/src/app/build/uploads


# 构建项目
WORKDIR /usr/src/app/build
RUN cmake .. && make -j $(nproc)

# 创建启动脚本
RUN echo '#!/bin/bash\n\
if [ ! -z "$CONFIG_JSON" ]; then\n\
    echo "$CONFIG_JSON" > /usr/src/app/build/config.json\n\
fi\n\
\n\
if [ ! -z "$CUSTOM_CONFIG" ]; then\n\
    echo "$CUSTOM_CONFIG" | jq -s ".[0] * $(<config.json)" > /usr/src/app/build/config.json\n\
fi\n\
\n\
cd /usr/src/app/tools/accountlogin && python3 loginlocal.py &\n\
cd /usr/src/app/build && exec "$@"' > /usr/src/app/docker-entrypoint.sh

RUN chmod +x /usr/src/app/docker-entrypoint.sh

# 暴露端口
EXPOSE 5555

# 设置入口点和命令
ENTRYPOINT ["/usr/src/app/docker-entrypoint.sh"]
CMD ["./aiapi"]