/* 必须包含的两个头文件 */
#include <linux/module.h>
#include <linux/init.h>
/* 设备号,文件操作 */
#include <linux/fs.h>
/* 字符设备头文件 */
#include <linux/cdev.h>
/* 平台设备头文件 */
#include <linux/platform_device.h>
/* 设备节点创建 */
#include <linux/device.h>
/* ioremap */
#include <asm/io.h>
/* 内存拷贝 */
#include <asm/uaccess.h>
/* gpio子系统 */
#include <linux/of_gpio.h>
/* 中断子系统 */
#include <linux/interrupt.h>
#include <linux/of_irq.h>
/* 定时器 */
#include <linux/timer.h>

#if 1
    #define LOG_TRACE()  printk("\nkdebug: [file]:%s [func]:%s [line]:%d\n", __FILE__, __FUNCTION__, __LINE__)
#else
    #define LOG_TRACE() 
#endif

#define DEVICE_NUM 1
#define GPIO_DEV_NAME "my_key"
#define DELAY_TIME_MS   20
struct key_dev_info {
    struct gpio_desc *gpiod;    /* gpio描述符 */
	struct device_node *node;	/* 设备树节点 */
    unsigned int irq;           /* 中断资源 */
    struct timer_list timerOn;  /* 消抖定时器 */
    struct timer_list timerOff; /* 消抖定时器 */
    u64 pressCnt;
    u64 releaseCnt;

    struct cdev cdev;           /* 字符设备 */

    dev_t dev_number;           /* 设备号 */
    struct class *class;        /* 类 */
    struct device *dev_node;    /* 设备节点 */
};

struct key_dev_info g_dev_list[DEVICE_NUM];


static int file_open(struct inode *node, struct file *file)
{
    file->private_data = &g_dev_list[0];
    return 0;
}
static ssize_t file_read(struct file *file, char __user *buf, size_t n, loff_t *offset)
{   
    char data;
    unsigned long ret;
    struct key_dev_info *dev_info = NULL;
    if (file == NULL || file->private_data == NULL) {
        return -1;
    }
    dev_info = file->private_data;
    data = gpiod_get_value(dev_info->gpiod);
    ret = copy_to_user(buf, &data, sizeof(data));
    return sizeof(data) - ret;
}
static ssize_t file_write(struct file *file, const char __user *buf, size_t n, loff_t *offset)
{
    char data;
    unsigned long ret;
    struct key_dev_info *dev_info = NULL;
    if (file == NULL || file->private_data == NULL) {
        return -1;
    }
    dev_info = file->private_data;
    ret = copy_from_user(&data, buf, sizeof(data));
    gpiod_set_value(dev_info->gpiod, !!data);
    return sizeof(data) - ret;
}
static int file_release(struct inode *node, struct file *file)
{
    file->private_data = NULL;
    return 0;
}
struct file_operations op = {
    .owner = THIS_MODULE,
    .read = file_read,
    .write = file_write,
    .open = file_open,
    .release = file_release,
};

// 按键按下 定时器消抖处理
static void timerOnHandler(unsigned long arg)
{
    char data;
    (void)arg;
    data = gpiod_get_value(g_dev_list[0].gpiod);
    if (data) {
        g_dev_list[0].pressCnt++;
        printk("key press   \t%llu\n", g_dev_list[0].pressCnt);
    }
}

// 按键松开 定时器消抖处理
static void timerOffHandler(unsigned long arg)
{
    char data;
    (void)arg;
    data = gpiod_get_value(g_dev_list[0].gpiod);
    if (data == 0) {
        g_dev_list[0].releaseCnt++;
        printk("key release \t%llu\n", g_dev_list[0].pressCnt);
    }
}

// 中断处理函数
static irqreturn_t key_irq_handler(int irq, void *dev)
{
    struct key_dev_info *dev_info = dev;
    char data;
    data = gpiod_get_value(dev_info->gpiod);
    if (data == 0) {
        // 松开按键
        mod_timer(&dev_info->timerOff,
                  jiffies + msecs_to_jiffies(DELAY_TIME_MS));
    } else {
        // 按下按键
        mod_timer(&dev_info->timerOn,
                  jiffies + msecs_to_jiffies(DELAY_TIME_MS));
    }
    return IRQ_HANDLED;
}

