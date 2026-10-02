/*
 * luci-app-rtl-poe — RTL8238B PoE PSE 控制守护进程
 * ===================================================
 * 协议：RTL8238B 直接挂在 I2C 总线上（地址 0x20），用寄存器读写，
 *       **不是** 12 字节 Host Command（那是外挂 MCU 方案）。
 *
 *   写：I2C 写 2 字节 [寄存器地址, 值]
 *   读：I2C 先写 1 字节 [寄存器地址]，再读 1 字节 [值]
 *
 * 寄存器映射（从原厂固件 cetron_pse + rtmgr/csdp_pse8238.c 反汇编得到）：
 *   0x01  写  全局使能/初始化（写 0）
 *   0x12  写  端口控制（每端口 2 bit，4 端口打包 1 字节）
 *   0x30  读  功率状态（电压）
 *   0x3F  读  功率状态（电流）
 *
 * 端口映射（反序）：LAN7=端口0 … LAN1=端口6，共 7 个 PoE 口。
 *   端口 0-3（LAN7-4）→ I2C 0x20 寄存器 0x12
 *   端口 4-6（LAN3-1）→ I2C 0x21 寄存器 0x12
 *   每端口 2 bit：0 = 关，3 = 开（原厂 init 写 0x12=0xFF 即全开）
 *
 * 用法:
 *   rtl-poe run              — 守护进程模式 (procd 管理)
 *   rtl-poe status           — JSON 状态查询
 *   rtl-poe clear-log        — 清空日志
 *   rtl-poe log              — 查看日志
 *   rtl-poe version          — 版本
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#include "rtl8238b_fw.h"

/* ================================================================
 * 硬件常量
 * ================================================================ */
#define RTL8238B_I2C_BUS         "/dev/i2c-0"
#define RTL8238B_I2C_ADDR        0x20   /* 7-bit 基地址 (DTS: rtl8238b@20) */
#define RTL8238B_POE_PORT_COUNT  7      /* LAN1-LAN7 */
#define RTL8238B_PHY_CHANNELS    8      /* 芯片物理通道数 */

/* 寄存器地址 (从原厂固件反汇编) */
#define RTL8238B_REG_GLOBAL      0x01   /* 全局使能/初始化 */
#define RTL8238B_REG_PORT_CTRL   0x12   /* 端口控制 */
#define RTL8238B_REG_POWER_LO    0x30   /* 功率状态 */
#define RTL8238B_REG_POWER_HI    0x3F   /* 功率状态 */

/* 固件下载相关寄存器 (从 pse_8238_firmware_init 反汇编) */
#define RTL8238B_REG_DEVICE_ID   0xE8   /* 设备 ID (读) */
#define RTL8238B_REG_FW_STATUS   0xEC   /* 固件状态: bit5=已加载 bit2=下载模式 */
#define RTL8238B_REG_FW_CTRL     0x1A   /* 下载控制 (写 0x3F) */
#define RTL8238B_FW_CMD          0xF4   /* 固件数据写入命令字 */

/* 端口控制: 每端口 2 bit, 值 0=关 3=开 */
#define PORT_STATE_OFF           0x0
#define PORT_STATE_ON            0x3

#define RTL8238B_POLL_INTERVAL   2      /* 轮询间隔 (秒) */

/* 日志 */
#define RTL8238B_LOG_FILE        "/var/log/rtl-poe.log"
#define RTL8238B_LOG_MAX_SIZE    (128 * 1024)
#define RTL8238B_LOG_MAX_LINES   200
#define RTL8238B_UCI_CONFIG      "/etc/config/rtl-poe"
#define RTL8238B_LOG_LOCK        "/var/run/rtl-poe-log.lock"

/* ================================================================
 * 数据结构
 * ================================================================ */
struct port_status {
    int port;               /* 逻辑端口号 0-6 (LAN7-LAN1) */
    int enabled;            /* 用户配置: 1=使能 */
    int power_mw;           /* 功率 (mW) */
    int voltage_mv;         /* 电压 (mV) */
    int current_ma;         /* 电流 (mA) */
};

struct pse_config {
    int enabled;            /* 全局使能 */
    int budget_mw;          /* 功率预算 (mW) */
    int debug;
    int port_enabled[RTL8238B_PHY_CHANNELS];  /* 每端口使能 (逻辑端口号) */
};

/* ================================================================
 * 全局状态
 * ================================================================ */
