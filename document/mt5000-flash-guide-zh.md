# GL.iNet GL-MT5000 (Brume 3) — bl-mt798x U-Boot 移植与刷入指南

> ⚠️ **刷写第三方 Bootloader 有变砖风险,请自担风险。** MT5000 需要 USB-TTL
> 串口线(115200 8N1)作为最后的恢复手段,强烈建议先备好再动手。

## 一、本移植包含什么

| 文件 | 作用 |
|---|---|
| `uboot-mtk-20250711/configs/mt7987_glinet_gl-mt5000_defconfig` | 默认变体(全功能:Web 恢复界面 + DHCPD + Flash Editor + 多语言) |
| `uboot-mtk-20250711/arch/arm/dts/mt7987a-glinet-gl-mt5000.dts` | 板级 DTS(复位键 GPIO4,电源灯 GPIO44,系统灯 GPIO7,eMMC) |
| `uboot-mtk-20250711/arch/arm/dts/mt7987a-glinet-gl-mt5000-u-boot.dtsi` | U-Boot 侧硬件描述(GMAC1 = WAN 口内置 2.5G PHY;GMAC0 = RTL8366UB 交换机固定 2500BASE-X;eMMC 保守 48MHz / 3.3V) |
| `uboot-mtk-20250711/drivers/net/mtk_eth/rtl8366ub.c` | RTL8366UB 交换机最小驱动(复位后重建 VLAN1 全口 untagged 转发、CPU 口 HS-GMII、关闭 RTL8_4 标签),**让 LAN 口在 U-Boot/failsafe 中可用** |
| `atf-20240117-bacca82a8/configs/mt7987_glinet_gl-mt5000_defconfig` | ATF(BL2)配置,基于 MT7987 eMMC DDR4 RFB |
| `mt798x_gpt/mt7987-gl_mt5000.json` | MT5000 **实测** eMMC GPT 布局(见下表) |

**MT5000 实测分区表**(2026-11-23 从真机 `/sys/block/mmcblk0/mmcblk0p*/`
读取;总计 15269888 扇区 ≈ 7.28GiB):

| 分区 | 起始扇区 | 起始字节 | 大小 | 备注 |
|---|---|---|---|---|
| p1 `u-boot-env` | 8192 | 0x400000 (4MiB) | 512KiB | 与 U-Boot `CONFIG_ENV_OFFSET` 一致 |
| p2 `factory` | 9216 | 0x480000 (4.5MiB) | **4MiB** | MAC/序列号/国家码,**绝不可覆盖** |
| p3 `fip` | **17408** | **0x880000 (8.5MiB)** | 2MiB | BL31+U-Boot |
| p4 `cfg` | 21504 | 0xA80000 (10.5MiB) | 2MiB | 原厂配置 |
| p5 `log` | 25600 | 0xC80000 (12.5MiB) | 2MiB | 原厂日志 |
| p6 `kernel` | 29696 | 0xE80000 (14.5MiB) | 32MiB | OpenWrt 内核+rootfs 前半 |
| p7 `rootfs` | 95232 | 0x2E80000 (46.5MiB) | ~7.24GiB | 数据分区 |

> ⚠️ **`fip` 在 8.5MiB(扇区 17408),不是 6.5MiB。** 早期按 GL-MT2500/MT6000
> 推断的 `0x680000` 在 MT5000 上会落进 `factory` 分区——覆盖 MAC 地址且刷不进
> 真正的 FIP。**必须用 `seek=17408`。**
> BL2 位于 eMMC **boot0 硬件分区**(4MiB,`/dev/mmcblk0boot0`)。

要点:

- **Failsafe Web/DHCPD 在 WAN 口与 lan1/lan2 上都可用**:
  - WAN(eth1,内置 2.5G PHY)——最稳的恢复入口;
  - lan1/lan2 经 GMAC0 → RTL8366UB 交换机,由 `rtl8366ub.c` 在 U-Boot 里
    重建扁平转发(VLAN1 全口 untagged)。该驱动**已真机验证可用**;若将来
    初始化失败,U-Boot 会打印 `rtl8366ub: ...` 且**不影响启动**,但 failsafe
    的网络会一起失效——`eth_get_dev()` 在 eth0 probe 失败时返回 NULL,U-Boot
    不会自动改走 eth1,两个口都进不去 Web 界面。
  - 交换机端口 2/3 在 MT5000 上没有引出,驱动会照常使能它们但无实际端口。