// 与设备匹配之后会执行，必须要实现
static int platform_probe(struct platform_device *dev)
{
    int ret = 0;
    dev_t dev_number = 0;
    struct device_node *node = NULL;
    unsigned int irq = 0;
    LOG_TRACE();

    node = of_find_compatible_node(NULL, NULL, "my_key_test");
    irq = irq_of_parse_and_map(node, 0); // index 为 0，表示第一个, 也可以使用 gpiod_to_irq 获取，中断号相同

    // 字符设备
    alloc_chrdev_region(&dev_number, 0, DEVICE_NUM, GPIO_DEV_NAME); // 分配设备号并注册，次设备号为1个
    cdev_init(&g_dev_list[0].cdev, &op); // 初始化 字符设备，绑定 file_operations
    ret = cdev_add(&g_dev_list[0].cdev, dev_number, DEVICE_NUM); // 把字符设备添加到系统中，使用设备号
    // 赋值
    g_dev_list[0].dev_number = dev_number;
    g_dev_list[0].class = class_create(THIS_MODULE, GPIO_DEV_NAME);
    g_dev_list[0].dev_node = device_create(g_dev_list[0].class, NULL, dev_number, NULL, GPIO_DEV_NAME); // 创建设备文件, 使用设备号
    g_dev_list[0].gpiod = gpiod_get(&dev->dev, "key");
    g_dev_list[0].node = node;
    g_dev_list[0].irq = irq;
    g_dev_list[0].pressCnt = 0;
    g_dev_list[0].releaseCnt = 0;
    gpiod_direction_input(g_dev_list[0].gpiod);
    ret = request_irq(irq, key_irq_handler, IRQ_TYPE_EDGE_BOTH, "my_key_irq", &g_dev_list[0]);
    if (ret != 0) {
        printk(" reqest irq error !");
        return ret;
    }
    init_timer(&g_dev_list[0].timerOn); // 初始化定时器
    g_dev_list[0].timerOn.function = timerOnHandler;
    init_timer(&g_dev_list[0].timerOff); // 初始化定时器
    g_dev_list[0].timerOff.function = timerOffHandler;
    return ret;
}
// 移除设备时会执行
static int platform_remove(struct platform_device *dev)
{
    LOG_TRACE();
    del_timer_sync(&g_dev_list[0].timerOn); // 释放定时器资源
    del_timer_sync(&g_dev_list[0].timerOff); // 释放定时器资源
    free_irq(g_dev_list[0].irq, &g_dev_list[0]); // 释放中断资源
    gpiod_put(g_dev_list[0].gpiod); // 释放GPIO
    device_destroy(g_dev_list[0].class, g_dev_list[0].dev_number); // 销毁设备文件, 使用设备号
    class_destroy(g_dev_list[0].class); // 销毁类
    cdev_del(&g_dev_list[0].cdev); // 销毁字符设备
    unregister_chrdev_region(g_dev_list[0].dev_number, DEVICE_NUM); // 反注册设备号
    return 0;
}

// 关闭设备时调用
static void platform_shutdown(struct platform_device *dev)
{
    LOG_TRACE();
}
// 进入睡眠时调用
static int platform_suspend(struct platform_device *dev, pm_message_t state)
{
    LOG_TRACE();
    return 0;
}
// 从睡眠模式恢复时调用
static int platform_resume(struct platform_device *dev)
{
    LOG_TRACE();
    return 0;
}

// 与设备树节点 compatible 匹配
struct of_device_id of_id_table[] = {
    {.compatible = "my_key_test"},
    {}, // 哨兵
};

/* 平台总线驱动 */
struct platform_driver my_platform_deiver = {
    .driver = {
        .name = "gpio_key", // 必须要有，否则可能引发错误
        .of_match_table = of_id_table, // 设备树compatible匹配
    },
    .probe = platform_probe,
    .remove = platform_remove,
    .shutdown = platform_shutdown,
    .suspend = platform_suspend,
    .resume = platform_resume,
    .id_table = NULL,
};

// 加载函数
static int key_driver_init(void)
{
    LOG_TRACE();
    /* 注册平台驱动 */
    return platform_driver_register(&my_platform_deiver);
}
// 卸载函数
static void key_driver_exit(void)
{
    LOG_TRACE();
    /* 注销平台驱动 */
    platform_driver_unregister(&my_platform_deiver);
}

module_init(key_driver_init);
module_exit(key_driver_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("pdg");
MODULE_VERSION("v1.0");



