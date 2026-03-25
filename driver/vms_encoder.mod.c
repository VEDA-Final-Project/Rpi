#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xdf484963, "device_destroy" },
	{ 0x6775d5d3, "class_destroy" },
	{ 0xc1514a3b, "free_irq" },
	{ 0xfe990052, "gpio_free" },
	{ 0x6bc3fbc0, "__unregister_chrdev" },
	{ 0x92997ed8, "_printk" },
	{ 0x15ba50a6, "jiffies" },
	{ 0xe2964344, "__wake_up" },
	{ 0xfe487975, "init_wait_entry" },
	{ 0x1000e51, "schedule" },
	{ 0x8c26d495, "prepare_to_wait_event" },
	{ 0x92540fbf, "finish_wait" },
	{ 0x6cbbfc54, "__arch_copy_to_user" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xa0c562db, "gpio_to_desc" },
	{ 0x3bdfd684, "gpiod_direction_input" },
	{ 0xcf6b2d6, "gpiod_get_raw_value" },
	{ 0x418c10ec, "__register_chrdev" },
	{ 0x59c02473, "class_create" },
	{ 0xb63fdcdb, "device_create" },
	{ 0x47229b5c, "gpio_request" },
	{ 0x9c62fdb0, "gpiod_to_irq" },
	{ 0x92d5838e, "request_threaded_irq" },
	{ 0x474e54d2, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "6AC9F22DE7C97C25D89AC0F");
