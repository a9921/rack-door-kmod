#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/err.h>
#include <linux/interrupt.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YUE");
MODULE_DESCRIPTION("Rack door reed switch monitor (GPIO IRQ + debounce + chardev)");
MODULE_VERSION("0.3");

#define GPIO_CHIP_LABEL "pinctrl-rp1"
#define DEV_NAME "rack_door1"
#define CLASS_NAME "rack1"
#define DOOR_GPIO 17

static struct gpio_device *gdev;
static struct gpio_desc *door_desc;

static int door_irq = -1;
static irqreturn_t door_isr(int irq, void *dev_id)
{
  int val;
  val = gpiod_get_value(door_desc);
  if(val < 0)
  {
    pr_err("rack: 電位讀取失敗\n");
    gpio_device_put(gdev);
    return val;
  }
  pr_info("rack: 讀取GPIO: %d, val為:%d %s\n", DOOR_GPIO, val, val?"門開":"門關");
  
  return IRQ_HANDLED;
}


static int __init door_init(void)
{
  pr_info("rack: door_init insmod\n");
  
  int ret;
  
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
  

  return 0;
}

static void __exit door_exit(void)
{
  gpio_device_put(gdev);
  pr_info("rack: door_exit rmmod\n");
}

module_init(door_init);
module_exit(door_exit);