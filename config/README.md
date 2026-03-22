# 配置文件说明

- **`wss_server.conf`**：默认预置参数；`wss_server` 启动时若当前工作目录下存在 `config/wss_server.conf` 则自动加载。
- **`wss_server.example.conf`**：示例副本，可复制后改名使用。
- **格式**：`键=值`，每行一条；`#` 开头为注释；键名大小写不敏感。
- **优先级**：`config` 文件 < **命令行**（命令行始终可覆盖文件中的项）。

指定其它路径：

```bash
./build/wss_server --config /path/to/my.conf
```

若使用 `--config` 且文件不存在，进程会报错退出；若未指定 `--config` 且默认路径不存在，则仅使用内置默认值与命令行。
