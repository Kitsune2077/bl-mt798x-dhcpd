# GL.iNet GL-MT5000 (Brume 3) — bl-mt798x U-Boot 移植与刷入指南

> ⚠️ **刷写第三方 Bootloader 有变砖风险,请自担风险。** MT5000 需要 USB-TTL
> 串口线(115200 8N1)作为最后的恢复手段,强烈建议先备好再动手。

## 一、本移植包含什么

| 文件 | 作用 |
|---|---|
| `uboot-mtk-20250711/configs/mt7987_glinet_gl-mt5000_defconfig` | 默认变体(bl 全功能:Web 恢复界面 + DHCPD + Flash Editor + 多语言) |
| `uboot-mtk-20250711/arch/arm/dts/mt7987a-glinet-gl-mt5000.dts` | 板级 DTS(复位键 GPIO4,电源灯 GPIO44,系统灯 GPIO7,eMMC) |
| `uboot-mtk-20250711/arch/arm/dts/mt7987a-glinet-gl-mt5000-u-boot.dtsi` | U-Boot 侧硬件描述(**仅启用 GMAC1 = WAN 口内置 2.5G PHY**,LAN 交换机无驱动不启用;eMMC 保守 48MHz / 3.3V) |
| `atf-20240117-bacca82a8/configs/mt7987_glinet_gl-mt5000_defconfig` | ATF(BL2)配置,基于 MT7987 eMMC DDR4 RFB |
| `mt798x_gpt/mt7987-gl_mt5000.json` | GL 标准 eMMC GPT 布局(env@4MB / factory@4.5MB / fip@6.5MB / kernel@12.5MB) |

要点:
- **Failsafe Web 界面/DHCPD 走 WAN 口(eth1 内置 2.5G PHY)**——LAN 口在
  U-Boot 阶段没有驱动(RTL8366UB 交换机仅在内核里工作)。电脑请接 WAN 口。
- 刷入本 U-Boot **不改变** kernel/rootfs 分区与 OpenWrt/ImmortalWrt 固件的
  刷写流程;之后仍可用 Web 界面刷 `factory.bin` / `sysupgrade`。

## 二、编译

### 方式 A:GitHub Actions(fork 本仓库后)

Actions → **FIP Build** → Run workflow:
- `MODEL`: `glinet_gl-mt5000`
- `FIP_VERSION`: **`SP2`**(MT7987 必须用 SP2 / TF-A 20260123)
- `FSTHEME`: `gl`(GL 风格恢复界面,可选)
- `REPO_URL`: **你的 fork 地址**(如 `https://github.com/Kitsune2077/bl-mt798x-dhcpd`)
- `REPO_BRANCH`: **`mt5000-support`**

> ⚠️ 注意:此 workflow 不使用 actions/checkout,而是按 `REPO_URL`/`REPO_BRANCH`
> 重新 clone 源码——**fork 上构建自己的分支时必须显式填写这两项**,否则会
> 静默构建上游 master(没有 MT5000,产物为空)。

产物发布在该次 run 对应的 GitHub Release 中:
- `bl2-mt7987_glinet_gl-mt5000_SP2_md5-<hash>_<date>.img` — BL2 预加载器
- `fip-mt7987_glinet_gl-mt5000_SP2-Yuzhii-dhcpd_md5-<hash>_<date>.bin` — FIP(BL31+U-Boot)

### 方式 B:本地编译(Linux)

```sh
sudo apt install gcc-aarch64-linux-gnu libssl-dev device-tree-compiler \
                 python3-dev swig libgnutls28-dev
./build.sh glinet_gl-mt5000 mt7987 SP2 FSTHEME=gl
# 产物: output/mt7987_glinet_gl-mt5000/{bl2.img, fip}
```

## 三、刷入(路径 A:系统还能启动 —— 推荐)

在**正在运行的 OpenWrt / ImmortalWrt**(原厂固件亦可)SSH 中执行:

### 0. 先备份(必做!)

