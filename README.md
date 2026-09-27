# auto-DNxHD

`auto-DNxHD` 会定期扫描输入目录，把媒体文件的视频转换为 DNxHR HQ，把音频转换为 PCM `s16le`，最后写入 MOV 文件。

程序使用 FFmpeg 的库完成解码、DNxHR 视频编码、音频重采样和 PCM 编码。多个文件可以并行处理；同一个文件在当前进程中有任务运行时不会被重复提交。

## 功能

- 支持 MP4、MKV、MOV、AVI 和 MXF 输入文件
- 视频输出为 DNxHR HQ、8-bit、4:2:2
- 音频输出为 PCM `s16le`
- 使用 `max_workers` 限制并行转换数量
- 转换成功后可选择删除源文件
- 输出先写入 `.part` 临时文件，成功后再改名为 `.mov`

## 运行依赖


- Linux x86_64
- 与构建环境兼容的 `glibc`
- C++ 标准库运行时 `libstdc++.so.6`
- 输入目录和输出目录的读写权限


## 配置

程序从当前工作目录读取 `config.json`。首次启动时会自动创建配置文件和输入、输出目录。也可以复制示例配置：

```bash
cp config.example.json config.json
```

配置项：

| 配置项 | 说明 |
| --- | --- |
| `input_dir` | 待转换文件目录 |
| `output_dir` | MOV 输出目录 |
| `interval` | 两次扫描之间的秒数；首次启动会立即扫描 |
| `delete_source` | 转换成功后是否删除源文件，默认 `false` |
| `max_workers` | 同时转换的最大文件数 |

相对路径相对于程序的当前工作目录。

## 运行

```bash
./auto_DNxHD
```

输出示例：

```text
Converting: input/example.mp4
Finished: output/example.mov
```

如果转换失败，源文件会保留，临时的 `.part` 文件会被清理。当前进程中的任务会通过文件路径标记，后续扫描会跳过仍在处理的文件。这个标记不跨程序重启或多个程序实例共享。

## 许可证

本项目自身代码采用 **GNU Lesser General Public License v3.0**（LGPL-3.0-only）授权，完整文本见 [LICENSE](LICENSE)。

FFmpeg、nlohmann-json 及其他第三方组件按照各自的许可证发布；发布时应同时保留相应的版权和许可证声明。
