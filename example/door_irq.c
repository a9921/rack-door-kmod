// door_irq.c  v0.3

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/interrupt.h>      // request_irq() / free_irq() / IRQF_*
#include <linux/ktime.h>          // ktime_get() / ktime_ms_delta()
#include <linux/gpio/consumer.h>  // gpiod_* 這一組
#include <linux/gpio/driver.h>    // gpio_device_find_by_label() / gpio_device_get_desc()
#include <linux/fs.h>             // file_operations / alloc_chrdev_region()
#include <linux/cdev.h>           // cdev_init() / cdev_add()
#include <linux/device.h>         // class_create() / device_create()
#include <linux/uaccess.h>        // copy_to_user()

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YUE");
MODULE_DESCRIPTION("Rack door reed switch monitor (GPIO IRQ + debounce + chardev)");
MODULE_VERSION("0.3");

// 樹莓派 5 的 40 針排針由 RP1 這顆 I/O 晶片管理，
// 它在核心裡的名稱固定是 pinctrl-rp1（用 gpiodetect 可以查到）。
#define GPIO_CHIP_LABEL "pinctrl-rp1"
#define DEV_NAME        "rack_door"      // 會變成 /dev/rack_door
#define CLASS_NAME      "rack"

// ---------------- 可調參數 ----------------
// 這些值可以在 insmod 時指定，不必改程式重編：
//   sudo insmod door_irq.ko door_gpio=27 debounce_ms=200

static unsigned int door_gpio = 17;
module_param(door_gpio, uint, 0444);
MODULE_PARM_DESC(door_gpio, "pinctrl-rp1 上的線號（預設 17）");

static unsigned int debounce_ms = 50;
module_param(debounce_ms, uint, 0644);
MODULE_PARM_DESC(debounce_ms, "去彈跳時間窗，單位毫秒（預設 50）");

// ---------------- GPIO 與中斷 ----------------

static struct gpio_device *gdev;
static struct gpio_desc   *door_desc;
static int door_irq = -1;

static ktime_t       last_irq;          // 上一次通過時間窗的中斷 → 去彈跳用
static ktime_t       last_change;       // 上一次真正的狀態改變   → 回報用
static int           door_state = -1;   // -1 未知，0 關好，1 打開
static unsigned long event_count;
static unsigned long bounce_count;

// ---------------- 字元裝置 ----------------

static dev_t          dev_num;
static struct cdev    door_cdev;
static struct class  *door_class;
static struct device *door_device;

static const char *state_text(int level)
{
    return level ? "open" : "closed";
}

// ---------------- 中斷處理常式 ----------------
//
// 這個函式由硬體中斷觸發，執行在「不可睡眠」的環境中。
// 規則：越短越好，不能呼叫任何可能睡眠的函式
//      （kmalloc 加 GFP_KERNEL、mutex、sleep 這些都不行）。

static irqreturn_t door_isr(int irq, void *dev_id)
{
    ktime_t now = ktime_get();
    s64 delta_ms = ktime_ms_delta(now, last_irq);
    int level;

    // 第一層：時間窗過濾。
    // 機械接點閉合的瞬間會在幾毫秒內彈跳好幾次，
    // 每一次彈跳都是一個真實的電位邊緣，硬體會老實地全部通知我們。
    // 距離上次接受的中斷太近，一律視為彈跳丟掉。
    if (delta_ms < (s64)debounce_ms) {
        bounce_count++;
        return IRQ_HANDLED;
    }
    last_irq = now;

    // 第二層：重讀實際電位。
    // 不用「上次是 0 這次就是 1」這種推論，而是直接問腳位現在是幾。
    // 這樣就算前面漏掉或多算了幾次邊緣，狀態也會自己校正回來。
    level = gpiod_get_value(door_desc);
    if (level < 0) {
        pr_warn("rack: read gpio failed (%d)\n", level);
        return IRQ_HANDLED;
    }

    // 電位跟上次一樣，代表這是一串彈跳的尾巴，不是真的狀態改變。
    if (level == door_state) {
        bounce_count++;
        return IRQ_HANDLED;
    }

    // 確定是真的狀態改變了
    pr_info("rack: door %-6s | held=%lldms events=%lu bounces=%lu\n",
            state_text(level),
            ktime_ms_delta(now, last_change),   // 上一個狀態維持了多久
            event_count + 1, bounce_count);

    door_state  = level;
    last_change = now;
    event_count++;

    return IRQ_HANDLED;
}

