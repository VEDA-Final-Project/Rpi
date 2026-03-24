#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>       // poll 지원을 위해 추가
#include <linux/wait.h>       // 대기 큐 지원을 위해 추가
#include <linux/jiffies.h> 

#define DEVICE_NAME "vms_encoder"

// ★ BCM 5, 6, 13 에 맞게 수정 (Base 571 기준)
#define GPIO_CLK 517
#define GPIO_DT  518
#define GPIO_SW  538

static int major_number;
static int event_val = 0;
static int last_clk = -1;
static int irq_clk, irq_sw;
static unsigned long last_sw_jiffies = 0; // 디바운싱용 변수 추가

// Qt QSocketNotifier에게 데이터 수신을 알리기 위한 대기 큐
static DECLARE_WAIT_QUEUE_HEAD(enc_wait_queue);

// 회전 감지 인터럽트
static irqreturn_t enc_irq_handler(int irq, void *dev_id) {
    int clk = gpio_get_value(GPIO_CLK);
    if (clk != last_clk) {
        event_val = (gpio_get_value(GPIO_DT) != clk) ? 1 : -1;
        wake_up_interruptible(&enc_wait_queue); // 데이터가 준비되었음을 Qt에 알림
    }
    last_clk = clk;
    return IRQ_HANDLED;
}

// 스위치 클릭 인터럽트
static irqreturn_t sw_irq_handler(int irq, void *dev_id) {
    // 250ms 디바운싱 (기존 Qt 코드의 swTimer.elapsed() > 250 역할)
    if (time_after(jiffies, last_sw_jiffies + msecs_to_jiffies(250))) {
        event_val = 100;
        wake_up_interruptible(&enc_wait_queue);
        last_sw_jiffies = jiffies;
    }
    return IRQ_HANDLED;
}

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    int ret;
    int current_val;

    // 데이터가 없을 때의 처리
    if (event_val == 0) {
        // Non-blocking 모드라면 (Qt에서 열었을 때)
        if (filep->f_flags & O_NONBLOCK) {
            return -EAGAIN;
        }
        
        // Blocking 모드라면 (cat 명령어 등으로 열었을 때), 데이터가 올 때까지 대기
        ret = wait_event_interruptible(enc_wait_queue, event_val != 0);
        if (ret) {
            return ret; // 시그널 등에 의해 인터럽트됨
        }
    }

    // 대기하다가 깨어났거나 이미 데이터가 있는 경우
    current_val = event_val;
    event_val = 0; // 읽었으니 초기화

    if (copy_to_user(buffer, &current_val, sizeof(int))) {
        return -EFAULT;
    }

    return sizeof(int);
}

// ★ Qt의 QSocketNotifier가 작동하기 위한 poll 함수 구현
static __poll_t dev_poll(struct file *filep, poll_table *wait) {
    __poll_t mask = 0;
    poll_wait(filep, &enc_wait_queue, wait);
    
    // event_val에 값이 있으면 읽기 가능(POLLIN) 상태로 마킹
    if (event_val != 0) {
        mask |= EPOLLIN | EPOLLRDNORM;
    }
    return mask;
}

// fops에 poll 추가
static struct file_operations fops = { 
    .read = dev_read,
    .poll = dev_poll 
};

static int __init vms_init(void) {
    int ret;
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    // GPIO 요청
    gpio_request(GPIO_CLK, "VMS_CLK");
    gpio_request(GPIO_DT, "VMS_DT");
    gpio_request(GPIO_SW, "VMS_SW");

    gpio_direction_input(GPIO_CLK);
    gpio_direction_input(GPIO_DT);
    gpio_direction_input(GPIO_SW);

    // 인터럽트 등록
    irq_clk = gpio_to_irq(GPIO_CLK);
    ret = request_irq(irq_clk, enc_irq_handler, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING, "vms_irq_enc", NULL);
    
    irq_sw = gpio_to_irq(GPIO_SW);
    ret = request_irq(irq_sw, sw_irq_handler, IRQF_TRIGGER_FALLING, "vms_irq_sw", NULL);

    printk(KERN_INFO "VMS Driver Success! Pins: %d, %d, %d\n", GPIO_CLK, GPIO_DT, GPIO_SW);
    return 0;
}

static void __exit vms_exit(void) {
    free_irq(irq_clk, NULL);
    free_irq(irq_sw, NULL);
    gpio_free(GPIO_CLK); gpio_free(GPIO_DT); gpio_free(GPIO_SW);
    unregister_chrdev(major_number, DEVICE_NAME);
}

module_init(vms_init); module_exit(vms_exit);
MODULE_LICENSE("GPL");