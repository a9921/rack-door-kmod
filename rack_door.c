#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/err.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("YUE");
MODULE_DESCRIPTION("Rack door reed switch monitor (GPIO IRQ + debounce + chardev)");
MODULE_VERSION("0.2");

#define GPIO_CHIP_LABEL "pinctrl-rp1"
#define DEV_NAME "rack_door1"
#define CLASS_NAME "rack1"
#define DOOR_GPIO 17

static struct gpio_device *gdev;
static struct gpio_desc *door_desc;




static int __init door_init(void)
{
  pr_info("door_init insmod\n");
  
  int ret = gpiod_direction_input(door_desc);
  int val = gpiod_get_value(door_desc);

  gdev = gpio_device_find_by_label(GPIO_CHIP_LABEL);
  if(!gdev)
  {
    pr_err("GPIO控制器(rp1)找不到\n");
    return -ENODEV;
  }

  door_desc = gpio_device_get_desc(gdev, DOOR_GPIO);
  if(IS_ERR(door_desc))
  {
    pr_err("%d找不到\n",DOOR_GPIO);
    gpio_device_put(gdev);
    return PTR_ERR(door_desc);
  }

  if(ret)
  {
    pr_err("GPIO設成輸入失敗\n");
    gpio_device_put(gdev);
    return ret;
  }

  if(val < 0)
  {
    pr_err("電位讀取失敗\n");
    gpio_device_put(gdev);
    return val;
  }

  pr_info("讀取GPIO: %u, val為: %d \n", door_desc, val);
  return 0;
}

static void __exit door_exit(void)
{
  gpio_device_put(gdev);
  pr_info("door_exit rmmod\n");
}

module_init(door_init);
module_exit(door_exit);