- 刷入本 U-Boot **不改变** kernel/rootfs 布局与固件刷写流程;之后仍可用 Web
  界面刷 `factory.bin` / `sysupgrade.bin`。
- Linux 启动后会用自己的 DSA 驱动重新配置交换机,U-Boot 的设置不会残留。

## 二、编译

### 方式 A:GitHub Actions(fork 本仓库后)

Actions → **FIP Build** → Run workflow:

- `MODEL`: `glinet_gl-mt5000`
- `FIP_VERSION`: **`SP2`**(MT7987 必须用 SP2 / TF-A 20260123)
- `FSTHEME`: **`bootstrap`**(README 截图里的新版界面:左侧栏导航、语言与明暗
  主题切换;`gl` 是 GL.iNet 风格的旧式单页界面)
- `REPO_URL`: **你的 fork 地址**(如 `https://github.com/Kitsune2077/bl-mt798x-dhcpd`)
- `REPO_BRANCH`: **`mt5000-support`**

> ⚠️ 此 workflow 不使用 actions/checkout,而是按 `REPO_URL`/`REPO_BRANCH`
> 重新 clone 源码——**fork 上构建自己的分支时必须显式填这两项**,否则会静默
> 构建上游 master(没有 MT5000,产物为空)。

产物发布在该次 run 对应的 GitHub Release 中:

- `bl2-mt7987_glinet_gl-mt5000_SP2_md5-<hash>_<date>.img` — BL2 预加载器
- `fip-mt7987_glinet_gl-mt5000_SP2-Yuzhii-dhcpd_md5-<hash>_<date>.bin` — FIP(BL31+U-Boot)

### 方式 B:本地编译(Linux)

```sh
sudo apt install gcc-aarch64-linux-gnu libssl-dev device-tree-compiler \
                 python3-dev swig libgnutls28-dev
# BOARD/SOC/VERSION 都是环境变量(build.sh 不吃位置参数)
BOARD=glinet_gl-mt5000 SOC=mt7987 VERSION=SP2 FSTHEME=bootstrap ./build.sh
# 产物: output/ 下的 bl2-*.img 与 fip-*.bin
```

## 三、刷入路径 A:系统还能启动(推荐)

在**正在运行的 OpenWrt / ImmortalWrt**(原厂固件亦可)SSH 中执行。

### 0. 先备份(必做!)

```sh
mkdir -p /tmp/bk && cd /tmp/bk

# ① BL2（eMMC boot0 硬件分区，4MiB）
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=/dev/mmcblk0boot0 of=mmcblk0boot0-bl2.img
echo 1 > /sys/block/mmcblk0boot0/force_ro
[ -e /dev/mmcblk0boot1 ] && dd if=/dev/mmcblk0boot1 of=mmcblk0boot1.img

# ② 引导区整段（GPT + env + factory + fip + cfg/log + kernel 头，12MiB）
dd if=/dev/mmcblk0 of=mmcblk0-0-12MiB.img bs=1M count=12

# ③ GPT 末尾备份表
S=$(cat /sys/block/mmcblk0/size)
dd if=/dev/mmcblk0 of=mmcblk0-tail-1MiB.img bs=512 skip=$((S-2048)) count=2048

# ④ 各分区按名字备份（<64MiB 的：env/factory/fip/cfg/log/kernel）
for d in /sys/block/mmcblk0/mmcblk0p*; do
  n=${d##*/}; lab=$(blkid -s PARTLABEL -o value /dev/$n 2>/dev/null); [ -n "$lab" ] || lab=$n
  z=$(cat "$d/size")
  [ "$z" -lt 131072 ] && dd if=/dev/$n of="part-${n}-${lab}.img" bs=1M 2>/dev/null
done

md5sum * && ls -l
```

**`/tmp` 是内存盘,重启即丢——先按第四节把备份导到电脑,再继续刷机。**

### 1. 写入 BL2 + FIP

