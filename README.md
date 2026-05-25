## 效果
<img width="105" height="71" alt="image" src="https://github.com/user-attachments/assets/25681880-97d6-4f37-96c9-d315563f912c" />

## Star History

<a href="https://www.star-history.com/?repos=LangYa466%2Ftrafficmonitor-claude-usage&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=LangYa466/trafficmonitor-claude-usage&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=LangYa466/trafficmonitor-claude-usage&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=LangYa466/trafficmonitor-claude-usage&type=date&legend=top-left" />
 </picture>
</a>

## 数据怎么来的

跟 Claude Code 自己用的那套一样：

- 读 `~/.claude/.credentials.json` 里的 `accessToken`
- 拿它去打 `https://api.anthropic.com/api/oauth/usage`
- 取响应里 `five_hour` / `seven_day` 的 `utilization`

默认每 3 分钟刷一次（间隔能改，见下面）。打太勤会被限流（HTTP 429），别手贱调太低。

## 出口节点校验

我是挂代理用的，不太想让 token 从随便哪个出口节点裸奔出去。所以每次请求 usage 之前，会先 GET 一下 `https://claude.ai/cdn-cgi/trace`，看里面的 `colo`（Cloudflare 节点）和 `loc`（国家）：

- 两个都跟设置里一致（默认 `NRT` / `JP`，也就是东京出口）才发请求
- 对不上就跳过这次，弹个系统通知提醒一下（同一次掉到错误节点只提醒一次，不刷屏）

用不上这功能的话，把 colo / loc 改成你当前出口的值就行。

## 编译

需要 MSVC（VS2022 Build Tools 就够）+ CMake：

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物是 `build/Release/ClaudeUsage.dll`，x64、静态链接 CRT，拷到别的机器也不用额外装运行库。

> 插件位数要跟 TrafficMonitor 主程序对上。我的是 x64；如果你装的是 32 位版，改成 `-A Win32`。

懒得自己编的话直接去 [Releases](https://github.com/LangYa466/trafficmonitor-claude-usage/releases) 下 dll。

## 安装

把 `ClaudeUsage.dll` 丢进 TrafficMonitor 安装目录下的 `plugins` 文件夹：

```
C:\Program Files\TrafficMonitor\plugins\ClaudeUsage.dll
```

重启 TrafficMonitor，右键任务栏 → 显示设置，把 **Claude 5h 用量 / Claude 7d 用量** 两项勾上。想上下两行就把它们放到不同行。

## 手动刷新

不想等下一轮间隔的话，左键点一下任务栏上的 5h / 7d 数字，就立刻强制刷新一次。刷新进行中，数字会变成一个滚动的字符（`| / - \`）动画，让你确认确实触发了；刷完再变回百分比。手动刷新要是失败了（节点不匹配、没 token、429、网络错误等），会弹一条系统通知说明简单原因，不用专门去翻 log。

## 设置

右键插件 → 设置，能改这几个：

| 项 | 说明 |
| --- | --- |
| colo | 期望的 Cloudflare 节点，默认 `NRT` |
| loc | 期望的国家代码，默认 `JP` |
| jsonpath | `credentials.json` 路径，留空就自动找 |
| 间隔(分) | 刷新间隔，默认 3；填得比默认小会先警告 |
| 开机后首次必须手动刷新 | 勾上后开机不自动拉，要你手动点一次 5h/7d 才触发首次获取。开机后还没用 Claude Code 时，出口节点 / token 常常没就绪，自动拉会失败又看不出原因——这个选项就是为了避开它。 |

配置存在 `plugins\ClaudeUsage.ini`。

## 不显示 / 数值不动？

插件会往 `plugins\ClaudeUsage.log` 写日志，每轮记一遍 trace 结果、节点匹没匹上、token 读到没、HTTP 状态、最后的百分比。出问题先看这个，一眼就知道卡在哪。

常见的：

- 一直 `--`：多半是节点对不上（看 log 里的 trace colo/loc），或者代理没开
- `HTTP 429`：刷太勤了，等会儿就好，或者把间隔调大
- 找不到 token：没登录 Claude Code，或者 credentials 不在默认位置，去设置里填 jsonpath

## 致谢

- 插件接口照着 TrafficMonitor 的[插件开发指南](https://github.com/zhongyang219/TrafficMonitor/wiki/%E6%8F%92%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97)写的
- JSON 解析用 [nlohmann/json](https://github.com/nlohmann/json)
- NodeSeek(https://nodeseek.com)
- LinuxDo(https://linux.do/)

## License

MIT