static struct pse_config g_config = {
    .enabled = 1,
    .budget_mw = 120000,
    .debug = 0,
    .port_enabled = {1, 1, 1, 1, 1, 1, 1, 1},
};
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reload = 0;
static int g_i2c_fd = -1;
static FILE *g_log_fp = NULL;
static int g_log_lock_fd = -1;  /* 日志锁文件 fd, 仅在临界区短暂持锁 */

/* ================================================================
 * 日志管理
 * ================================================================ */
static void log_lock(void)
{
    if (g_log_lock_fd >= 0)
        flock(g_log_lock_fd, LOCK_EX);
}

static void log_unlock(void)
{
    if (g_log_lock_fd >= 0)
        flock(g_log_lock_fd, LOCK_UN);
}

/* 每次写日志只在临界区短暂持锁, 避免与 clear-log 竞争, 也不长期阻塞其它子命令 */
#define LOG_INFO(fmt, ...) do { \
    if (g_log_fp) { \
        log_lock(); \
        time_t now = time(NULL); \
        char ts[32]; \
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now)); \
        fprintf(g_log_fp, "[%s] " fmt "\n", ts, ##__VA_ARGS__); \
        fflush(g_log_fp); \
        log_unlock(); \
    } \
} while(0)

/* 错误日志: 同时输出到 stderr 和日志文件 */
#define LOG_ERR(fmt, ...) do { \
    fprintf(stderr, "rtl-poe: " fmt "\n", ##__VA_ARGS__); \
    if (g_log_fp) { \
        log_lock(); \
        time_t now = time(NULL); \
        char ts[32]; \
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now)); \
        fprintf(g_log_fp, "[%s] ERROR: " fmt "\n", ts, ##__VA_ARGS__); \
        fflush(g_log_fp); \
        log_unlock(); \
    } \
} while(0)

static void log_open(void)
{
    if (g_log_lock_fd >= 0)
        close(g_log_lock_fd);
    g_log_lock_fd = open(RTL8238B_LOG_LOCK, O_CREAT | O_RDWR, 0644);

    /* 轮转仅在持锁窗口内进行, 不长期持有 */
    log_lock();
    struct stat st;
    if (stat(RTL8238B_LOG_FILE, &st) == 0 && st.st_size > RTL8238B_LOG_MAX_SIZE)
        rename(RTL8238B_LOG_FILE, "/var/log/rtl-poe.log.1");
    log_unlock();

    g_log_fp = fopen(RTL8238B_LOG_FILE, "a");
}

static void log_close(void)
{
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
    if (g_log_lock_fd >= 0) { close(g_log_lock_fd); g_log_lock_fd = -1; }
}

/* ================================================================
 * I2C 寄存器读写
 * ================================================================ */
static int i2c_open_dev(const char *bus, uint8_t addr)
{
    int fd = open(bus, O_RDWR);
    if (fd < 0) {
        int e = errno;
        LOG_ERR("cannot open I2C bus %s: errno=%d (%s)", bus, e, strerror(e));
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        int e = errno;
        LOG_ERR("I2C_SLAVE ioctl failed (addr=0x%02x): errno=%d (%s)", addr, e, strerror(e));
        close(fd);
        return -1;
    }
    return fd;
}

/* 写寄存器: I2C 写 2 字节 [reg, val] 到 0x20 + exaddr */
static int pse_reg_write(int fd, uint8_t exaddr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    struct i2c_msg msg = {
        .addr  = RTL8238B_I2C_ADDR + exaddr,
        .flags = 0,
        .len   = 2,
        .buf   = buf,
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };

    if (ioctl(fd, I2C_RDWR, &rdwr) < 0) {
        LOG_ERR("I2C write failed: addr=0x%02x reg=0x%02x val=0x%02x errno=%d (%s)",
                RTL8238B_I2C_ADDR + exaddr, reg, val, errno, strerror(errno));
        return -1;
    }
    return 0;
}

/* 读寄存器: I2C 写 [reg] + 读 [val] */
static int pse_reg_read(int fd, uint8_t exaddr, uint8_t reg, uint8_t *val)
{
    uint8_t wr = reg;
    uint8_t rd = 0;
    struct i2c_msg msgs[2] = {
        { .addr = RTL8238B_I2C_ADDR + exaddr, .flags = 0, .len = 1, .buf = &wr },
        { .addr = RTL8238B_I2C_ADDR + exaddr, .flags = I2C_M_RD, .len = 1, .buf = &rd },
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = msgs, .nmsgs = 2 };

    if (ioctl(fd, I2C_RDWR, &rdwr) < 0) {
        LOG_ERR("I2C read failed: addr=0x%02x reg=0x%02x errno=%d (%s)",
                RTL8238B_I2C_ADDR + exaddr, reg, errno, strerror(errno));
        return -1;
    }
    *val = rd;
    return 0;
}

