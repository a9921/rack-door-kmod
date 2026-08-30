#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/poll.h>

#include <linux/slab.h>
#include <linux/wait.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YUE");
MODULE_DESCRIPTION("Rack door reed switch monitor (GPIO IRQ + debounce + chardev)");
MODULE_VERSION("1.0");

#define GPIO_CHIP_LABEL "pinctrl-rp1"
#define DEV_NAME "rack_door1"
#define CLASS_NAME "rack1"
#define TMP_LEN 192

static unsigned int door_gpio = 17;
module_param(door_gpio, uint, 0444);
MODULE_PARM_DESC(door_gpio, "rack_door的輸入針腳號");

static struct gpio_device *gdev;
static struct gpio_desc *door_desc;
static unsigned int debounce_ms = 50;
module_param(debounce_ms, uint, 0644);
MODULE_PARM_DESC(debounce_ms, "彈跳的基礎常數");

static ktime_t last_change;
static int door_state = -1;   //-1未知,0關,1開
static unsigned long event_count;
static unsigned long bounce_count;
static unsigned long err_count;
static unsigned long irq_count;

static dev_t dev_num;
static struct cdev door_cdev;
static struct class *door_class;
static struct device *door_device;

static int door_irq = -1;
static struct delayed_work door_work;
static unsigned long work_count;

static wait_queue_head_t door_wq;
static atomic_t door_gen = ATOMIC_INIT(0);

struct door_ctx
{
  int last_gen;
};


static void door_work_fn(struct work_struct *w)
{
  int val;
  ktime_t now = ktime_get();
  
  work_count++;

  val = gpiod_get_value(door_desc);
  if(val < 0)
  {
    pr_warn("rack: 電位讀取失敗\n");
    err_count++;
    return;
  }

  if(val == door_state)
  {
    bounce_count++;
    return;
  }

  event_count++;
  pr_info("rack: 讀取GPIO: %u; 狀態%d %s; 上個狀態維持%lld ms; 事件數: %lu; 彈跳數: %lu\n", door_gpio, val, val?"門開":"門關", ktime_ms_delta(now,  last_change), event_count, bounce_count);
  door_state = val;
  last_change = now;

  atomic_inc(&door_gen);
  wake_up_interruptible(&door_wq);
}

static irqreturn_t door_isr(int irq, void *dev_id)
{
  irq_count++;
  mod_delayed_work(system_wq, &door_work, msecs_to_jiffies(debounce_ms));
  return IRQ_HANDLED;
}

static int door_open(struct inode *inode, struct file *filp)
{
 struct door_ctx *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
 
 if(!ctx)
 {
  return -ENOMEM;
 }
  
 ctx->last_gen = atomic_read(&door_gen);
 filp->private_data = ctx;
 return 0;
}

static int door_release(struct inode *inode, struct file *filp)
{
  kfree(filp->private_data);
  return 0;
}

static __poll_t door_poll(struct file *filp, struct poll_table_struct *wait)
{
  struct door_ctx *ctx = filp->private_data;
  __poll_t mask = 0;

  poll_wait(filp, &door_wq, wait);

  if(atomic_read(&door_gen) != ctx->last_gen)
  {
    mask |= EPOLLIN | EPOLLRDNORM;
  }
  return mask;
}

static ssize_t door_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
  int n;
  char tmp[TMP_LEN];
  struct door_ctx *ctx = filp->private_data;
  int gen = atomic_read(&door_gen);

  if(*off > 0)  //*off(位置))目前讀到第n個位元組
  {
    return 0;
  }
  
  n = scnprintf(tmp, sizeof(tmp), "{\"door_state\": \"%s\", \"door_state_num\": %d, \"event_count\": %lu, \"bounce_count\": %lu, \"since_ms\": %lld}\n", door_state == 1 ?"open": (door_state == 0 ? "closed" : "unknown"), door_state, event_count, bounce_count, ktime_ms_delta(ktime_get(), last_change));
  if(len < (size_t)n)
  {
    return -EINVAL;
  }
  
  if(copy_to_user(buf, tmp, n))
  {
    return -EFAULT;
  }
  
  *off = n;
  ctx->last_gen = gen;

  return n;
}