```sh
# BL2 -> eMMC boot0 硬件分区（4MiB 够放 ~178KiB 的 BL2）
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=/tmp/bl2.img of=/dev/mmcblk0boot0 bs=512 conv=notrunc
sync
echo 1 > /sys/block/mmcblk0boot0/force_ro

# FIP -> fip 分区（实测 0x880000 = 扇区 17408，分区 2MiB）
dd if=/tmp/fip.bin of=/dev/mmcblk0 bs=512 seek=17408 conv=notrunc
sync
```

写 FIP 前可再确认一次目标分区内容确实是 FIP(前 4 字节 `01 00 64 aa`):

```sh
# 有 hexdump：
head -c 4 part-mmcblk0p3-fip.img | hexdump -C
# 没有 hexdump（用 strings 看是否含 U-Boot/MT7987）：
head -c 64 part-mmcblk0p3-fip.img | strings -n 4
```

### 2. 重启验证

`reboot`,接串口观察。成功标志:复位键长按进入 failsafe 时蓝灯闪烁,WAN 口
可获取 192.168.1.x,浏览器访问 `http://192.168.1.1`(或 `http://failsafe.lan`)。

## 四、导出备份文件(OpenWrt 没有 scp 也行)

OpenWrt 默认只有 **dropbear**(不带 sftp-server),所以从路由器 `scp` 拉文件
会失败。在你**自己的电脑**上执行(电脑有 `ssh` 即可,路由器端不用装东西):

```sh
mkdir -p ~/mt5000-backup && cd ~/mt5000-backup

# 方式 1：整目录走 SSH 管道（推荐）
ssh root@192.168.1.1 'tar -C /tmp -cf - bk' | tar -xf -

# 方式 2：逐文件 cat 重定向
ssh root@192.168.1.1 'cat /tmp/bk/mmcblk0boot0-bl2.img' > mmcblk0boot0-bl2.img
```

Windows 10+ 自带 `ssh` 与 `tar`,方式 1 同样可用(DOS 下路径换到
`%USERPROFILE%\mt5000-backup`)。

传完两边对 md5:

```sh
# 电脑上
md5sum *
# 路由器上（对照）
md5sum /tmp/bk/*
```

> 备选通道:路由器 `tar -cf - /tmp/bk | nc 电脑IP 9999`,电脑
> `nc -l -p 9999 > bk.tar`;或拷到 `/www/bk/` 用 `curl -O http://192.168.1.1/bk/xxx`。

## 五、刷入路径 B:系统已挂(UART 恢复)