/* 批量写: 写 [cmd, data...] (len+1 字节) 到 0x20 + exaddr (固件下载用) */
static int pse_reg_write_buf(int fd, uint8_t exaddr, uint8_t cmd, const uint8_t *data, int len)
{
    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        LOG_ERR("malloc(%d) failed", len + 1);
        return -1;
    }
    buf[0] = cmd;
    memcpy(buf + 1, data, len);

    struct i2c_msg msg = {
        .addr  = RTL8238B_I2C_ADDR + exaddr,
        .flags = 0,
        .len   = len + 1,
        .buf   = buf,
    };
    struct i2c_rdwr_ioctl_data rdwr = { .msgs = &msg, .nmsgs = 1 };

    int ret = ioctl(fd, I2C_RDWR, &rdwr);
    if (ret < 0) {
        LOG_ERR("I2C bulk write failed: addr=0x%02x cmd=0x%02x len=%d errno=%d (%s)",
                RTL8238B_I2C_ADDR + exaddr, cmd, len, errno, strerror(errno));
    }
    free(buf);
    return ret < 0 ? -1 : 0;
}

/* ================================================================
 * RTL8238B 操作
 * ================================================================ */

/* 下载 RTL8238B 内部固件 (与原厂 pse_8238_firmware_init 一致)。
 * RTL8238B 固件在 SRAM 中, 每次上电都要重新下载, 否则功率寄存器不工作。 */
static int rtl8238b_firmware_download(int fd)
{
    uint8_t val;

    /* 固件已加载则跳过 (0xEC bit5) */
    if (pse_reg_read(fd, 0, RTL8238B_REG_FW_STATUS, &val) == 0 && (val & 0x20))
        return 0;

    /* 读设备 ID 选择固件: ==2 用小固件, 否则用大固件 */
    uint8_t dev_id = 0;
    pse_reg_read(fd, 0, RTL8238B_REG_DEVICE_ID, &dev_id);

    const unsigned char *fw = (dev_id == 2) ? rtl8238b_fw_small : rtl8238b_fw_big;
    int fw_len = (dev_id == 2) ? sizeof(rtl8238b_fw_small) : sizeof(rtl8238b_fw_big);

    LOG_INFO("downloading RTL8238B firmware (dev_id=0x%02x, %d bytes)", dev_id, fw_len);

    /* 进入下载模式 */
    pse_reg_write(fd, 0, RTL8238B_REG_FW_STATUS, 0x03);
    pse_reg_write(fd, 0, RTL8238B_REG_FW_CTRL, 0x3F);
    usleep(50000);
    pse_reg_write(fd, 0, RTL8238B_REG_FW_STATUS, 0x03);

    /* 下载固件: 写 [0xF4, 固件数据] */
    if (pse_reg_write_buf(fd, 0, RTL8238B_FW_CMD, fw, fw_len) < 0)
        return -1;

    usleep(50000);

    /* 校验: 0xEC bit5 置位 = 成功 */
    if (pse_reg_read(fd, 0, RTL8238B_REG_FW_STATUS, &val) < 0 || (val & 0x20) == 0) {
        LOG_ERR("RTL8238B firmware download failed (0xEC=0x%02x)", val);
        return -1;
    }

    LOG_INFO("RTL8238B firmware downloaded");
    return 0;
}

/* 初始化 (与原厂 rtl8238b_probe 一致):
 *   下载固件 → 写 0x01=0x00 → 写 0x12=0x00 → 延时 → 写 0x12=0xFF */
static int rtl8238b_init(int fd)
{
    rtl8238b_firmware_download(fd);

    pse_reg_write(fd, 0, RTL8238B_REG_GLOBAL, 0x00);
    pse_reg_write(fd, 0, RTL8238B_REG_PORT_CTRL, 0x00);
    usleep(100000);
    pse_reg_write(fd, 0, RTL8238B_REG_PORT_CTRL, 0xFF);
    return 0;
}

/* 计算某个逻辑端口的寄存器地址和位偏移
 *   端口 0-3 → exaddr 0, 端口 4-6 → exaddr 1
 *   每端口 2 bit: 位偏移 = (port % 4) * 2 */
