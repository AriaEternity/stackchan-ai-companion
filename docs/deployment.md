# 自托管

`deploy/compose.yml` 是独立部署模板。它默认只把网关暴露到回环 `127.0.0.1:8787`，PostgreSQL 和 Redis 无宿主端口。首次运行前需准备自己的秘密与 HTTPS 域名。

1. 复制 `.env.example` 为被 Git 忽略的 `.env`，设置自己的访问密码、随机签名密钥、设备凭据、模型和数据库密码。
2. 数据库 URL 与 `POSTGRES_PASSWORD` 必须对应，特殊字符需要 URL 编码。样例中空值不能直接作为生产配置。
3. 可以通过 `PERSONA_FILE` 指向自己机器上的 UTF-8 角色文档；默认挂载新建的合成示例。
4. 运行 `docker compose --env-file .env -f deploy/compose.yml up -d --build`。
5. 按 `deploy/nginx-stackchan.conf.example` 配置自己的域名和证书，验证 HTTPS、登录与 WSS。

网页登录使用 Secure、HttpOnly、SameSite=Lax Cookie。设备凭据仅经 `Authorization` 请求头发送，设备 ID 与协议版本也用独立请求头。设备接口拒绝查询参数，关闭其访问日志。不要把凭据写入 URL。

生产使用是单用户、自托管场景；不含公共注册、多租户权限或完整的公网滥用防护。请自行结合部署环境配置访问控制与限流，并维护受控的数据库备份。

Compose 的镜像标签是可维护的发行系列标签；需要严格复现时，由部署者验证并锁定镜像摘要。本仓库没有携带任何原部署环境或数据库。

## 数据与更新

SQLite 位于运行目录，PostgreSQL 使用独立卷。更新源码之前先完成一致性备份；不要用示例数据覆盖已有数据库。项目会记录聊天和用户确认的记忆，请由部署者管理保留周期。
