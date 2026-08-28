# 這個 Makefile 建置 rack_door 模組。
# obj-m 可以列多個目標，每個目標會各自產生一個 .ko 檔。
obj-m += rack_door.o

KDIR    := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)
EXTRADIR := /lib/modules/$(shell uname -r)/extra
MODULE  := rack_door

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 安裝到系統目錄，供開機自動載入使用
install: all
	sudo mkdir -p $(EXTRADIR)
	sudo cp -a $(MODULE).ko $(EXTRADIR)/
	sudo depmod -a
	@echo "已安裝到 $(EXTRADIR)/$(MODULE).ko"

# 重新載入：rmmod 會觸發 BindsTo 停掉 service，modprobe 後 udev 會自動拉回
reload: install
	-sudo rmmod $(MODULE)
	sudo modprobe $(MODULE)
	@sleep 2
	@systemctl is-active rack-monitor.service

uninstall:
	-sudo rmmod $(MODULE)
	sudo rm -f $(EXTRADIR)/$(MODULE).ko
	sudo depmod -a

.PHONY: all clean install reload uninstall

# 常用指令備忘：
#   make                          編譯，產生 rack_door.ko
#   make install                  編譯並安裝到 /lib/modules/.../extra/
#   make reload                   編譯、安裝、重新載入（service 會自動重啟）
#   sudo dmesg -w                 即時監看核心日誌，Ctrl+C 離開
#   lsmod | grep rack_door        確認已載入
#   cat /proc/interrupts | grep rack_door    看中斷被觸發幾次
#   modinfo rack_door.ko          查看模組資訊與可用參數
#   journalctl -u rack-monitor.service -f    看 agent 即時 log