#!/bin/bash
# 启动全部服务。
# 服务按当前工作目录读取 config.ini，因此每个服务必须进入自己的目录启动。
# config.ini 由 config-templates 下的模板渲染：模板里的 __XXX__ 占位符
# 替换为同名环境变量（__PUBLIC_HOST__、__EMAIL_PASS__ 等）。
set -uo pipefail

export PUBLIC_HOST="${PUBLIC_HOST:-127.0.0.1}"
export GATE_PORT="${GATE_PORT:-8080}"
export EMAIL_HOST="${EMAIL_HOST:-smtp.qq.com}"
export EMAIL_PORT="${EMAIL_PORT:-465}"
export EMAIL_USER="${EMAIL_USER:-}"
export EMAIL_PASS="${EMAIL_PASS:-}"
export EMAIL_FROM="${EMAIL_FROM:-${EMAIL_USER}}"

# 证书两个文件都挂进来才启用 HTTPS；只挂了一个就当没配，免得网关直接起不来
CERT_DIR=/etc/yunjian/certs
if [ -f "${CERT_DIR}/fullchain.pem" ] && [ -f "${CERT_DIR}/privkey.pem" ]; then
    export TLS_CERT="${CERT_DIR}/fullchain.pem"
    export TLS_KEY="${CERT_DIR}/privkey.pem"
else
    export TLS_CERT=""
    export TLS_KEY=""
fi

BIN=/workspace/server/build/linux-docker/bin
TPL=/opt/yunjian/config-templates
LOG=/var/log/yunjian

render() {
    local tpl="$1" out="$2" var val key
    cp "$tpl" "$out"
    for var in PUBLIC_HOST GATE_PORT TLS_CERT TLS_KEY \
               EMAIL_HOST EMAIL_PORT EMAIL_USER EMAIL_PASS EMAIL_FROM; do
        key="__${var}__"
        val="${!var:-}"
        # 转义口令里的特殊字符，避免破坏 sed 的替换表达式
        val=${val//\\/\\\\}
        val=${val//&/\\&}
        val=${val//|/\\|}
        sed -i "s|${key}|${val}|g" "$out"
    done
}

wait_port() {
    for _ in $(seq 1 60); do
        if (exec 3<>/dev/tcp/127.0.0.1/"$1") 2>/dev/null; then
            exec 3<&- ; return 0
        fi
        sleep 1
    done
    return 1
}

if [ ! -d "${BIN}" ]; then
    echo "[start] 错误：未找到 ${BIN}，请先编译"
    exit 1
fi
if [ -z "${EMAIL_PASS}" ]; then
    echo "[start] 警告：未配置 EMAIL_PASS，注册时的验证码邮件会发送失败"
fi
if [ -n "${TLS_CERT}" ]; then
    echo "[start] 传输加密：HTTPS 网关(${GATE_PORT}) + 聊天(8090/8091) + 资源(9090)，证书 ${TLS_CERT}"
else
    echo "[start] 传输加密：未启用（没有 ${CERT_DIR}/fullchain.pem），网关与两条 TCP 通道都是明文"
fi

echo "[start] 等待 Redis (6380)"
wait_port 6380 || echo "[start] 警告：Redis 未就绪，服务可能连不上"
echo "[start] 等待 MySQL (3306)"
wait_port 3306 || echo "[start] 警告：MySQL 未就绪，服务可能连不上"

start_service() {
    local dir="$1" exe="$2" tpl="$3"
    if [ ! -x "${BIN}/${dir}/${exe}" ]; then
        echo "[start] 跳过 ${dir}：${BIN}/${dir}/${exe} 不存在"
        return 0
    fi
    render "${TPL}/${tpl}" "${BIN}/${dir}/config.ini"
    ( cd "${BIN}/${dir}" && exec "./${exe}" ) >> "${LOG}/${dir}.log" 2>&1 &
    echo "[start] ${dir} 已启动（对外地址 ${PUBLIC_HOST}）"
    sleep 1
}

start_service StatusServer  StatusServer  StatusServer/config.ini.example
start_service VerifyServer  VerifyServer  VerifyServer/config.ini.example
start_service ResourceServer ResourceServer ResourceServer/config.ini.example
start_service ChatServer    ChatServer    ChatServer/config.ini.example
start_service ChatServer2   ChatServer    ChatServer2/config.ini.example
start_service GateServer    GateServer    GateServer/config.ini.example

echo "[start] 监听端口："
ss -lntp 2>/dev/null | grep -E ":${GATE_PORT}|8090|8091|9090|50051|50052|50055|50056" || true
echo "[start] 服务日志：${LOG}"

wait
