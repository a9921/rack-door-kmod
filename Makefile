# 這個 Makefile 同時建置兩支模組。
# obj-m 可以列多個目標，每個目標會各自產生一個 .ko 檔。
#obj-m += hello_rack.o
#obj-m += door_irq.o
obj-m += rack_door.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 常用指令備忘：
#   make                          編譯，產生 hello_rack.ko 與 door_irq.ko
#   sudo insmod door_irq.ko       載入（可加參數：door_gpio=17 debounce_ms=50）
#   sudo dmesg -w                 即時監看核心日誌，Ctrl+C 離開
#   lsmod | grep door_irq         確認已載入
#   cat /proc/interrupts | grep rack_door    看中斷被觸發幾次
#   sudo rmmod door_irq           卸載
#   modinfo door_irq.ko           查看模組資訊與可用參數
