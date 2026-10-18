# chromium-output-investigation

- **[敲定方案：fork `--headless=new` + 保留 HWND + 离屏输出](docs/architecture.md)**：全链路和 patch 位点
- **[接口与补丁说明](docs/interface.md)**：补丁内容、Chrome 侧接口、网关要实现的协议、未完成项
- [`patches/`](patches)：基于 Chromium M158（`f05e8113`）的补丁，`git am` 应用（未编译）
- [`protocol/cloud_stream_protocol.h`](protocol/cloud_stream_protocol.h)：Chrome 与网关共用的协议头
- [调研过程记录（存档）](docs/archive/investigation-notes.md)：选型过程，以及被否决的方案
