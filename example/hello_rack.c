// hello_rack.c
// 用途：驗證 RPi5 上的 kernel module 編譯環境是否可用。
// 這支模組不碰任何硬體，只在載入和卸載時各印一行訊息到核心日誌。
// 目的是把「環境問題」和「程式問題」分開 ——
// 先確定這支能編、能載入、能看到訊息，之後寫真正的 GPIO 模組
// 出錯時，才能確定是程式的問題而不是環境的問題。

#include <linux/init.h>     // __init / __exit 標記
#include <linux/module.h>   // 所有核心模組都要
#include <linux/kernel.h>   // pr_info()

// 這四行是模組的中繼資料，載入後可以用 modinfo 查到。
// MODULE_LICENSE 必填，且必須是 GPL 相容的授權，
// 否則核心會標記你的模組「污染核心（tainted）」並拒絕使用部分 API。
MODULE_LICENSE("GPL");
MODULE_AUTHOR("YUE");
MODULE_DESCRIPTION("Rack security module - build environment test");
MODULE_VERSION("0.1");

// __init 表示「這段程式只在載入時執行一次」，
// 執行完核心會把這塊記憶體釋放掉，省下常駐空間。
static int __init hello_rack_init(void)
{
    pr_info("rack: module loaded\n");
    return 0;   // 回傳 0 代表載入成功；回傳負數代表失敗，insmod 會報錯
}

// __exit 表示「這段程式只在卸載時執行」。
static void __exit hello_rack_exit(void)
{
    pr_info("rack: module unloaded\n");
}

// 告訴核心：載入時呼叫哪個函式、卸載時呼叫哪個函式。
module_init(hello_rack_init);
module_exit(hello_rack_exit);