// ---------------- 字元裝置的讀取 ----------------
//
// 使用者空間執行 cat /dev/rack_door 時，核心會呼叫到這裡。
//
// 兩個必須注意的地方：
//   1. 使用者空間的記憶體不能直接用指標寫入，一定要透過 copy_to_user()。
//      核心和使用者程式的位址空間是分開的，直接寫會造成安全漏洞或當機。
//   2. 要處理 *off（檔案位移）。cat 會一直讀到某次回傳 0 才停，
//      沒處理的話它會無限讀下去，畫面會被洗版。

static ssize_t door_read(struct file *filp, char __user *buf,
                         size_t len, loff_t *off)
{
    char tmp[192];
    int n;

    if (*off > 0)
        return 0;               // 第二次讀就回報「檔案結束」

    n = scnprintf(tmp, sizeof(tmp),
                  "{\"door\":\"%s\",\"state\":%d,\"events\":%lu,"
                  "\"bounces\":%lu,\"since_ms\":%lld}\n",
                  state_text(door_state), door_state,
                  event_count, bounce_count,
                  ktime_ms_delta(ktime_get(), last_change));

    if (len < (size_t)n)
        return -EINVAL;

    if (copy_to_user(buf, tmp, n))
        return -EFAULT;

    *off = n;
    return n;
}

static int door_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int door_release(struct inode *inode, struct file *filp)
{
    return 0;
}

// 這張表告訴核心：對這個裝置檔做各種操作時，要呼叫哪個函式。
// 沒填的操作（write、ioctl 等）使用者空間去做會得到錯誤，這是我們要的
// —— 門的狀態是唯讀的，不該讓外部程式寫入。
static const struct file_operations door_fops = {
    .owner   = THIS_MODULE,
    .open    = door_open,
    .read    = door_read,
    .release = door_release,
};

// ---------------- 載入 ----------------

