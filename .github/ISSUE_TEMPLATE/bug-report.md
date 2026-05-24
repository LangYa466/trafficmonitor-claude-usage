---
name: 报个 bug
about: 不显示、数值不动、闪退之类的
title: ''
labels: bug
---

先说下啥情况，越具体越好。

## 最要紧的：把 log 贴上来

插件每轮都会把发生的事记到日志里——trace 拿到的 colo/loc、节点匹没匹上、
token 读到没、usage 接口返回啥状态码、最后解析出来的百分比。九成的问题
扫一眼 log 就知道卡哪了，光说"不显示"我也看不出来。

log 在这：

```
C:\Program Files\TrafficMonitor\plugins\ClaudeUsage.log
```

要是你 TrafficMonitor 没装在默认位置，就是它安装目录下的 `plugins\ClaudeUsage.log`。

把最近几十行贴过来就行。贴之前**自己扫一眼有没有敏感信息**（正常 log 不会
写完整 token，但保险起见看一下）。

## 顺手补几句

- TrafficMonitor 版本 + 位数（x64 / 32 位）：
- 插件版本（设置里有，或你下的哪个 release）：
- 任务栏现在显示成啥（一直 `--` / `HTTP 429` / 完全不出来 / 别的）：
- 有没有挂代理、出口大概在哪：