static int port_to_reg(int port, uint8_t *exaddr, int *shift)
{
    *exaddr = (port >= 4) ? 1 : 0;
    *shift = (port % 4) * 2;
    return 0;
}

/* 读端口控制寄存器, 返回某端口的 2-bit 值 (0-3) */
static int read_port_state(int fd, int port, uint8_t *state)
{
    uint8_t exaddr, val;
    int shift;

    port_to_reg(port, &exaddr, &shift);
    if (pse_reg_read(fd, exaddr, RTL8238B_REG_PORT_CTRL, &val) < 0)
        return -1;
    *state = (val >> shift) & 0x3;
    return 0;
}

/* 设置某端口的使能状态 (开=3, 关=0), 读-改-写 */
static int set_port_state(int fd, int port, int on)
{
    uint8_t exaddr, val;
    int shift;

    port_to_reg(port, &exaddr, &shift);
    if (pse_reg_read(fd, exaddr, RTL8238B_REG_PORT_CTRL, &val) < 0)
        return -1;

    uint8_t v = on ? PORT_STATE_ON : PORT_STATE_OFF;
    val &= ~(0x3 << shift);
    val |= (v << shift);

    return pse_reg_write(fd, exaddr, RTL8238B_REG_PORT_CTRL, val);
}

/* 读功率/电压/电流状态。
 * 从原厂反汇编(rtmgr csdp_pse8238.c + 内核 ioctl 0x40847004):
 *   读命令参数 = [起始寄存器 0x30, exaddr, 结束寄存器 0x3F], 返回 0x30..0x3F 共 16 个寄存器值。
 *   每端口占 4 个寄存器(仅低 4 口 = 一组):
 *     [0x30+p*4] 电压低字节, [0x31+p*4] 电压高字节 (16bit)
 *     [0x32+p*4] 温度(暂未用)
 *     [0x33+p*4] 电流
 *   power = ((volt_word * 250) >> 13) * ((curr>>1) & 0x3F)
 * 端口 4-6 用 exaddr=1 (I2C 0x21) 的同一组寄存器。 */
static int rtl8238b_read_power(int fd, struct port_status *ports, int *total_mw)
{
    int total = 0;

    for (int i = 0; i < RTL8238B_POE_PORT_COUNT; i++) {
        uint8_t exaddr = (i >= 4) ? 1 : 0;
        int p = i % 4;
        uint8_t vlo = 0, vhi = 0, cur = 0;

        uint8_t reg_vlo = RTL8238B_REG_POWER_LO + p * 4;   /* 0x30 + p*4 */
        uint8_t reg_vhi = reg_vlo + 1;                     /* 0x31 + p*4 */
        uint8_t reg_cur = reg_vlo + 3;                     /* 0x33 + p*4 */

        if (pse_reg_read(fd, exaddr, reg_vlo, &vlo) == 0 &&
            pse_reg_read(fd, exaddr, reg_vhi, &vhi) == 0 &&
            pse_reg_read(fd, exaddr, reg_cur, &cur) == 0) {
            int volt_word = vlo | (vhi << 8);
            int curr = (cur >> 1) & 0x3F;
            int power = ((volt_word * 250) >> 13) * curr;

            ports[i].power_mw = power;
            ports[i].voltage_mv = volt_word;   /* 原始值, 单位待确认 */
            ports[i].current_ma = curr;        /* 原始值, 单位待确认 */
            total += power;
        }
    }

    *total_mw = total;
    return 0;
}

/* 读取全部端口状态 */
static int rtl8238b_read_all(int fd, struct pse_config *cfg, struct port_status *ports, int *total_mw)
{
    for (int i = 0; i < RTL8238B_POE_PORT_COUNT; i++) {
        ports[i].port = i;
        ports[i].enabled = cfg->port_enabled[i];
        uint8_t st;
        if (read_port_state(fd, i, &st) == 0)
            ports[i].enabled = (st != 0) ? 1 : 0;
    }

    rtl8238b_read_power(fd, ports, total_mw);
    return 0;
}

/* 应用端口使能配置 */
static int rtl8238b_apply_config(int fd, struct pse_config *cfg)
{
    for (int i = 0; i < RTL8238B_POE_PORT_COUNT; i++)
        set_port_state(fd, i, cfg->port_enabled[i]);
    return 0;
}

/* ================================================================
 * UCI 配置读取 (简易行解析)
 * ================================================================ */