static int __init door_init(void)
{
    int ret;

    // ===== 第一部分：取得 GPIO 並註冊中斷 =====

    // 依名稱找到 RP1 這顆 GPIO 控制器。
    // 這個呼叫會增加一個參考計數，之後一定要用 gpio_device_put() 還回去。
    gdev = gpio_device_find_by_label(GPIO_CHIP_LABEL);
    if (!gdev) {
        pr_err("rack: gpio chip '%s' not found\n", GPIO_CHIP_LABEL);
        return -ENODEV;
    }

    door_desc = gpio_device_get_desc(gdev, door_gpio);
    if (IS_ERR(door_desc)) {
        ret = PTR_ERR(door_desc);
        pr_err("rack: get desc for line %u failed (%d)\n", door_gpio, ret);
        door_desc = NULL;
        goto err_put;
    }

    ret = gpiod_direction_input(door_desc);
    if (ret) {
        pr_err("rack: set direction input failed (%d)\n", ret);
        goto err_put;
    }

    // 有些 GPIO 控制器內建硬體去彈跳。RP1 不一定支援，失敗不算錯，
    // 因為我們本來就有軟體那一層。
    ret = gpiod_set_debounce(door_desc, debounce_ms * 1000);  // 參數單位是微秒
    if (ret)
        pr_info("rack: hardware debounce unavailable (%d), software only\n", ret);
    else
        pr_info("rack: hardware debounce enabled (%u ms)\n", debounce_ms);

    door_irq = gpiod_to_irq(door_desc);
    if (door_irq < 0) {
        ret = door_irq;
        pr_err("rack: gpiod_to_irq failed (%d)\n", ret);
        door_irq = -1;
        goto err_put;
    }

    // 記錄起始狀態，避免載入後第一次中斷把「未知」誤判成變化。
    door_state  = gpiod_get_value(door_desc);
    last_irq    = ktime_get();
    last_change = last_irq;

    // RISING | FALLING 代表上升緣和下降緣都要通知 ——
    // 門打開和門關上都要抓到，只抓一邊會漏掉另一半。
    ret = request_irq(door_irq, door_isr,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                      DEV_NAME, NULL);
    if (ret) {
        pr_err("rack: request_irq %d failed (%d)\n", door_irq, ret);
        goto err_put;
    }

    // ===== 第二部分：建立字元裝置 =====

    // 1. 跟核心要一組裝置編號。
    //    傳 0 代表「主編號由核心自己分配」，比寫死一個數字安全，
    //    不會跟系統上其他裝置撞號。
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEV_NAME);
    if (ret) {
        pr_err("rack: alloc_chrdev_region failed (%d)\n", ret);
        goto err_irq;
    }

    // 2. 把操作函式表跟裝置編號綁在一起，並向核心註冊。
    cdev_init(&door_cdev, &door_fops);
    door_cdev.owner = THIS_MODULE;
    ret = cdev_add(&door_cdev, dev_num, 1);
    if (ret) {
        pr_err("rack: cdev_add failed (%d)\n", ret);
        goto err_region;
    }

    // 3. 建立裝置類別，再建立裝置節點。
    //    這兩步會讓 udev 自動在 /dev/ 底下產生 rack_door 這個檔案，
    //    不必手動 mknod。
    //    注意：核心 6.4 之後 class_create() 只收一個參數，
    //    網路上很多教學還寫成兩個參數（多一個 THIS_MODULE），那是舊寫法。
    door_class = class_create(CLASS_NAME);
    if (IS_ERR(door_class)) {
        ret = PTR_ERR(door_class);
        pr_err("rack: class_create failed (%d)\n", ret);
        goto err_cdev;
    }

    door_device = device_create(door_class, NULL, dev_num, NULL, DEV_NAME);
    if (IS_ERR(door_device)) {
        ret = PTR_ERR(door_device);
        pr_err("rack: device_create failed (%d)\n", ret);
        goto err_class;
    }

    pr_info("rack: watching %s line %u, irq %d, debounce %ums\n",
            GPIO_CHIP_LABEL, door_gpio, door_irq, debounce_ms);
    pr_info("rack: /dev/%s ready (major %d, minor %d)\n",
            DEV_NAME, MAJOR(dev_num), MINOR(dev_num));
    pr_info("rack: initial state = %s\n", state_text(door_state));
    return 0;

// 錯誤處理採階梯式回收：失敗在哪一層，就從那一層開始往回拆。
// 這是核心程式的標準寫法，可以確保任何一步失敗都不會漏掉資源。
err_class:
    class_destroy(door_class);
err_cdev:
    cdev_del(&door_cdev);
err_region:
    unregister_chrdev_region(dev_num, 1);
err_irq:
    free_irq(door_irq, NULL);
    door_irq = -1;
err_put:
    gpio_device_put(gdev);
    gdev = NULL;
    return ret;
}

// ---------------- 卸載 ----------------

static void __exit door_exit(void)
{
    // 釋放順序要跟取得順序完全相反。
    // 順序反了的話，中斷還可能在資源被回收後被觸發，會直接當機。
    device_destroy(door_class, dev_num);
    class_destroy(door_class);
    cdev_del(&door_cdev);
    unregister_chrdev_region(dev_num, 1);

    if (door_irq >= 0)
        free_irq(door_irq, NULL);

    if (gdev)
        gpio_device_put(gdev);

    pr_info("rack: unloaded | total events=%lu bounces=%lu\n",
            event_count, bounce_count);
}

module_init(door_init);
module_exit(door_exit);