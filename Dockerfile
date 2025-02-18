FROM selenium/standalone-chrome:latest
USER root

# 设置时区和语言环境
ENV TZ=Asia/Shanghai
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone
ENV LANG=C.UTF-8

# 安装基本工具和Chrome
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
 wget \
 unzip\
 software-properties-common \
 && rm -rf /var/lib/apt/lists/*


# 强制系统级安装Python包
COPY requirements.txt .
RUN pip3 install --no-cache-dir --break-system-packages -r requirements.txt

# 安装 Drogon
WORKDIR /usr/src
RUN git clone https://github.com/drogonframework/drogon
WORKDIR /usr/src/drogon
RUN git submodule update --init
RUN mkdir build
WORKDIR /usr/src/drogon/build
RUN cmake ..
RUN make -j $(nproc)
RUN make install
RUN ldconfig

# 设置工作目录
WORKDIR /usr/src/app

# 复制项目文件
COPY . .

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
cd /usr/src/app/tools/accountlogin && python3 loginremote.py &\n\
cd /usr/src/app/build && exec "$@"' > /usr/src/app/docker-entrypoint.sh

RUN chmod +x /usr/src/app/docker-entrypoint.sh

# 创建构建目录
RUN mkdir -p build
WORKDIR /usr/src/app/build

# 构建项目
RUN cmake ..
RUN make -j $(nproc)

# 设置环境变量
ENV SELENIUM_PORT=4444

# 暴露端口
EXPOSE 5555 5556

# 设置入口点
ENTRYPOINT ["/usr/src/app/docker-entrypoint.sh"]

# 运行应用
CMD ["./aiapi"]