static void uci_read_config(struct pse_config *cfg)
{
    FILE *fp = fopen(RTL8238B_UCI_CONFIG, "r");
    if (!fp) {
        int e = errno;
        LOG_ERR("cannot open config %s: errno=%d (%s)", RTL8238B_UCI_CONFIG, e, strerror(e));
        return;
    }

    char line[256];
    int in_poe = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == '#' || p[0] == '\n') continue;

        if (strncmp(p, "config", 6) == 0) {
            in_poe = (strstr(p, "rtl-poe") || strstr(p, "poe"));
            continue;
        }
        if (!in_poe) continue;

        char key[64], val[64];
        if (sscanf(p, "option %63s '%63[^']'", key, val) == 2 ||
            sscanf(p, "option %63s \"%63[^\"]\"", key, val) == 2) {
            if (strcmp(key, "enabled") == 0)
                cfg->enabled = atoi(val);
            else if (strcmp(key, "budget_mw") == 0)
                cfg->budget_mw = atoi(val);
            else if (strcmp(key, "debug") == 0)
                cfg->debug = atoi(val);
            else if (strncmp(key, "port_lan", 8) == 0) {
                /* port_lan1..port_lan7 → 逻辑端口 6..0 (反序) */
                int lan = atoi(key + 8);        /* 1..7 */
                int port = 7 - lan;             /* LAN1=6 .. LAN7=0 */
                if (lan >= 1 && lan <= 7)
                    cfg->port_enabled[port] = atoi(val);
            }
        }
    }
    fclose(fp);
}

/* ================================================================
 * JSON 输出
 * ================================================================ */
static void json_status(struct pse_config *cfg, struct port_status *ports, int total_mw, FILE *out)
{
    fprintf(out, "{\n");
    fprintf(out, "  \"controller\": \"RTL8238B\",\n");
    fprintf(out, "  \"total_power_mw\": %d,\n", total_mw);
    fprintf(out, "  \"budget_mw\": %d,\n", cfg->budget_mw);
    fprintf(out, "  \"ports\": [\n");

    for (int i = 0; i < RTL8238B_POE_PORT_COUNT; i++) {
        struct port_status *p = &ports[i];
        int lan = 7 - i;   /* 逻辑端口 i → 物理 LAN(7-i) */
        fprintf(out, "    {\n");
        fprintf(out, "      \"port\": %d,\n", lan);
        fprintf(out, "      \"label\": \"LAN%d\",\n", lan);
        fprintf(out, "      \"enabled\": %s,\n", p->enabled ? "true" : "false");
        fprintf(out, "      \"power_mw\": %d,\n", p->power_mw);
        fprintf(out, "      \"voltage_mv\": %d,\n", p->voltage_mv);
        fprintf(out, "      \"current_ma\": %d\n", p->current_ma);
        fprintf(out, "    }%s\n", (i < RTL8238B_POE_PORT_COUNT - 1) ? "," : "");
    }
    fprintf(out, "  ]\n}\n");
}

/* ================================================================
 * 信号处理
 * ================================================================ */
static void signal_handler(int sig)
{
    if (sig == SIGTERM || sig == SIGINT)
        g_running = 0;
    else if (sig == SIGHUP)
        g_reload = 1;
}

/* ================================================================
 * 守护进程
 * ================================================================ */
static int daemon_run(void)
{
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    signal(SIGHUP, signal_handler);

    LOG_INFO("RTL8238B PoE daemon starting");

    g_i2c_fd = i2c_open_dev(RTL8238B_I2C_BUS, RTL8238B_I2C_ADDR);
    if (g_i2c_fd < 0) {
        LOG_ERR("FATAL: cannot open I2C bus %s", RTL8238B_I2C_BUS);
        return 1;
    }

    rtl8238b_init(g_i2c_fd);
    LOG_INFO("RTL8238B initialized (reg 0x01=0, 0x12=0xFF)");

    if (g_config.enabled)
        rtl8238b_apply_config(g_i2c_fd, &g_config);

    while (g_running) {
        if (g_reload) {
            g_reload = 0;
            uci_read_config(&g_config);
            if (g_config.enabled)
                rtl8238b_apply_config(g_i2c_fd, &g_config);
            LOG_INFO("Configuration reloaded");
        }

        struct port_status ports[RTL8238B_POE_PORT_COUNT];
        memset(ports, 0, sizeof(ports));
        int total_mw = 0;
        rtl8238b_read_all(g_i2c_fd, &g_config, ports, &total_mw);

        int powered = 0;
        for (int i = 0; i < RTL8238B_POE_PORT_COUNT; i++)
            if (ports[i].power_mw > 0) powered++;

        LOG_INFO("Poll: %d/%d ports powered, total %dmW", powered, RTL8238B_POE_PORT_COUNT, total_mw);

        sleep(RTL8238B_POLL_INTERVAL);
    }

    LOG_INFO("RTL8238B PoE daemon stopping");
    close(g_i2c_fd);
    return 0;
}