```sh
mkdir -p /tmp/backup && cd /tmp/backup
# 备份 BL2(eMMC boot0 硬件分区)
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=/dev/mmcblk0boot0 of=bl2-backup.bin
echo 1 > /sys/block/mmcblk0boot0/force_ro
# 备份 FIP(0x680000 起 2MB, GL 标准 eMMC 布局)
dd if=/dev/mmcblk0 of=fip-backup.bin bs=512 skip=13312 count=4096
# 把两个备份文件 scp 出路由器保管!
```

### 1. 写入 BL2 + FIP

把 Release 里的 `bl2-*.img`、`fip-*.bin` scp 到 `/tmp`(下文以
`bl2.img` / `fip.bin` 代称),然后:

```sh
# BL2 -> eMMC boot0 硬件分区
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=/tmp/bl2.img of=/dev/mmcblk0boot0 bs=512
sync
echo 1 > /sys/block/mmcblk0boot0/force_ro

# FIP -> fip 分区(0x680000 = 扇区 13312)
dd if=/tmp/fip.bin of=/dev/mmcblk0 bs=512 seek=13312 conv=notrunc
sync
```

### 2. 重启验证

`reboot`,串口/观察指示灯。成功标志:复位键长按进入 failsafe 时蓝灯闪烁,
WAN 口可获取 192.168.1.x,浏览器访问 `http://192.168.1.1`(或 `http://failsafe.lan`)。

## 四、刷入(路径 B:系统已挂 —— UART 恢复)

需要 USB-TTL 接到主板 UART 触点(115200),用 [mtk_uartboot](
https://github.com/981213/mtk_uartboot)在 BootROM 阶段把 BL2/FIP 先加载进
RAM 运行,再通过其 Web/TFTP 界面写回:

```sh
# 在路由器重启瞬间执行(抓住 BootROM 握手窗口):
./mtk_uartboot -s /dev/ttyUSB0 -a \
  -p bl2-mt7987-emmc-ddr4-ram.bin \
  -f mt7987_glinet_gl-mt5000_fip.bin
```

RAM 引导起 bl U-Boot 后,用其菜单/TFTP 把 BL2、FIP 永久写入
(与 GL-MT2500 的社区流程一致,详见
[Debricking a GL-MT2500A with UART](https://iraklijani.com/blog/debricking-a-gl-mt2500a-brume-2-with-uart))。

## 五、刷入后怎么用

- **进 failsafe Web**:开机时长按复位键(或开机后连按 5 次复位键,取决于
  版本),指示灯闪烁后从 **WAN 口** 访问 `http://192.168.1.1`。
- 界面功能:刷固件(`factory.bin`/`sysupgrade.bin` 直传)、备份/恢复
  分区、DHCPD、多语言(中文)。
- **刷回原厂/换固件**:与本仓库 ImmortalWrt 固件完全兼容——kernel/rootfs
  分区布局未动,`factory.bin` 照常刷;想回 GL 原厂,在 GL 固件升级页上传
  原厂固件即可(注意原厂固件可能会校验/替换 bootloader)。
- **回滚本 U-Boot**:用第一步备份的 `bl2-backup.bin`、`fip-backup.bin`
  反向 `dd` 回去即可。

## 六、已知限制 / 待验证

- **编译已验证**(GitHub Actions,SP2):产出 bl2 + fip。
- **尚未真机验证**:DRAM 初始化使用 MT7987 eMMC DDR4 通用配置(设备为
  1GB DDR4),首次上电请接串口(115200)观察 DRAM size 是否为 1024MB、
  eMMC 是否枚举;failsafe Web 请从 WAN 口访问。

- DRAM 初始化使用 MT7987 eMMC DDR4 通用配置(与 RFB 同),1GB DDR4 预期
  兼容;首次上电请接串口观察 DRAM size 是否为 1024MB。
- FIP/BL2 的 `emmc_write_*` 均按 GL 标准布局(fip@0x680000);若你的机器
  GPT 有差异,先在 failsafe Web 的 Flash Editor / `mmc part` 核对。
- LAN 口(交换机下的 lan1/lan2)在 U-Boot 里不可用——所有网络恢复操作
  请走 WAN 口。