需要 USB-TTL 接主板 UART 触点(115200),用
[mtk_uartboot](https://github.com/981213/mtk_uartboot) 在 BootROM 阶段把
BL2/FIP 先加载进 RAM 运行,再通过其 Web/TFTP 界面永久写回:

```sh
# 在路由器重启瞬间执行(抓住 BootROM 握手窗口)。
# -p 用本仓库构建的 bl2-*.img（内含 MT7987 eMMC DDR4 的 DRAM 初始化）
# -f 用 fip-*.bin
./mtk_uartboot -s /dev/ttyUSB0 -a \
  -p bl2-mt7987_glinet_gl-mt5000_SP2_md5-*.img \
  -f fip-mt7987_glinet_gl-mt5000_SP2-*.bin
```

RAM 引导起 U-Boot 后,用其菜单/TFTP 把 BL2、FIP 永久写入(BL2 → boot0;
FIP → `fip` 分区)。流程可参考
[Debricking a GL-MT2500A with UART](https://iraklijani.com/blog/debricking-a-gl-mt2500a-brume-2-with-uart)。

## 六、关于 GPT(通常不需要动)

`mt798x_gpt/mt7987-gl_mt5000.json` 已按真机实测填好,出厂 GPT 已存在,
**正常刷 BL2/FIP 无需重建**。仅当 GPT 被误删/需恢复时:

```sh
# 处理 mt798x_gpt/ 下所有 json，产物在 output_gpt/gpt-<name>.bin
# （需要 python2.7；只生成本板可先移走其它 json）
VERSION=SP2 ./generate_gpt.sh      # 等价于 make gpt VERSION=SP2

# 生成后用 failsafe Web 的 Flash Editor，或 UART 引导后的 U-Boot
# （gpt write mmc 0 $loadaddr）写回
```

> 重建 GPT 前务必先用 failsafe Web 备份全盘(MMC 设备 200~300MB 量级)。

## 七、刷入后怎么用

- **进 failsafe Web**:开机时长按复位键,指示灯闪烁后访问
  `http://192.168.1.1`(**WAN 口与 lan1/lan2 都可以**)。
- **远程进 failsafe(netabort)**:开机倒计时前 U-Boot 在 UDP 37541 监听
  `UBOOT:ABORT` / `BREED:ABORT` 魔术包,收到后回复 `<PROTO>:ABORTED` 并进入
  Web failsafe。**本板默认的 3 秒窗口不够**:交换机复位后 LAN 链路要约 5 秒
  才建立,窗口在此之前就关闭了。需要远程进入时先把监听窗口调大(例如往 env
  分区写一条 `net_abort_wait=8`),或直接按复位键。
- 注意:在 failsafe 里操作界面会让 U-Boot 执行 `env_save()`,env 分区会从
  出厂的"全零"变成完整环境(含 bootmenu、随机 MAC 等),属正常现象。
- 界面功能:刷固件(`factory.bin`/`sysupgrade.bin` 直传)、备份/恢复分区、
  DHCPD、多语言(中文)。
- **刷回原厂/换固件**:kernel/rootfs 布局未动,`factory.bin` 照常刷;想回
  GL 原厂,在原厂固件升级页上传即可(原厂固件可能校验/替换 bootloader)。
- **回滚本 U-Boot**:

  ```sh
  # BL2 回滚
  echo 0 > /sys/block/mmcblk0boot0/force_ro
  dd if=mmcblk0boot0-bl2.img of=/dev/mmcblk0boot0 bs=512 conv=notrunc && sync
  echo 1 > /sys/block/mmcblk0boot0/force_ro
  # FIP 回滚（扇区 17408）
  dd if=part-mmcblk0p3-fip.img of=/dev/mmcblk0 bs=512 seek=17408 conv=notrunc && sync
  ```

## 八、真机验证状态与已知限制

**2026-11-24 已在真机(1GB DDR4 / 8GB eMMC,运行 ImmortalWrt 6.18)上完成验证:**

- **启动**:刷入 BL2 + FIP 后连续多次正常启动,`/chosen/u-boot,version`
  为 `2025.07.1-Mediatek`;DRAM 1GiB、eMMC 8-bit 高速模式均正常。
- **分区布局**:fip@17408、env@8192、boot0 4MiB、factory 4MiB,与上表一致;
  刷机前后 factory / kernel / cfg / log / GPT / boot1 的 md5 未变。
- **LAN(failsafe)驱动**:`dm tree` 中 `ethernet@15110100` 与
  `ethernet@15110200` 均 probe 成功,LAN 口经 RTL8366UB 交换机可正常收发。
- **Failsafe**:Web 界面(固件 / BL2 / FIP / GPT / Factory 更新、备份、闪存
  编辑、环境管理、网页终端)、DHCP 服务器、telnet 控制台、netabort 均实测可用。
- **与原厂对照**:本移植的板级设备树与真机原厂 bootloader 内嵌的设备树逐项
  一致(网口、复位键、LED、eMMC 引脚),RTL8366UB 的 PHY patch 表与原厂驱动
  逐字节相同。
- **换主题**:只替换 `fip` 分区即可更换 failsafe 主题,BL2 与其他分区无需改动。

已知问题:

- **netabort 窗口偏短**:默认 3 秒在本板上不够,原因与解决办法见第七节。
- **telnet 服务端协商**:客户端每回一个 `DO`,服务端都会回一个 `WILL`
  (即使该选项已开启),简单脚本客户端可能陷入协商循环;普通终端客户端不受影响。
- U-Boot 里没有 STP/IGMP snooping 概念,交换机以"全口泛洪 + MAC 学习"工作,
  仅适合 bootloader 用途;正常系统由 Linux DSA 驱动接管。

以下两项是**出厂既有现象**,不是刷机造成的问题,不要用 parted 之类工具去"修复":

- 内核启动时打印 `GPT: Use GNU Parted to correct GPT errors.`——出厂 GPT 主头的
  `AlternateLBA` 为 1 且磁盘末尾没有备份头,内核照常识别全部 7 个分区。
- 内核打印 `u-boot-env-layout ... Invalid calculated CRC32`——出厂 env 分区
  为全零,属预期。