/* ================================================================
 * 命令行接口
 * ================================================================ */
static int cmd_status(void)
{
    int fd = i2c_open_dev(RTL8238B_I2C_BUS, RTL8238B_I2C_ADDR);
    if (fd < 0)
        return 1;

    /* 确保固件已加载 (幂等, 已加载则跳过) */
    rtl8238b_firmware_download(fd);

    uci_read_config(&g_config);

    struct port_status ports[RTL8238B_POE_PORT_COUNT];
    memset(ports, 0, sizeof(ports));
    int total_mw = 0;
    rtl8238b_read_all(fd, &g_config, ports, &total_mw);

    json_status(&g_config, ports, total_mw, stdout);

    close(fd);
    return 0;
}

/* 调试: 读取并打印所有寄存器原始值 (exaddr 0 和 1) */
static int cmd_dump_regs(void)
{
    int fd = i2c_open_dev(RTL8238B_I2C_BUS, RTL8238B_I2C_ADDR);
    if (fd < 0)
        return 1;

    rtl8238b_firmware_download(fd);

    for (int exaddr = 0; exaddr <= 1; exaddr++) {
        printf("=== exaddr %d (I2C 0x%02x) ===\n", exaddr, RTL8238B_I2C_ADDR + exaddr);
        for (int reg = 0x00; reg <= 0x7F; reg += 16) {
            printf("  %02x:", reg);
            for (int r = 0; r < 16; r++) {
                uint8_t v = 0;
                if (pse_reg_read(fd, exaddr, reg + r, &v) == 0)
                    printf(" %02x", v);
                else
                    printf(" --");
            }
            printf("\n");
        }
    }
    close(fd);
    return 0;
}

static int cmd_clear_log(void)
{
    log_lock();

    int fd = open(RTL8238B_LOG_FILE, O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    unlink("/var/log/rtl-poe.log.1");

    log_unlock();
    return 0;
}

static int cmd_log(void)
{
    FILE *fp = fopen(RTL8238B_LOG_FILE, "r");
    if (!fp) return 0;

    char **lines = NULL;
    int count = 0, cap = 0;
    char buf[512];

    while (fgets(buf, sizeof(buf), fp)) {
        if (count >= cap) {
            cap = cap ? cap * 2 : 200;
            lines = realloc(lines, cap * sizeof(char *));
        }
        lines[count++] = strdup(buf);
    }
    fclose(fp);

    int start = count > RTL8238B_LOG_MAX_LINES ? count - RTL8238B_LOG_MAX_LINES : 0;
    for (int i = start; i < count; i++) {
        fputs(lines[i], stdout);
        free(lines[i]);
    }
    free(lines);
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command>\n"
        "Commands:\n"
        "  run              Run as daemon (procd service)\n"
        "  status           Print JSON status\n"
        "  regs             Dump all registers (debug)\n"
        "  clear-log        Clear the log file\n"
        "  log              Print the last 200 log lines\n"
        "  version          Print version\n",
        prog);
}

int main(int argc, char *argv[])
{
    int ret;
    const char *cmd = argc > 1 ? argv[1] : "";

    log_open();
    uci_read_config(&g_config);

    if (strcmp(cmd, "run") == 0) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) return 0;

        setsid();
        umask(0);
        if (g_log_fp) fclose(g_log_fp);
        log_open();
        ret = daemon_run();

    } else if (strcmp(cmd, "status") == 0) {
        ret = cmd_status();

    } else if (strcmp(cmd, "regs") == 0) {
        ret = cmd_dump_regs();

    } else if (strcmp(cmd, "clear-log") == 0) {
        ret = cmd_clear_log();

    } else if (strcmp(cmd, "log") == 0) {
        ret = cmd_log();

    } else if (strcmp(cmd, "version") == 0) {
        printf("rtl-poe version 20261002-reg\n");
        ret = 0;

    } else {
        print_usage(argv[0]);
        ret = 1;
    }

    log_close();
    return ret;
}
