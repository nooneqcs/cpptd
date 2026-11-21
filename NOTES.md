## 2026-11-21 晚 进度
- CMake + git 就绪（3 次提交）；CTP .so 用 IMPORTED 目标
- iptables 免密 sudoers: /etc/sudoers.d/ctp-drill → sudo -n /usr/sbin/iptables
- tick→报单打点完成：rdtsc_now/TickMsg.tsc、tick_cb、tsc_to_ns 标定、__tick_order/lat
- 离线验证：lat n=0、__tick_order → REJECT not ready (INIT)
- 注意：CMake 产物在 build/cpptd，根目录旧手工 cpptd 已删
- 待办：换可用前置 → READY → 真实 LAT 数字 → sub 行情 → drill 演练
- 状态：待定（阻塞于前置）
## 约定（2026-11-21 起）
- 结论先写 NOTES.md → commit → 对外口径再打 tag
