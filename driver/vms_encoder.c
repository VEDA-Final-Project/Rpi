#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "vms_encoder"
#define GPIO_CLK 5
#define GPIO_DT 6
#define GPIO_SW 26

static int major_number;
static int event_val = 0; // 1: CW, -1: CCW, 100: SW
static int last_clk = -1;

// 인터럽트 핸들러: 회전 감지
static irqreturn_t enc_irq_handler(int irq, void *dev_id) {
    int clk = gpio_get_value(GPIO_CLK);
    if (clk != last_clk) {
        if (gpio_get_value(GPIO_DT) != clk) event_val = 1;  // 시계
        else event_val = -1; // 반시계
    }
    last_clk = clk;
    return IRQ_HANDLED;
}

// 인터럽트 핸들러: 스위치 클릭
static irqreturn_t sw_irq_handler(int irq, void *dev_id) {
    event_val = 100;
    return IRQ_HANDLED;
}

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    if (event_val == 0) return 0;
    if (copy_to_user(buffer, &event_val, sizeof(int))) return -EFAULT;
    event_val = 0; // 읽었으니 초기화
    return sizeof(int);
}

static struct file_operations fops = { .read = dev_read };

static int __init vms_init(void) {
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    gpio_request(GPIO_CLK, "CLK"); gpio_request(GPIO_DT, "DT"); gpio_request(GPIO_SW, "SW");
    gpio_direction_input(GPIO_CLK); gpio_direction_input(GPIO_DT); gpio_direction_input(GPIO_SW);
    
    request_irq(gpio_to_irq(GPIO_CLK), enc_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "vms_enc", NULL);
    request_irq(gpio_to_irq(GPIO_SW), sw_irq_handler, IRQF_TRIGGER_FALLING, "vms_sw", NULL);
    
    printk(KERN_INFO "VMS Driver Loaded. Major: %d\n", major_number);
    return 0;
}

static void __exit vms_exit(void) {
    free_irq(gpio_to_irq(GPIO_CLK), NULL); free_irq(gpio_to_irq(GPIO_SW), NULL);
    gpio_free(GPIO_CLK); gpio_free(GPIO_DT); gpio_free(GPIO_SW);
    unregister_chrdev(major_number, DEVICE_NAME);
}

module_init(vms_init); module_exit(vms_exit);
MODULE_LICENSE("GPL");