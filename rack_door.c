#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YUE");
MODULE_DESCRIPTION("Rack door reed switch monitor (GPIO IRQ + debounce + chardev)");
MODULE_VERSION("0.4");

#define GPIO_CHIP_LABEL "pinctrl-rp1"
#define DEV_NAME "rack_door1"
#define CLASS_NAME "rack1"
#define DOOR_GPIO 17

static struct gpio_device *gdev;
static struct gpio_desc *door_desc;
static unsigned int debounce_ms = 50;
static ktime_t last_irq;
static ktime_t last_change;
static int door_state = -1;   //-1未知,0關,1開
static unsigned long event_count;
static unsigned long bounce_count;

static int door_irq = -1;
static irqreturn_t door_isr(int irq, void *dev_id)
{
  int val;
  
  ktime_t now = ktime_get();
  s64 delta_ms = ktime_ms_delta(now, last_irq);
  
  if(delta_ms < (s64)debounce_ms)   //如果變換時間(s64)小於(32)正常人類手速，視為彈跳
  {
    bounce_count++;
    return IRQ_HANDLED;
  }
  last_irq = now;
  
  val = gpiod_get_value(door_desc);
  if(val < 0)
  {
    pr_warn("rack: 電位讀取失敗\n");
    return IRQ_HANDLED;
  }

  if(val == door_state)
  {
    bounce_count++;
    return IRQ_HANDLED;
  }
  
  pr_info("rack: 讀取GPIO: %d； 狀態%d %s； 上個狀態維持%lld； 事件數: %lu； 彈跳數: %lu\n", DOOR_GPIO, val, val?"門開":"門關", ktime_ms_delta(now, last_change), event_count, bounce_count);
  door_state = val;
  last_change = now;
  event_count++;

  return IRQ_HANDLED;
}


static int __init door_init(void)
{
  int ret;
  int req;
  int val;

  pr_info("rack: door_init insmod\n");
  
  gdev = gpio_device_find_by_label(GPIO_CHIP_LABEL);
  if(!gdev)
  {
    pr_err("rack: GPIO控制器(rp1)找不到\n");
    return -ENODEV;
  }
  
  door_desc = gpio_device_get_desc(gdev, DOOR_GPIO);
  if(IS_ERR(door_desc))
  {
    pr_err("rack: %d找不到\n",DOOR_GPIO);
    gpio_device_put(gdev);
    return PTR_ERR(door_desc);
  }
  
  ret = gpiod_direction_input(door_desc);
  if(ret)
  {
    pr_err("rack: GPIO設成輸入失敗\n");
    gpio_device_put(gdev);
    return ret;
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
  last_irq = ktime_get();
  last_change = ktime_get();

  door_irq = gpiod_to_irq(door_desc);
  if(door_irq < 0)
  {
    pr_err("rack: 中斷號碼設置失敗\n");
    gpio_device_put(gdev);
    return door_irq;
  }

  req = request_irq(door_irq, door_isr, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, DEV_NAME, NULL);
  if(req)
  {
    pr_err("rack: 註冊中斷函式失敗\n");
    gpio_device_put(gdev);
    return req;
  }

  pr_info("rack: 讀取GPIO: %d, val為:%d %s, irq: %d\n", DOOR_GPIO, val, val?"門開":"門關", door_irq);
  return 0;
}

static void __exit door_exit(void)
{
  if(door_irq >= 0)
  {
    free_irq(door_irq, NULL);
  }
  gpio_device_put(gdev);
  pr_info("rack: door_exit rmmod\n");
}

module_init(door_init);
module_exit(door_exit);