static const struct file_operations door_fops = {
  .owner = THIS_MODULE,
  .read = door_read,
  .open = door_open,
  .release = door_release,
  .poll = door_poll
  .llseek = default_llseek
};

static int __init door_init(void)
{
  int ret;
  int val;
  int major;
  int minor;

  pr_info("rack: door_init insmod\n");
  
  gdev = gpio_device_find_by_label(GPIO_CHIP_LABEL);
  if(!gdev)
  {
    pr_err("rack: GPIO控制器(rp1)找不到\n");
    return -ENODEV;
  }
  
  door_desc = gpio_device_get_desc(gdev, door_gpio);
  if(IS_ERR(door_desc))
  {
    pr_err("rack: %u找不到\n",door_gpio);
    ret = PTR_ERR(door_desc);
    goto err_put;
  }
  
  ret = gpiod_direction_input(door_desc);
  if(ret)
  {
    pr_err("rack: GPIO設成輸入失敗\n");
    goto err_put;
  }
  
  val = gpiod_get_value(door_desc);
  if(val < 0 )
  {
    pr_err("rack: 電位讀取失敗\n");
  }
  else
  {
    door_state = val;
  }

  last_change = ktime_get();

  door_irq = gpiod_to_irq(door_desc);
  if(door_irq < 0)
  {
    pr_err("rack: 中斷號碼設置失敗\n");
    ret = door_irq;
    goto err_put;
  }

  init_waitqueue_head(&door_wq);
  INIT_DELAYED_WORK(&door_work, door_work_fn);

  ret = request_irq(door_irq, door_isr, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, DEV_NAME, NULL);
  if(ret)
  {
    pr_err("rack: 註冊中斷函式失敗\n");
    goto err_put;
  }

  ret = alloc_chrdev_region(&dev_num, 0, 1, DEV_NAME);
  if(ret < 0)
  {
    pr_err("rack: 字元裝置建制失敗(%d)\n", ret);
    goto err_irq;
  }
  
  cdev_init(&door_cdev, &door_fops);
  door_cdev.owner = THIS_MODULE;

  ret = cdev_add(&door_cdev, dev_num, 1);
  if(ret < 0)
  {
    pr_err("rack: 字元裝置加入失敗\n");
    goto err_region;
  }

  door_class = class_create(CLASS_NAME);
  if(IS_ERR(door_class))
  {
    pr_err("rack: 裝置類別建制失敗\n");
    ret = PTR_ERR(door_class);
    goto err_cdev;
  }

  door_device = device_create(door_class, NULL, dev_num, NULL, DEV_NAME);
  if(IS_ERR(door_device))
  {
    pr_err("rack: 裝置節點建立失敗\n");
    ret = PTR_ERR(door_device);
    goto err_class;
  }  
  
  major = MAJOR(dev_num);
  minor = MINOR(dev_num);
  pr_info("rack: 讀取GPIO: %u, val為:%d %s, irq: %d, major:%d, minor:%d\n", door_gpio, val, val?"門開":"門關", door_irq, major, minor);
  return 0;

err_class:
  class_destroy(door_class);
err_cdev: 
  cdev_del(&door_cdev);
err_region:
  unregister_chrdev_region(dev_num, 1);
err_irq:
  free_irq(door_irq, NULL);
  cancel_delayed_work_sync(&door_work);
err_put:
  gpio_device_put(gdev);
  return ret;
}

static void __exit door_exit(void)
{
  device_destroy(door_class, dev_num);
  class_destroy(door_class);
  cdev_del(&door_cdev);
  unregister_chrdev_region(dev_num, 1);

  if(door_irq >= 0)
  {
    free_irq(door_irq, NULL);
  }

  cancel_delayed_work_sync(&door_work);

  gpio_device_put(gdev);


    pr_info("rack: door_exit rmmod； debounce=%u ms； 硬體邊緣 %lu； 去彈跳後處理 %lu（合併掉 %lu）； 事件 %lu + 彈跳 %lu + 失敗 %lu； 差值 %ld\n",
        debounce_ms,
        irq_count,
        work_count, irq_count - work_count,
        event_count, bounce_count, err_count,
        (long)work_count - (long)event_count - (long)bounce_count - (long)err_count);
}

module_init(door_init);
module_exit(door_exit);