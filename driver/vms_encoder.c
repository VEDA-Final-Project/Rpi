#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h> // ★ kfifo 헤더 추가

#define DEVICE_NAME "vms_encoder"

// 라즈베리 파이 4 (Base 512) 기준 핀 매핑
#define GPIO_CLK 517
#define GPIO_DT  518
#define GPIO_SW  538

// ★ 32개의 이벤트를 저장할 수 있는 큐 생성 (크기는 반드시 2의 거듭제곱)
#define FIFO_SIZE 32
static DECLARE_KFIFO(enc_fifo, int, FIFO_SIZE);

static int major_number;
static int last_clk = -1;
static int irq_clk, irq_sw;
static unsigned long last_sw_jiffies = 0;

static struct class *vms_class = NULL;
static struct device *vms_device = NULL;

static DECLARE_WAIT_QUEUE_HEAD(enc_wait_queue);
static DEFINE_SPINLOCK(vms_lock);

// --- [인터럽트] 로터리 엔코더 회전 ---
static irqreturn_t enc_irq_handler(int irq, void *dev_id) {
    unsigned long flags;
    int clk = gpio_get_value(GPIO_CLK);
    int val;
    
    if (clk != last_clk) {
        val = (gpio_get_value(GPIO_DT) != clk) ? 1 : -1;
        
        // ★ 스핀락을 걸고 큐에 데이터 푸시 (Event Loss 완벽 방어)
        spin_lock_irqsave(&vms_lock, flags);
        if (!kfifo_is_full(&enc_fifo)) {
            kfifo_put(&enc_fifo, val);
        }
        spin_unlock_irqrestore(&vms_lock, flags);
        
        wake_up_interruptible(&enc_wait_queue);
    }
    last_clk = clk;
    return IRQ_HANDLED;
}

// --- [인터럽트] 스위치 클릭 ---
static irqreturn_t sw_irq_handler(int irq, void *dev_id) {
    unsigned long flags;
    int val = 100;
    
    if (time_after(jiffies, last_sw_jiffies + msecs_to_jiffies(250))) {
        // ★ 큐에 스위치 이벤트 푸시
        spin_lock_irqsave(&vms_lock, flags);
        if (!kfifo_is_full(&enc_fifo)) {
            kfifo_put(&enc_fifo, val);
        }
        spin_unlock_irqrestore(&vms_lock, flags);
        
        wake_up_interruptible(&enc_wait_queue);
        last_sw_jiffies = jiffies;
    }
    return IRQ_HANDLED;
}

// --- [파일 연산] 데이터 읽기 ---
static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    int ret;
    int current_val;
    unsigned long flags;

    // ★ 큐가 비어있으면 대기 (Blocking / Non-blocking 처리)
    if (kfifo_is_empty(&enc_fifo)) {
        if (filep->f_flags & O_NONBLOCK) return -EAGAIN;
        
        ret = wait_event_interruptible(enc_wait_queue, !kfifo_is_empty(&enc_fifo));
        if (ret) return ret;
    }

    // ★ 큐에서 데이터 팝 (Get)
    spin_lock_irqsave(&vms_lock, flags);
    ret = kfifo_get(&enc_fifo, &current_val);
    spin_unlock_irqrestore(&vms_lock, flags);

    // 데이터를 성공적으로 꺼냈다면 유저 스페이스로 복사
    if (ret) {
        if (copy_to_user(buffer, &current_val, sizeof(int))) {
            return -EFAULT;
        }
        return sizeof(int);
    }
    
    return 0; 
}

// --- [파일 연산] 이벤트 폴링 ---
static __poll_t dev_poll(struct file *filep, poll_table *wait) {
    __poll_t mask = 0;
    poll_wait(filep, &enc_wait_queue, wait);
    
    // 큐에 데이터가 있으면 읽기 가능 상태로 전환
    if (!kfifo_is_empty(&enc_fifo)) {
        mask |= EPOLLIN | EPOLLRDNORM;
    }
    return mask;
}

static struct file_operations fops = { 
    .read = dev_read,
    .poll = dev_poll 
};

// --- [초기화] 모듈 적재 ---
static int __init vms_init(void) {
    int ret;
    
    INIT_KFIFO(enc_fifo); // ★ kfifo 초기화
    
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    vms_class = class_create("vms_class");
    if (IS_ERR(vms_class)) {
        ret = PTR_ERR(vms_class);
        goto err_class;
    }
    
    vms_device = device_create(vms_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(vms_device)) {
        ret = PTR_ERR(vms_device);
        goto err_device;
    }

    ret = gpio_request(GPIO_CLK, "VMS_CLK");
    if (ret) goto err_gpio_clk;
    
    ret = gpio_request(GPIO_DT, "VMS_DT");
    if (ret) goto err_gpio_dt;
    
    ret = gpio_request(GPIO_SW, "VMS_SW");
    if (ret) goto err_gpio_sw;

    gpio_direction_input(GPIO_CLK);
    gpio_direction_input(GPIO_DT);
    gpio_direction_input(GPIO_SW);

    irq_clk = gpio_to_irq(GPIO_CLK);
    ret = request_irq(irq_clk, enc_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "vms_irq_enc", NULL);
    if (ret) goto err_irq_clk;
    
    irq_sw = gpio_to_irq(GPIO_SW);
    ret = request_irq(irq_sw, sw_irq_handler, IRQF_TRIGGER_FALLING, "vms_irq_sw", NULL);
    if (ret) goto err_irq_sw;

    printk(KERN_INFO "VMS Driver Loaded! (kfifo enabled)\n");
    return 0;

err_irq_sw:
    free_irq(irq_clk, NULL);
err_irq_clk:
    gpio_free(GPIO_SW);
err_gpio_sw:
    gpio_free(GPIO_DT);
err_gpio_dt:
    gpio_free(GPIO_CLK);
err_gpio_clk:
    device_destroy(vms_class, MKDEV(major_number, 0));
err_device:
    class_destroy(vms_class);
err_class:
    unregister_chrdev(major_number, DEVICE_NAME);
    return ret;
}

// --- [종료] 모듈 제거 ---
static void __exit vms_exit(void) {
    free_irq(irq_sw, NULL);
    free_irq(irq_clk, NULL);
    gpio_free(GPIO_SW); 
    gpio_free(GPIO_DT); 
    gpio_free(GPIO_CLK);
    
    device_destroy(vms_class, MKDEV(major_number, 0));
    class_destroy(vms_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "VMS Driver Unloaded!\n");
}

module_init(vms_init); 
module_exit(vms_exit);
MODULE_LICENSE("GPL");